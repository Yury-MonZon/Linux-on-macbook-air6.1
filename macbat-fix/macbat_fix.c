// SPDX-License-Identifier: GPL-2.0
/*
 * macbat_fix - corrected real-time battery reporting for MacBookAir6,1
 *
 * Apple's stock _BST method reports a power value (mA*mV/1000) into the
 * field the kernel now reads as current (a side effect of _BIX declaring
 * mA-based Power Unit), and multiplies RemainingCapacity by a fixed x10
 * left over from the old _BIF mWh approximation. This driver bypasses
 * _BST/_BIX entirely, reading the underlying SBS registers directly
 * through a small wrapper ACPI method (\_SB.MBRD, loaded via a tiny
 * additive SSDT, no DSDT modification), and registers its own corrected
 * power_supply device in place of the stock ACPI battery driver.
 */

#include <linux/module.h>
#include <linux/acpi.h>
#include <linux/power_supply.h>
#include <linux/workqueue.h>
#include <linux/slab.h>
#include <linux/device.h>
#include <linux/platform_device.h>

#define DRVNAME "macbat_fix"
#define POLL_MS 10000
#define SANE_CHARGE_MAH_MAX 10000

struct macbat_data {
	struct power_supply *psy;
	struct delayed_work poll_work;
	struct acpi_device *bat_adev;
	struct device *pdev;

	bool present;
	int status;
	s32 current_now_ua;
	u32 voltage_now_uv;
	u32 voltage_min_design_uv;
	u32 charge_now_uah;
	u32 charge_full_uah;
	u32 charge_full_design_uah;
	u32 cycle_count;
	char model[32];
	char manufacturer[32];
	char serial[16];
};

static struct macbat_data *g_data;

static int mbrd_call(int type, int reg, struct acpi_buffer *out)
{
	union acpi_object args[2];
	struct acpi_object_list arg_list;
	acpi_status status;

	args[0].type = ACPI_TYPE_INTEGER;
	args[0].integer.value = type;
	args[1].type = ACPI_TYPE_INTEGER;
	args[1].integer.value = reg;

	arg_list.count = 2;
	arg_list.pointer = args;

	out->length = ACPI_ALLOCATE_BUFFER;
	out->pointer = NULL;

	status = acpi_evaluate_object(NULL, "\\_SB.MBRD", &arg_list, out);
	if (ACPI_FAILURE(status))
		return -EIO;
	return 0;
}

static int read_word(int reg, u32 *val)
{
	struct acpi_buffer out;
	union acpi_object *obj;
	int ret;

	ret = mbrd_call(0, reg, &out);
	if (ret)
		return ret;
	obj = out.pointer;
	if (!obj || obj->type != ACPI_TYPE_INTEGER) {
		kfree(out.pointer);
		return -EINVAL;
	}
	*val = (u32)obj->integer.value;
	kfree(out.pointer);
	return 0;
}

static int read_named_int(const char *path, u32 *val)
{
	struct acpi_buffer out = { ACPI_ALLOCATE_BUFFER, NULL };
	union acpi_object *obj;
	acpi_status status;

	status = acpi_evaluate_object(NULL, (acpi_string)path, NULL, &out);
	if (ACPI_FAILURE(status))
		return -EIO;
	obj = out.pointer;
	if (!obj || obj->type != ACPI_TYPE_INTEGER) {
		kfree(out.pointer);
		return -EINVAL;
	}
	*val = (u32)obj->integer.value;
	kfree(out.pointer);
	return 0;
}

static int read_string(int reg, char *dest, size_t destsz)
{
	struct acpi_buffer out;
	union acpi_object *obj;
	u32 scnt = 0;
	size_t n;
	int ret;

	ret = mbrd_call(1, reg, &out);
	if (ret)
		return ret;
	obj = out.pointer;
	if (!obj || obj->type != ACPI_TYPE_BUFFER) {
		kfree(out.pointer);
		return -EINVAL;
	}

	read_named_int("\\_SB.PCI0.LPCB.EC.SCNT", &scnt);

	n = min_t(size_t, scnt, obj->buffer.length);
	n = min_t(size_t, n, destsz - 1);
	if (n > 0)
		memcpy(dest, obj->buffer.pointer, n);
	dest[n] = '\0';

	kfree(out.pointer);
	return 0;
}

static s32 sign_extend_16(u32 raw)
{
	if (raw & 0x8000)
		return (s32)raw - 0x10000;
	return (s32)raw;
}

static void macbat_update(struct macbat_data *d)
{
	u32 raw, pwrs = 0, bstatus = 0;

	if (read_word(0x0A, &raw)) {
		d->present = false;
		return;
	}
	d->current_now_ua = sign_extend_16(raw) * 1000;

	if (read_word(0x09, &raw))
		return;
	d->voltage_now_uv = raw * 1000;

	/* SBS charge registers occasionally return a transient garbage
	 * value if read while the EC/SMBus is still settling (observed
	 * right after a real S3 resume, e.g. reg 0x10 misread as far
	 * above design capacity). No real charge reading on this pack
	 * can plausibly exceed 2x its 4800mAh design capacity; reject
	 * anything above that and keep the last good value instead of
	 * propagating garbage to userspace. */
	if (!read_word(0x0F, &raw) && raw <= SANE_CHARGE_MAH_MAX)
		d->charge_now_uah = raw * 1000;
	if (!read_word(0x10, &raw) && raw <= SANE_CHARGE_MAH_MAX)
		d->charge_full_uah = raw * 1000;
	if (!read_word(0x18, &raw) && raw <= SANE_CHARGE_MAH_MAX)
		d->charge_full_design_uah = raw * 1000;
	if (!read_word(0x19, &raw))
		d->voltage_min_design_uv = raw * 1000;
	if (!read_word(0x17, &raw))
		d->cycle_count = raw;

	read_string(0x21, d->model, sizeof(d->model));
	read_string(0x20, d->manufacturer, sizeof(d->manufacturer));

	if (!read_word(0x1C, &raw))
		snprintf(d->serial, sizeof(d->serial), "%u", raw);

	read_named_int("\\PWRS", &pwrs);
	read_word(0x16, &bstatus);
	if (bstatus & 0x0010) {
		/* FULLY_CHARGED, per the SBS BatteryStatus() bitfield */
		d->status = POWER_SUPPLY_STATUS_FULL;
	} else if (bstatus & 0x8000) {
		/* OVER_CHARGED_ALARM: this bq20z451 firmware never sets
		 * FULLY_CHARGED and signals a completed charge with this
		 * alarm bit instead (confirmed: seen alongside charge_now
		 * == charge_full and current_now == 0) */
		d->status = POWER_SUPPLY_STATUS_FULL;
	} else if (pwrs) {
		if (d->charge_full_uah && d->charge_now_uah >= d->charge_full_uah) {
			/* charge_now has reached charge_full by our own SBS
			 * charge registers, but the EC hasn't set
			 * FULLY_CHARGED/OVER_CHARGED_ALARM yet: this pack's
			 * real top-balance trickle tail can keep delivering a
			 * real, non-negligible current for many minutes past
			 * 100%. Report Full once the numbers say full and let
			 * that trickle run in the background rather than
			 * surfacing it as "Charging" indefinitely. */
			d->status = POWER_SUPPLY_STATUS_FULL;
		} else {
			d->status = (bstatus & 0x4000) ?
				/* TERMINATE_CHARGE_ALARM: charging paused, e.g. thermal */
				POWER_SUPPLY_STATUS_NOT_CHARGING :
				POWER_SUPPLY_STATUS_CHARGING;
		}
	} else {
		d->status = POWER_SUPPLY_STATUS_DISCHARGING;
	}

	d->present = true;
}

static int macbat_percent(struct macbat_data *d)
{
	if (!d->charge_full_uah)
		return 0;
	return (int)((u64)d->charge_now_uah * 100 / d->charge_full_uah);
}

static enum power_supply_property macbat_props[] = {
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_TECHNOLOGY,
	POWER_SUPPLY_PROP_CYCLE_COUNT,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_VOLTAGE_MIN_DESIGN,
	POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_CHARGE_NOW,
	POWER_SUPPLY_PROP_CHARGE_FULL,
	POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_CAPACITY_LEVEL,
	POWER_SUPPLY_PROP_MODEL_NAME,
	POWER_SUPPLY_PROP_MANUFACTURER,
	POWER_SUPPLY_PROP_SERIAL_NUMBER,
};

static int macbat_get_property(struct power_supply *psy,
				enum power_supply_property psp,
				union power_supply_propval *val)
{
	struct macbat_data *d = power_supply_get_drvdata(psy);
	int pct;

	switch (psp) {
	case POWER_SUPPLY_PROP_STATUS:
		val->intval = d->present ? d->status : POWER_SUPPLY_STATUS_UNKNOWN;
		break;
	case POWER_SUPPLY_PROP_PRESENT:
		val->intval = d->present;
		break;
	case POWER_SUPPLY_PROP_TECHNOLOGY:
		val->intval = POWER_SUPPLY_TECHNOLOGY_LION;
		break;
	case POWER_SUPPLY_PROP_CYCLE_COUNT:
		val->intval = d->cycle_count;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		val->intval = d->voltage_now_uv;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_MIN_DESIGN:
		val->intval = d->voltage_min_design_uv;
		break;
	case POWER_SUPPLY_PROP_CURRENT_NOW:
		val->intval = d->current_now_ua;
		break;
	case POWER_SUPPLY_PROP_CHARGE_NOW:
		val->intval = d->charge_now_uah;
		break;
	case POWER_SUPPLY_PROP_CHARGE_FULL:
		val->intval = d->charge_full_uah;
		break;
	case POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN:
		val->intval = d->charge_full_design_uah;
		break;
	case POWER_SUPPLY_PROP_CAPACITY:
		val->intval = macbat_percent(d);
		break;
	case POWER_SUPPLY_PROP_CAPACITY_LEVEL:
		pct = macbat_percent(d);
		if (pct <= 5)
			val->intval = POWER_SUPPLY_CAPACITY_LEVEL_CRITICAL;
		else if (pct <= 15)
			val->intval = POWER_SUPPLY_CAPACITY_LEVEL_LOW;
		else if (pct >= 100)
			val->intval = POWER_SUPPLY_CAPACITY_LEVEL_FULL;
		else
			val->intval = POWER_SUPPLY_CAPACITY_LEVEL_NORMAL;
		break;
	case POWER_SUPPLY_PROP_MODEL_NAME:
		val->strval = d->model;
		break;
	case POWER_SUPPLY_PROP_MANUFACTURER:
		val->strval = d->manufacturer;
		break;
	case POWER_SUPPLY_PROP_SERIAL_NUMBER:
		val->strval = d->serial;
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

static const struct power_supply_desc macbat_desc = {
	.name = "BAT0",
	.type = POWER_SUPPLY_TYPE_BATTERY,
	.properties = macbat_props,
	.num_properties = ARRAY_SIZE(macbat_props),
	.get_property = macbat_get_property,
};

static void macbat_poll_fn(struct work_struct *work)
{
	struct macbat_data *d = container_of(to_delayed_work(work),
					      struct macbat_data, poll_work);
	macbat_update(d);
	power_supply_changed(d->psy);
	schedule_delayed_work(&d->poll_work, msecs_to_jiffies(POLL_MS));
}

static void macbat_notify(acpi_handle handle, u32 event, void *context)
{
	struct macbat_data *d = context;

	if (!d)
		return;
	macbat_update(d);
	power_supply_changed(d->psy);
}

static int __init macbat_init(void)
{
	struct acpi_device *adev;
	struct power_supply_config psy_cfg = {};
	int ret;

	struct device *pdev;

	adev = acpi_dev_get_first_match_dev("PNP0C0A", NULL, -1);
	if (!adev) {
		pr_err(DRVNAME ": no PNP0C0A battery device found\n");
		return -ENODEV;
	}

	/* the real acpi-battery driver binds at the platform-bus level,
	 * not on the acpi_device itself */
	pdev = bus_find_device_by_name(&platform_bus_type, NULL, "PNP0C0A:00");
	if (pdev && pdev->driver) {
		pr_info(DRVNAME ": unbinding stock driver '%s' from BAT0\n",
			pdev->driver->name);
		device_release_driver(pdev);
	}
	if (!pdev)
		pr_warn(DRVNAME ": platform device PNP0C0A:00 not found, proceeding anyway\n");

	g_data = kzalloc(sizeof(*g_data), GFP_KERNEL);
	if (!g_data) {
		if (pdev)
			put_device(pdev);
		acpi_dev_put(adev);
		return -ENOMEM;
	}
	g_data->bat_adev = adev;
	g_data->pdev = pdev;

	psy_cfg.drv_data = g_data;

	g_data->psy = power_supply_register(pdev, &macbat_desc, &psy_cfg);
	if (IS_ERR(g_data->psy)) {
		ret = PTR_ERR(g_data->psy);
		pr_err(DRVNAME ": failed to register power_supply: %d\n", ret);
		if (pdev)
			put_device(pdev);
		kfree(g_data);
		g_data = NULL;
		acpi_dev_put(adev);
		return ret;
	}

	INIT_DELAYED_WORK(&g_data->poll_work, macbat_poll_fn);
	macbat_update(g_data);
	schedule_delayed_work(&g_data->poll_work, msecs_to_jiffies(2000));

	ret = acpi_dev_install_notify_handler(adev, ACPI_ALL_NOTIFY, macbat_notify, g_data);
	if (ret)
		pr_warn(DRVNAME ": could not install notify handler: %d\n", ret);

	pr_info(DRVNAME ": loaded, corrected battery reporting active\n");
	return 0;
}

static void __exit macbat_exit(void)
{
	struct device *pdev;
	int ret;

	if (!g_data)
		return;

	acpi_dev_remove_notify_handler(g_data->bat_adev, ACPI_ALL_NOTIFY, macbat_notify);
	cancel_delayed_work_sync(&g_data->poll_work);
	power_supply_unregister(g_data->psy);
	acpi_dev_put(g_data->bat_adev);

	pdev = g_data->pdev;
	kfree(g_data);
	g_data = NULL;

	if (pdev) {
		/* give the stock driver a chance to reclaim the device */
		ret = device_attach(pdev);
		if (ret < 0)
			pr_warn(DRVNAME ": could not rebind stock driver: %d\n", ret);
		put_device(pdev);
	}

	pr_info(DRVNAME ": unloaded\n");
}

module_init(macbat_init);
module_exit(macbat_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Corrected real-time battery reporting for MacBookAir6,1");
MODULE_AUTHOR("Yury MonZon");

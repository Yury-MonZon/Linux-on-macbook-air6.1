# CachyOS fresh-install guide: MacBook Air 6,1 (A1465, i7-4650U)

Condensed, imperative, **final state only**. For the full reasoning, dead ends, and measured evidence behind each choice, see `cachyos_patches_applied.txt` in this same directory (personal research log, not included in this public guide). Do these roughly in the order listed; a few have real ordering dependencies (noted).

> Every code block is written to discover its own values (UUIDs, package versions, boot-entry hashes) at run time via shell command substitution, so copy-paste each block into a root shell in order, no manual editing or find-and-replace needed.

**What you'll end up with:** a MacBook Air 6,1 on CachyOS with:

- Working **hibernate and suspend-then-hibernate**, no spurious wake loops
- A working **FaceTime HD camera**
- **WiFi** via the correct proprietary driver (not the non-functional in-kernel one)
- The physical **Fn/Control keys swapped** so the leftmost bottom-row key acts as Control (the natural PC position, so swap the keycaps to match), with **F1-F12 working** as plain function keys by default (hold Fn for the media/brightness row)
- **Thunderbolt disabled** for ~2W power savings
- **Thermal throttling eliminated, not just reduced**: at stock, this CPU throttled 10.1% of the time under sustained load and idled at ~68-73°C; after a repaste + a thermal pad bridging the heatsink to the aluminum bottom case, that's 0.0% throttle time and a ~52-54°C idle floor, a measured +34.6% sustained throughput increase. On top of the physical fix, the CPU is frequency-capped (2.8GHz on AC, the highest P-state with zero throttling in a 90s sustained 4-core test; 3.0GHz hits the 100°C critical ceiling and throttles hard within seconds; 1.6GHz on battery, chosen for ops-per-watt over raw runtime) and TLP is kept in sync with that cap so it doesn't get silently reverted on every AC plug/unplug
- **Fan control** (`mbpfan`) matched to this unit's real SMC RPM floor, not a generic default
- Accurate **battery reporting: real cycle count**, capacities, and fw serial instead of Linux's default zeroes/blanks

Two long-standing hardware limitations remain even after all of this: the lid switch can't wake the machine from suspend (needs a key tap after opening it), and macOS's rich battery serial string has no ACPI equivalent. Both are covered where they come up (§10, §16).

---

## 1. Install / partitioning

- Standard CachyOS Calamares install, BTRFS + Limine.
- Let Calamares build its own partition/subvolume layout (`@`, `@home`, `@root`, `@srv`, `@cache`, `@log`, `@tmp`); don't fight it with a manual scheme, it gets recreated anyway.
- `/boot` **must** be the actual FAT32 EFI System Partition, not a btrfs directory: Limine has no BTRFS driver, only FAT12/16/32 + ISO9660.
- Keep a small FAT32 partition GPT-typed `EF00` with the extracted CachyOS ISO contents (not just the raw `.iso`; it needs `/EFI`, `/arch`, `/boot`, and the `archisosearchuuid` marker file) as a rescue/reinstall medium. `EF00` typing is required for Apple's Option-key boot picker to see it at all.
- After first boot, set the Apple Option-key picker's default: at the picker, arrow to the Linux/EFI entry and hold **Control** (an up-arrow indicator appears). This is a separate mechanism from `efibootmgr`'s `BootOrder`.

---

## 2. Swap: zram + dedicated `@swap` subvolume

Do this **before** the hibernate config in step 3.

Do NOT put a swapfile inside the `@` root subvolume: BTRFS refuses to snapshot any subvolume containing an active swapfile, which silently breaks every snap-pac pacman post-transaction snapshot (a confusing `Invalid snapshot '--type'` error shows up one transaction later).

```bash
BTRFS_UUID=$(findmnt -no UUID /)   # the swap subvol lives on the same filesystem as root
sudo mkdir -p /mnt/btrfs-top
sudo mount -o subvolid=5 UUID="$BTRFS_UUID" /mnt/btrfs-top
sudo btrfs subvolume create /mnt/btrfs-top/@swap
sudo umount /mnt/btrfs-top

sudo mkdir -p /swap
echo "UUID=$BTRFS_UUID /swap btrfs subvol=/@swap,defaults,noatime 0 0" | sudo tee -a /etc/fstab
sudo systemctl daemon-reload
sudo mount /swap

sudo btrfs filesystem mkswapfile --size 9g /swap/swapfile
echo "/swap/swapfile none swap defaults 0 0" | sudo tee -a /etc/fstab
sudo swapon /swap/swapfile

# stash the resume offset for step 3 to pick up automatically
sudo btrfs inspect-internal map-swapfile -r /swap/swapfile \
  | sudo tee /etc/default/limine-resume-offset > /dev/null
echo "$BTRFS_UUID" | sudo tee /etc/default/limine-swap-uuid > /dev/null
```

zram0 stays primary (priority 100, default CachyOS zstd config); the btrfs swapfile is priority -1 (fallback/hibernate target only).

---

## 3. Hibernate resume parameters

- Append to `/etc/default/limine` using the values step 2 stashed:

  ```bash
  SWAP_UUID=$(sudo cat /etc/default/limine-swap-uuid)
  RESUME_OFFSET=$(sudo cat /etc/default/limine-resume-offset)
  echo "KERNEL_CMDLINE[default]+=\" resume=UUID=$SWAP_UUID resume_offset=$RESUME_OFFSET\"" \
    | sudo tee -a /etc/default/limine
  ```

- `/etc/mkinitcpio.conf` `HOOKS`: add `resume` (the plain hook from base mkinitcpio, **not** `sd-resume`, since that name doesn't exist), placed before `filesystems`:

  ```bash
  grep -qP '^HOOKS=\([^)]*\bresume\b' /etc/mkinitcpio.conf || \
    sudo sed -i 's/^HOOKS=(\(.*\)filesystems\(.*\))/HOOKS=(\1resume filesystems\2)/' /etc/mkinitcpio.conf
  ```

- Regenerate:

  ```bash
  sudo limine-update   # rebuilds boot config + both kernels' initramfs
  ```

- Verify after reboot: `/sys/power/resume` and `/sys/power/resume_offset` match what you set.

Hibernate reliability historically had an intermittent RTC-related bug on this hardware; see **§10** below (`rtc_cmos.use_acpi_alarm=1`) for the actual fix. With that in place, hibernate via suspend-then-hibernate is reliable.

---

## 4. Fn-key / keyboard remap

Do NOT use `/etc/modprobe.d/hid_apple.conf`: the internal keyboard's `hid_apple` driver binds before `/etc/modprobe.d` is reliably read at boot (a genuine boot-ordering race). Use a udev rule instead:

```bash
sudo tee /etc/udev/rules.d/99-hid-apple-fix.rules > /dev/null << 'EOF'
ACTION=="add", SUBSYSTEM=="module", KERNEL=="hid_apple", RUN+="/bin/sh -c 'echo 1 > /sys/module/hid_apple/parameters/swap_opt_cmd; echo 2 > /sys/module/hid_apple/parameters/fnmode; echo 1 > /sys/module/hid_apple/parameters/swap_fn_leftctrl'"
EOF
sudo udevadm control --reload-rules
```

(`swap_opt_cmd=1`, `fnmode=2`, `swap_fn_leftctrl=1`)

---

## 5. PAM faillock

```bash
sudo sed -i -E 's/^#?[[:space:]]*deny[[:space:]]*=.*/deny = 0/' /etc/security/faillock.conf
```

(default locks out GDM/sudo login after 3 failed attempts for 10 minutes)

---

## 6. GNOME extensions + settings

Install these via [extensions.gnome.org](https://extensions.gnome.org) or the GNOME Extension Manager (installed below). Search each by name and configure to your own preference (there's no generic "correct" per-extension setting to copy; these are a personal choice, not something this guide can hand you pre-configured):

- AppIndicator and KStatusNotifierItem Support (`appindicatorsupport@rgcjonas.gmail.com`)
- Dash to Dock (`dash-to-dock@micxgx.gmail.com`)
- Vitals (`Vitals@CoreCoding.com`)
- Clipboard Indicator (`clipboard-indicator@tudmotu.com`)
- Audio Switch Shortcuts (`audio-switch-shortcuts@dbatis.github.com`)
- Primary Input On Lockscreen (`primary_input_on_lockscreen@sagidayan.com`)
- ddterm, a drop-down terminal (`ddterm@amezin.github.com`)
- Medialine (`medialine@funinkina.co.in`)
- CoverflowAltTab (`CoverflowAltTab@palatis.blogspot.com`)
- Hibernate Power Menu (`hibernate-power-menu@arnakazim`), installed separately below, not via Extension Manager

Also set: xkb input sources `us`+`ru` (per-window = true), language switch hotkeys `<Alt>Shift_L` / `<Shift><Alt>`; adjust to your own preferred layouts.

Install Extension Manager via Flatpak **user** scope (system scope needs interactive polkit, which fails over SSH):

```bash
flatpak remote-add --user flathub https://flathub.org/repo/flathub.flatpakrepo
flatpak install --user com.mattjakeman.ExtensionManager
```

Takes effect on the next GNOME session start (log out/in; Wayland can't live-reload extensions the way X11 could).

Hibernate Power Menu installs from source (checks its own declared `shell-version` list, currently `48`/`49`/`50`, against your running shell first and aborts with a clear message if they don't match, rather than installing an incompatible version):

```bash
SHELL_VER=$(gnome-shell --version | grep -oP '\d+' | head -1)
git clone https://git.sr.ht/~arnakazim/gnome-hibernate-power-menu /tmp/gnome-hibernate-power-menu
cd /tmp/gnome-hibernate-power-menu

if grep -q "\"$SHELL_VER\"" metadata.json; then
  make install
  gnome-extensions enable hibernate-power-menu@arnakazim
  echo "Installed and enabled. Log out and back in for it to appear in the power menu."
else
  echo "GNOME Shell $SHELL_VER not in this extension's supported shell-version list, skipping."
fi

cd - > /dev/null
```

---

## 7. FaceTime HD camera

```bash
paru -S facetimehd-dkms facetimehd-firmware facetimehd-data
```

If the DKMS build fails with `call to undeclared library function 'strncpy'`, that's because kernel 7.2+ removed `strncpy()` entirely. Fix directly in the DKMS-registered source (persists across rebuilds):

```bash
FTHD_VER=$(pacman -Q facetimehd-dkms | awk '{print $2}' | cut -d- -f1)
sudo sed -i 's/\bstrncpy(/strscpy(/g' "/usr/src/facetimehd-$FTHD_VER/fthd_v4l2.c"
sudo dkms install "facetimehd/$FTHD_VER" -k "$(uname -r)"
```

Verify: `/dev/video0` present, `dmesg` shows a clean ISP init (firmware loaded, DDR calibration completed, no errors).

---

## 8. WiFi driver (BCM4360)

The real chip on this model is a Broadcom BCM4360 (confirmed via `lspci -k`, subsystem `Apple Inc. Device 0117`); it needs the proprietary `wl` driver, not the in-kernel `brcmfmac`/`b43` stack (which doesn't support this chip). Available directly from the CachyOS repo, no AUR helper needed:

```bash
sudo pacman -S --needed broadcom-wl-dkms
sudo modprobe wl
```

Verify:

```bash
lspci -k | grep -A3 "Network controller"   # should show "Kernel driver in use: wl"
iw dev                                     # should list wlan0
```

NetworkManager picks up `wlan0` automatically; no separate NM config needed.

---

## 9. Thunderbolt disable (power saving)

```bash
grep -q 'acpi_osi=!Darwin' /etc/default/limine || \
  echo 'KERNEL_CMDLINE[default]+=" acpi_osi=!Darwin"' | sudo tee -a /etc/default/limine
sudo limine-update
```

Fully disables the DSL3510 Thunderbolt controller (~2W savings vs. partial-savings blacklist approaches). Thunderbolt is completely gone from `lspci` after, not just unbound. This is a deliberate, permanent tradeoff; don't remove it later chasing wake-speed theories: it was tested and disproven as a wake-speed cause, and removing it only makes suspend-resume slower (Thunderbolt bridges re-enumerate).

---

## 10. Suspend/wake: final working config (do not deviate from this)

```bash
sudo mkdir -p /etc/systemd/sleep.conf.d /etc/systemd/logind.conf.d

# real deep S3 (suspend-to-RAM), not s2idle: measured ~0.06W standby draw
# (weeks of standby) vs the 1-3W typical of s2idle on this Haswell hardware
sudo tee /etc/systemd/sleep.conf.d/50-deep-s3.conf > /dev/null << 'EOF'
[Sleep]
MemorySleepMode=deep
EOF

# lid closes with hybrid-sleep: hibernation image written up front, THEN
# suspend to RAM. Deliberately not suspend-then-hibernate, see below.
sudo tee /etc/systemd/logind.conf.d/50-lid-hybrid-sleep.conf > /dev/null << 'EOF'
[Login]
HandleLidSwitch=hybrid-sleep
HandleLidSwitchExternalPower=hybrid-sleep
EOF
```

Kernel cmdline additions go in **`/etc/default/limine` `KERNEL_CMDLINE`, not `/etc/kernel/cmdline`** (this system's `limine-entry-tool` ignores that file once `KERNEL_CMDLINE[default]` is set; run `sudo limine-update` after any cmdline edit to rebuild both initramfs images + `limine.conf`):

```bash
for param in \
  'acpi_sleep=nonvs' \
  'button.lid_init_state=open' \
  'i915.enable_dc=0 i915.enable_fbc=0 i915.enable_psr=0' \
  'nvme_core.default_ps_max_latency_us=0 nvme.noacpi=1' ; do
  grep -qF "$param" /etc/default/limine || \
    echo "KERNEL_CMDLINE[default]+=\" $param\"" | sudo tee -a /etc/default/limine
done
sudo limine-update
```

**`rtc_cmos.use_acpi_alarm=1` deliberately is NOT in that list, and earlier versions of this guide were wrong to call it "THE critical fix".** It is a no-op on this hardware: the kernel force-enables it anyway via a DMI quirk (`drivers/rtc/rtc-cmos.c`, `use_acpi_alarm_quirks()`: Intel plus `dmi_get_bios_year() >= 2015` sets it unconditionally), and that runs during probe, after module-parameter parsing, so the command line can never change it either way. This Mac reports a 2022 BIOS date (Apple firmware 474.0.0.0.0 on 2013 hardware), so it trips the quirk. Verified by removing the parameter, rebooting, and observing `/sys/module/rtc_cmos/parameters/use_acpi_alarm` still reading `Y`. Setting it changes nothing; whatever fixed the old suspend-retrigger loop was one of the other changes here.

```bash
# disable ACPI wake from S3 on XHC1 (USB/Bluetooth controller):
# known MacBookAir6,x spurious-wake source, causes instant self-resume
# after suspend if left enabled
sudo tee /etc/udev/rules.d/90-xhc_sleep.rules > /dev/null << 'EOF'
SUBSYSTEM=="pci", KERNEL=="0000:00:14.0", ATTR{power/wakeup}="disabled"
EOF
sudo udevadm control --reload-rules
```

(side effect: Bluetooth peripherals can no longer wake the machine from suspend, not normally missed on a laptop)

**Both hooks below must go in `/usr/lib/systemd/system-sleep/`, not `/etc/systemd/system-sleep/`.** The `/etc/` path looks plausible (systemd usually supports an `/etc/` override alongside `/usr/lib/`) but `systemd-sleep` only ever scans `/usr/lib/systemd/system-sleep/` for this specific hook mechanism -- confirmed by direct instrumentation after both hooks turned out to have been silently inert for the entire life of this project when placed in `/etc/`. If you already have these in `/etc/systemd/system-sleep/` from an earlier version of this guide, move them, don't copy: `sudo mv /etc/systemd/system-sleep/* /usr/lib/systemd/system-sleep/` (the facetimehd hibernate-freeze fix and this d3cold fix were never actually running until moved).

```bash
sudo tee /usr/lib/systemd/system-sleep/facetimehd > /dev/null << 'EOF'
#!/bin/bash
case "$2" in
  hibernate|hybrid-sleep)
    case "$1" in
      pre)  modprobe -r facetimehd 2>/dev/null ;;
      post) modprobe facetimehd 2>/dev/null ;;
    esac
    ;;
esac
EOF
sudo chmod +x /usr/lib/systemd/system-sleep/facetimehd
```

(unloads/reloads the camera driver around hibernate transitions only: the driver failing to cleanly re-suspend right after its own fragile s2idle-resume firmware reinit was the root cause of a genuine kernel freeze at hibernation-entry, requiring a hard cold boot to recover. Scoped to `$2` = hibernate/hybrid-sleep only, not plain suspend: the camera's own ISP firmware reload can stall for several real seconds on `post`, and that cost has no reason to be paid on an ordinary lid-open S3 resume where the freeze bug doesn't occur. Do NOT also match `suspend-then-hibernate` here even though it sounds hibernate-adjacent: if your `HandleLidSwitch` is set to `suspend-then-hibernate`, systemd passes that exact string as `$2` on every lid-close, including ones that stay a plain few-second S3 sleep and never reach real hibernation, confirmed live by kernel log showing "PM: suspend entry (deep)"/"PM: suspend exit" (not hibernation) on a cycle that still triggered the reload before this was caught and fixed.)

```bash
sudo tee /usr/lib/systemd/system-sleep/disable-d3cold > /dev/null << 'EOF'
#!/bin/bash
case $1 in
  pre)
    find /sys/devices/ -name d3cold_allowed -exec sh -c 'echo 0 > "$1" 2>/dev/null' _ {} \;
    ;;
esac
EOF
sudo chmod +x /usr/lib/systemd/system-sleep/disable-d3cold
```

```bash
systemctl disable NetworkManager-wait-online.service   # minor boot-time win
```

**Why hybrid-sleep and not `suspend-then-hibernate`:** this is the single least obvious decision in the whole guide, and it is forced by a firmware limitation that took a lot of measuring to pin down.

`suspend-then-hibernate` works by suspending to RAM, arming an RTC wake alarm for `HibernateDelaySec`, waking itself at that deadline, and only then writing the hibernation image. **This machine's firmware will not honor an RTC alarm wake while the lid is shut and the system is in real S3.** The wake that is supposed to trigger the "should I hibernate now?" check never fires, so the machine simply stays suspended forever and only hibernates when you open the lid yourself. Proven with a controlled four-way test (a small program arming a `CLOCK_BOOTTIME_ALARM` timerfd across a suspend, plus a `systemd-inhibit handle-lid-switch` lock so logind could not interfere):

| sleep mode | lid | RTC alarm wakes it? |
|---|---|---|
| s2idle | closed | yes |
| deep S3 | open | yes |
| deep S3 | **closed** | **no** (slept through a 75 second alarm for 3.5 minutes) |

systemd is blameless here: with debug logging on it correctly logs `Set timerfd wake alarm for 59s` and the machine just sleeps through it.

`hybrid-sleep` sidesteps the problem entirely because **it has no timer at all**. It writes the hibernation image up front, then suspends to RAM, and stays there until woken. Confirmed in systemd's source: only `suspend-then-hibernate` goes through the timerfd loop (`execute_s2h()`), while `hybrid-sleep` calls `execute()` directly. The image is pure insurance: discarded on a normal wake, used only if power is actually lost. That means `HibernateDelaySec` is irrelevant under this config.

This is also, as it turns out, exactly what macOS does on this hardware. Its own power-management logs show `hibmode=3 standbydelaylow=10800`, i.e. it writes the image at sleep time and keeps RAM powered, and every wake in those logs is `EC.LidOpen` or HID activity, with not a single RTC or timer wake. macOS never needs the capability this firmware lacks. (The one thing macOS additionally does, and Linux cannot, is have the SMC cut RAM power after the standby delay. Boot Camp was checked too and ships no SMC or power driver at all, so there is no non-Darwin mechanism to borrow.)

**`MemorySleepMode=deep` is correct here, and earlier versions of this guide were wrong to forbid it.** The old objection was that the screen would not wake on lid-open. That is a real s2idle limitation (see below), not an S3 one. Under deep S3 the lid genuinely does wake the machine. Deep S3 was measured at roughly **0.06W** standby draw (about 4mAh over a 31 minute measured sleep, i.e. weeks of standby), versus the 1-3W typical of s2idle on Haswell-era hardware. Its one real cost is that firmware platform resume takes a few seconds before Linux regains control at all, which is invisible to every OS-side measurement and not fixable from Linux (§10a removes 2 seconds of it).

**Wake sources, and why they differ by sleep mode** (this trips people up, so it is worth stating precisely):

- **Under deep S3 (this config): opening the lid wakes the machine.** This is the normal workflow and it works.
- **Under s2idle: opening the lid does NOT wake it.** The lid event genuinely is received at the ACPI level, but under s2idle the kernel only treats an SCI as a real wake if it came from a GPE armed specifically for wake, and this one is not, so the event is processed without ending the s2idle loop. Enabling EC/LID0/SPIT in `/proc/acpi/wakeup` does not change it. The workaround there is to open the lid and then tap a key.
- **Keyboard wake needs the whole USB chain enabled, not just `XHC1`.** If you ever want it, note that the internal keyboard is USB device `1-5` hanging off `0000:00:14.0`, and enabling `XHC1` in `/proc/acpi/wakeup` alone is not enough: the **root hub** `usb1` also has to have `power/wakeup` enabled or the remote-wake signal cannot propagate up the chain. That was verified live (keyboard wake stayed dead with `XHC1` enabled, and started working the moment the root hub was enabled too). This guide leaves `XHC1` disabled on purpose (spurious-wake source, see the udev rule above), and under deep S3 the lid alone is sufficient.
- Waking from a genuinely *completed* hibernate (not just suspend) always requires the power button on this hardware. Not fixable here: neither the RTC nor the lid switch has a valid S4/power-on wake path, only S3.

Separately, the physical lid switch itself sometimes sends genuine spurious Lid-closed/Lid-opened ACPI events at short (30-90s) intervals with no user interaction; root cause not identified (hinge/sensor wear vs. firmware quirk). Currently just tolerated as-is.

---

## 10a. Cutting 2 seconds off every S3 resume (`DTLK` SSDT override)

Apple's firmware runs a Thunderbolt shutdown sequence on every resume from S3, and for non-Darwin operating systems it contains a blind **`Sleep(2000)`**, i.e. a full 2 seconds of doing nothing, on the critical resume path. Measured precisely with ftrace on `acpi_ps_execute_method()`: **2051ms before the fix, 164ms after.**

The method lives in `SSDT5` (OEM ID `APPLE `, table ID `PcieTbt`) and is called from `_WAK` only when `!OSDW() && Arg0 == 0x03`, i.e. S3 wake on a non-Darwin OS. Real macOS never executes it at all, which is why Apple never had to care about the delay.

```bash
# decompile the table as shipped by your own firmware
mkdir -p ~/ssdt-dtlk && cd ~/ssdt-dtlk
sudo cat /sys/firmware/acpi/tables/SSDT5 > SSDT5.aml
iasl -d SSDT5.aml
```

Make exactly two edits to `SSDT5.dsl`:

1. Bump the OEM revision in the `DefinitionBlock` header so the kernel accepts it as a replacement (an override only wins if its revision is strictly higher):
   `0x00001000` becomes `0x00001001`
2. Inside `Method (DTLK, 0, Serialized)`, change `Sleep (0x07D0)` (2000ms) to `Sleep (0x64)` (100ms).

100ms rather than removing the sleep outright is deliberate: `RP05` (the Thunderbolt root port) is already disabled the whole time we are asleep under this guide's config, so there is no live link transition to wait on, but a small settle margin costs nothing.

```bash
iasl -tc SSDT5.dsl                      # expect 0 errors, 0 warnings
sudo mkdir -p /etc/initcpio/acpi_override
sudo cp SSDT5.aml /etc/initcpio/acpi_override/ssdt5-dtlk-fix.aml
```

Add `acpi_override` to `HOOKS` in `/etc/mkinitcpio.conf` if it is not already there, then **before rebooting, build a clean fallback image with no overrides** so a bad table cannot leave you unbootable:

```bash
sudo mkinitcpio -k "$(uname -r)" -g /boot/initramfs-linux-cachyos-safe.img -S acpi_override
sudo limine-update
lsinitcpio /boot/initramfs-linux-cachyos.img | grep -i acpi   # should list the override
lsinitcpio /boot/initramfs-linux-cachyos-safe.img | grep -ci acpi   # should be 0
```

Confirm after reboot with `sudo journalctl -k -b | grep PcieTbt`, which should show `Table Upgrade: override [SSDT-APPLE - PcieTbt]`.

**Honest scope note:** this genuinely removes ~1.9 seconds of firmware stall, verified by direct measurement, but it does **not** make resume feel instant. Most of the remaining delay is firmware platform resume that happens before Linux regains control at all, and is invisible to every OS-side instrument. For reference, macOS logs its own wake times on this machine at 0.33 to 0.60 seconds, but those are wakes from its "Deep Idle" state, not from S3, so it is not a like-for-like comparison.

---

## 11. `no_batt_fix` (BD_PROCHOT disable safety net)

Harmless with a working battery; protects against a silent ~800MHz lock-in if the battery is ever removed/fails.

```bash
sudo pacman -S --needed msr-tools

sudo tee /usr/local/bin/no_batt_fix.sh > /dev/null << 'EOF'
#!/bin/bash
r=$(rdmsr 0x1FC); f=$(( (0x$r) & 0xFFFFE )); wrmsr 0x1FC $f
EOF
sudo chmod +x /usr/local/bin/no_batt_fix.sh

echo msr | sudo tee /etc/modules-load.d/msr.conf
sudo modprobe msr

sudo tee /etc/systemd/system/no_batt_fix.service > /dev/null << 'EOF'
[Unit]
After=network.target
[Service]
Type=simple
ExecStart=/usr/local/bin/no_batt_fix.sh
[Install]
WantedBy=multi-user.target
EOF

sudo systemctl enable --now no_batt_fix.service
```

---

## 12. TLP

```bash
sudo pacman -S tlp
sudo systemctl mask --now power-profiles-daemon.service   # NOT disable:
  # disable alone doesn't stop D-Bus auto-reactivation by GNOME's power UI
sudo systemctl enable --now tlp.service
```

Append to `/etc/tlp.conf` (don't overwrite the shipped template): `CPU_SCALING_MAX_FREQ_ON_AC` here MUST match §14's cap, or TLP will re-override it on every AC plug/unplug event; see §14 for why `CPU_SCALING_MAX_FREQ_ON_BAT` is 1.6GHz:

```bash
sudo tee -a /etc/tlp.conf > /dev/null << 'EOF'
CPU_SCALING_GOVERNOR_ON_AC=schedutil
CPU_SCALING_GOVERNOR_ON_BAT=conservative
CPU_SCALING_MIN_FREQ_ON_AC=800000
CPU_SCALING_MAX_FREQ_ON_AC=2800000
CPU_SCALING_MIN_FREQ_ON_BAT=800000
CPU_SCALING_MAX_FREQ_ON_BAT=1600000
INTEL_GPU_MIN_FREQ_ON_AC=200000
INTEL_GPU_MAX_FREQ_ON_AC=1100000
INTEL_GPU_MIN_FREQ_ON_BAT=200000
INTEL_GPU_MAX_FREQ_ON_BAT=200000
USB_EXCLUDE_PHONE=1
SOUND_POWER_SAVE_ON_AC=0
SOUND_POWER_SAVE_ON_BAT=1
SOUND_POWER_SAVE_CONTROLLER=Y
EOF
sudo systemctl restart tlp.service
```

Deliberately did NOT add: `DISK_APM_LEVEL_*`/`SATA_LINKPWR_*` (pure NVMe machine, these are SATA-only no-ops), aggressive `PCIE_ASPM_*`/`RUNTIME_PM_ON_BAT=auto` (risks destabilizing the wifi/camera drivers).

`powertop --html` will list further "Bad" tunables beyond what TLP covers; most of its generic advice actively conflicts with the tuning already done above (e.g. it wants `intel_pstate/min_perf_pct` raised to 50%, undoing the frequency caps in §14) and should not be applied blindly. The one genuinely safe, no-downside item is runtime PM for the Intel HECI (Management Engine) PCI device, applied via udev so it survives reboots:

```bash
sudo tee /etc/udev/rules.d/99-heci-runtime-pm.rules > /dev/null << 'EOF'
ACTION=="add", SUBSYSTEM=="pci", ATTR{vendor}=="0x8086", ATTR{device}=="0x9c3a", ATTR{power/control}="auto"
EOF
sudo udevadm control --reload-rules
sudo udevadm trigger --action=add --subsystem-match=pci
```

Left alone: USB autosuspend for the internal keyboard/trackpad and xHCI controller runtime PM both touch the same hardware already tuned for wake-source reliability in §10, real regression risk for negligible power gain.

---

## 13. Hardware thermal mods (physical work, not scriptable)

Repaste the CPU + add a thermal pad bridging the heatsink to the aluminum bottom case (1.5mm thick GELID).

Measured result: +34.6% sustained throughput, thermal throttling eliminated (10.1% throttle time at stock → 0.0% after both mods), idle floor dropped from ~68-73°C to ~52-54°C. This is what actually fixed the thermal problem: RAPL and frequency capping (§14) are software mitigations for a machine that didn't get this done; if you do this mod, the 2.8GHz cap in §14 becomes extra margin rather than a strict necessity.

---

## 14. CPU frequency caps

The actual working thermal/power lever: RAPL does *not* work on this hardware (see the RAPL note at the end of this section).

AC: capped to 2.8GHz, empirically the highest P-state bin with zero thermal throttling over a 90s sustained 4-core load (measured cliff: 2.9GHz ≈ noise-level 17ms/90s, 3.0GHz jumps to 6813ms/90s and hits the 100°C critical ceiling). Persisted two ways (must match each other):

```bash
sudo tee /usr/local/bin/set-freq-cap.sh > /dev/null << 'EOF'
#!/bin/bash
for c in /sys/devices/system/cpu/cpu*/cpufreq/scaling_max_freq; do
  echo 2800000 > "$c"
done
EOF
sudo chmod +x /usr/local/bin/set-freq-cap.sh

sudo tee /etc/systemd/system/set-freq-cap.service > /dev/null << 'EOF'
[Unit]
Description=Cap CPU max frequency to 2.8GHz
After=multi-user.target
[Service]
Type=oneshot
ExecStart=/usr/local/bin/set-freq-cap.sh
RemainAfterExit=yes
[Install]
WantedBy=multi-user.target
EOF

sudo systemctl enable --now set-freq-cap.service
```

Also set `CPU_SCALING_MAX_FREQ_ON_AC=2800000` in `tlp.conf` (§12); otherwise TLP silently reverts this to 3300000 on the next AC plug/unplug event.

Battery: capped to 1.6GHz via TLP's `CPU_SCALING_MAX_FREQ_ON_BAT` (§12). This is a deliberate performance-over-runtime choice: measured real battery runtime under moderate sustained load was 2:44 @1.2GHz vs. 2:22 @1.6GHz (runtime drops monotonically as cap rises; there's no efficiency "sweet spot" for pure runtime, only for ops-per-watt at continuous full load, which peaks at 1.6GHz/227 bogo-ops/W). If runtime becomes the priority again, drop back toward 1.2GHz.

**Note on RAPL:** PL1/PL2 power limits (`intel-rapl` powercap sysfs) do NOT work on this platform/microcode: the registers accept and reflect any value written but the chip ignores them entirely, running at its natural ~22-23W draw regardless. Frequency capping (above) is the only lever that actually works. RAPL limits are still set to sane 15W/25W values (vs. the firmware's nonsensical 100W/125W-with-2.44ms-window defaults) purely as harmless documentation; don't expect them to do anything.

**Note on undervolting:** not possible via any OS-level tool (`intel-undervolt`, `throttled`, writing MSR 0x150 directly) on this machine: Apple's firmware enforces an `OC_Locked` mailbox lock on that MSR during its own DXE boot phase, before any bootloader or OS gets control, so no software or alternate-bootloader workaround exists. The only way around it is flashing a corrected EC/BIOS image via an external SPI programmer; not attempted here (no programmer on hand, and the target flash region has no fallback if a flash is interrupted). See `macbook_air_undervolt_research.txt` for the full investigation if this ever becomes worth revisiting.

---

## 15. mbpfan

```bash
paru -S --needed mbpfan

sudo tee /etc/mbpfan.conf > /dev/null << 'EOF'
[general]
min_fan1_speed = 1200
max_fan1_speed = 6500
low_temp = 63
high_temp = 66
max_temp = 86
polling_interval = 7
EOF

sudo systemctl enable --now mbpfan
```

(1200/6500 matches this exact unit's real `applesmc` `fan1_min`/`fan1_max`; don't use the generic community default floor of 2000, it's louder than necessary and this hardware's SMC firmware clamps to ~1200 RPM as its own floor regardless of what's written anyway.)

---

## 16. Real battery info (cycle count, serial, correct capacity units)

Fixes Linux showing `cycle_count=0`, blank serial, and mismatched-looking capacity units for `BAT0`.

Apple's DSDT only implements the older ACPI `_BIF` battery method (no cycle-count field exists in that format at all) and hardcodes the serial number blank. Fix: a supplemental SSDT adding a real `_BIX` method, loaded via mkinitcpio's official built-in `acpi_override` hook (ships with the `mkinitcpio` package itself; do NOT write a custom hook with this same name, it'll shadow the real one and use the wrong `add_file` function).

1. Enable the hook:

   ```bash
   echo 'HOOKS+=(acpi_override)' | sudo tee /etc/mkinitcpio.conf.d/20-acpi-override.conf
   ```

2. Install the ACPI compiler, write the SSDT source (below) to a file, and compile it directly into the hook's directory:

   ```bash
   sudo pacman -S --needed acpica

   sudo mkdir -p /etc/initcpio/acpi_override
   cat > /tmp/battery-bix-fix.asl << 'ASLEOF'
   DefinitionBlock ("", "SSDT", 2, "LOCAL", "BATBIX", 0x00000002)
   {
       External (\_SB.BAT0, DeviceObj)
       External (\_SB.PCI0.LPCB.EC.SMB0.SBRW, MethodObj)
       External (\_SB.PCI0.LPCB.EC.SMB0.SBRB, MethodObj)
       External (\_SB.PCI0.LPCB.EC.SCNT, FieldUnitObj)

       Name (\_SB.BAT0.PBIX, Package (0x14)
       {
           0x00, 0x01, 0xFFFFFFFF, 0xFFFFFFFF, 0x01, 0xFFFFFFFF,
           0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF,
           0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0x01, 0x01,
           "", "", "", ""
       })

       Method (\_SB.BAT0._BIX, 0, Serialized)
       {
           Local0 = 0
           \_SB.PCI0.LPCB.EC.SMB0.SBRW (0x0B, 0x18, RefOf (Local0))
           \_SB.BAT0.PBIX [0x02] = Local0
           \_SB.PCI0.LPCB.EC.SMB0.SBRW (0x0B, 0x10, RefOf (Local0))
           \_SB.BAT0.PBIX [0x03] = Local0
           \_SB.PCI0.LPCB.EC.SMB0.SBRW (0x0B, 0x19, RefOf (Local0))
           \_SB.BAT0.PBIX [0x05] = Local0
           \_SB.PCI0.LPCB.EC.SMB0.SBRW (0x0B, 0x01, RefOf (Local0))
           \_SB.BAT0.PBIX [0x06] = Local0
           \_SB.PCI0.LPCB.EC.SMB0.SBRW (0x0B, 0x17, RefOf (Local0))
           \_SB.BAT0.PBIX [0x08] = Local0
           \_SB.PCI0.LPCB.EC.SMB0.SBRW (0x0B, 0x1C, RefOf (Local0))
           \_SB.BAT0.PBIX [0x11] = ToDecimalString (Local0)
           \_SB.PCI0.LPCB.EC.SMB0.SBRB (0x0B, 0x21, RefOf (Local0))
           Local2 = \_SB.PCI0.LPCB.EC.SCNT
           \_SB.BAT0.PBIX [0x10] = ToString (Mid (Local0, 0x00, Local2))
           \_SB.PCI0.LPCB.EC.SMB0.SBRB (0x0B, 0x22, RefOf (Local0))
           Local2 = \_SB.PCI0.LPCB.EC.SCNT
           \_SB.BAT0.PBIX [0x12] = ToString (Mid (Local0, 0x00, Local2))
           \_SB.PCI0.LPCB.EC.SMB0.SBRB (0x0B, 0x20, RefOf (Local0))
           Local2 = \_SB.PCI0.LPCB.EC.SCNT
           \_SB.BAT0.PBIX [0x13] = ToString (Mid (Local0, 0x00, Local2))
           Return (\_SB.BAT0.PBIX)
       }
   }
   ASLEOF

   iasl -tc /tmp/battery-bix-fix.asl
   sudo mv /tmp/battery-bix-fix.aml /etc/initcpio/acpi_override/batbix.aml
   ```

   (`External` declarations only need to match the real object *types* Apple's DSDT declares them as; confirm with `sudo iasl -d /sys/firmware/acpi/tables/DSDT -sa` if this ever needs adapting to different hardware; see the gotchas below for why the *paths* used here matter.)

3. Rebuild:

   ```bash
   sudo limine-update
   ```

   (not bare `mkinitcpio -p ...`: this system's `/usr/local/bin/mkinitcpio` wrapper prompts interactively for `-p`/`-P`, which hangs forever with no TTY; `limine-update` avoids that path entirely and also refreshes `limine.conf`'s embedded initramfs checksum, which is required either way)

4. Verify placement before rebooting:

   ```bash
   BOOT_HASH=$(cat /etc/machine-id)
   sudo python3 -c "
   d = open('/boot/$BOOT_HASH/linux-cachyos/initramfs', 'rb').read()
   z = d.find(bytes.fromhex('28b52ffd'))
   a = d.find(b'BATBIX')
   print(0 < a < z)
   "
   ```

   must print `True` (confirms it's in the early/uncompressed segment, not the main compressed archive: the difference between this working and silently doing nothing). `28b52ffd` is zstd's own file-format magic number (RFC 8878 §3.1.1), not a value specific to this machine; it only applies as-is because CachyOS's default `mkinitcpio.conf` uses zstd; if `COMPRESSION=` is set to something else, swap in that format's magic bytes instead (e.g. `1f8b` for gzip, `fd377a585a00` for xz).

**Key gotchas** (each cost real debugging time; don't repeat them):

- `BAT0`'s real ACPI path is `\_SB.BAT0` (direct child of the system bus), **not** `\_SB.PCI0.LPCB.EC.BAT0`; easy to misread from the DSDT's indentation, since Apple's own AML always uses fully-qualified absolute paths for its SMBus calls regardless of actual nesting. Every wrong-path attempt fails identically with `ACPI BIOS Error (bug): Object does not exist: BAT0`, no matter what else is changed.
- The block-read string fields (ManufacturerName `0x20`/DeviceName `0x21`/DeviceChemistry `0x22` via `SBRB`) return a fixed 32-byte EC buffer (`SBFR`, declared 256 bits in the DSDT): only the first *N* bytes are real, the rest is non-null leftover buffer content with a null only at the very end, so `ToString()` alone does not trim it. The real fix: `SCNT` (an 8-bit field right after `SBFR` in the DSDT, declared but never read anywhere in Apple's own code) holds the real byte count of the last block-read transaction; read it right after each `SBRB` call and slice with `Mid()` before `ToString()`. No hardcoding needed or wanted.
- Do NOT deliver this via a second Limine boot module (a hand-built early cpio + `initrd=` in `KERNEL_CMDLINE`): this caused a real kernel panic requiring manual recovery. The mkinitcpio `acpi_override` hook above is the correct, safe, already-proven mechanism (same one microcode already uses every boot); use it, not a custom one.
- Real cycle count, design capacity, and full capacity all confirmed matching macOS's own IOKit data exactly (verified via `ioreg -l -w0 -r -c AppleSmartBattery` on the macOS side, if dual-booting). The one thing not achievable: macOS's rich proprietary `Serial` string has no equivalent anywhere in the ACPI `_BIX` data model; only the small numeric `SerialNumber` (`FirmwareSerialNumber` in macOS) is reachable.
- `_BIX` field index 6 (Design Capacity of Warning) is filled from a genuine EC-reported threshold too: SBS command `0x01` (RemainingCapacityAlarm) returns the fuel gauge's real factory alarm level (300 mAh on this unit), not a guessed value. In practice nothing on this kernel surfaces it as a distinct sysfs file (`capacity_alert_min`/`max` don't exist in `drivers/acpi/battery.c` for ACPI batteries at all), so treat this as "correct or genuine data if anyone ever reads it," not as a feature that visibly does anything today.

---

## 17. Real-time battery current/power and live percentage (kernel module, no DSDT patch)

This fixes two remaining bugs that `_BIX` (§16) doesn't touch: reported power draw showing impossible values (70-100W on a 15W-TDP chip), and battery percentage getting stuck at 100% no matter how much it actually discharges. Both come from Apple's real `_BST` method (Battery Status, the one polled continuously for live state), not from anything `_BIX` covers.

**Root cause:** `_BIF`/`_BIX` declare a `Power Unit` field telling the kernel whether to expose this battery as mA-based (`current_now`/`charge_now`) or mW-based (`power_now`/`energy_now`). In `_BIF` that field is index 0; in `_BIX` a new `Revision` field shifts it to index 1. Apple's stock `_BIF` uses `0` (mW-based, matching their own crude `x10` mAh-to-mWh approximation used throughout their firmware). Our own `_BIX` from §16 deliberately uses `1` (mA-based) instead, to report clean, exact mAh values matching macOS rather than Apple's approximation. That's the right call, but it silently broke consistency with `_BST`, which was never updated to match: Apple's real `UBST` method still computes `current(mA) * voltage(mV) / 1000` (a genuine mW value) into the field the kernel now reads as current, and still multiplies `RemainingCapacity` by `10` the same way `_BIF`'s capacity fields used to be scaled. Confirmed this wasn't always broken: checking `upower`'s own historical rate log from before `_BIX` was ever installed shows sane 9-13W readings under plain stock `_BIF`.

**The approach:** an earlier version of this fix patched the real DSDT directly (replacing `_BST`'s logic the same way `_BIX` was added in §16), and that DSDT patch genuinely worked, verified over hours of real use. But `_BST` already exists in the firmware DSDT, so fixing it that way means replacing the *entire* table rather than adding a small overlay, a meaningfully bigger blast radius than any other fix in this guide. The approach below avoids that entirely: a small kernel module reads the underlying hardware registers directly (bypassing `_BST`/`_BIX` altogether) and replaces the kernel's stock battery driver for this one device. **The real DSDT stays 100% stock and unmodified** with this approach; only one small additive SSDT is used, in the same safe, proven, already-established category as `_BIX`.

Do §16 first if you also want the battery-info fields this section doesn't cover (nothing here strictly requires it, since the module below reads hardware directly and duplicates everything `_BIX` provides anyway, but §16 alone is a smaller, more conservative change if you don't need live current/power/percentage).

1. `_BST`'s real implementation returns its results by writing through a `RefOf()` reference argument, a mechanism that only works from inside another ACPI method's own scope, not from an external kernel module calling in. So the module needs one tiny helper method with a plain integer return value instead: `\_SB.MBRD(type, reg)`. `type=0` proxies `SBRW` (word reads: current, voltage, capacities, cycle count), `type=1` proxies `SBRB` (block/string reads: model, manufacturer). It's loaded as a small additive SSDT (a new method name, never existed before, so no `AE_ALREADY_EXISTS` risk, no table replacement). Source and the kernel module's C source both live in this repo's [`macbat-fix/`](macbat-fix/) directory rather than inline here, since the module is a few hundred lines:

   ```bash
   git clone https://github.com/Yury-MonZon/Linux-on-macbook-air6.1.git /tmp/macbat-fix-src
   cd /tmp/macbat-fix-src/macbat-fix
   iasl -tc mbread.asl
   sudo mkdir -p /etc/initcpio/acpi_override
   sudo cp mbread.aml /etc/initcpio/acpi_override/mbread.aml
   ```

   If you also did §16, remove `batbix.aml` from the same directory and any DSDT-replacement `.aml` from an earlier attempt; this module supersedes both entirely.

2. Package the module (`macbat-fix/macbat_fix.c`, `Makefile`, `dkms.conf` in the cloned repo) as DKMS, matching the pattern of the other out-of-tree modules in this guide (`facetimehd`, `broadcom-wl`), so it survives kernel updates and rebuilds automatically for every installed kernel:

   ```bash
   sudo mkdir -p /usr/src/macbat-fix-1.0
   sudo cp macbat_fix.c Makefile dkms.conf /usr/src/macbat-fix-1.0/
   sudo dkms add -m macbat-fix -v 1.0
   for k in $(ls /usr/lib/modules/ | grep cachyos); do
     sudo dkms install -m macbat-fix -v 1.0 -k "$k"
   done
   ```

3. Auto-load at boot and load it now:

   ```bash
   echo macbat_fix | sudo tee /etc/modules-load.d/macbat_fix.conf
   sudo limine-update
   sudo modprobe macbat_fix
   ```

4. Verify:

   ```bash
   sudo dmesg | grep macbat_fix
   for f in status capacity charge_now charge_full current_now voltage_now cycle_count model_name; do
     printf "%-15s %s\n" "$f:" "$(cat /sys/class/power_supply/BAT0/$f)"
   done
   upower -i $(upower -e | grep BAT) | grep -E "energy-rate|percentage"
   ```

   Expect `unbinding stock driver 'acpi-battery' from BAT0` followed by `loaded, corrected battery reporting active`, real mAh values matching §16's numbers, a plausible `energy-rate` (single digits to ~20W range, not 70-100W), and `capacity`/`charge_now` that actually move over a few minutes of real charge/discharge instead of staying frozen.

**Reversible at any time:** `sudo rmmod macbat_fix` cleanly hands the device back to the stock driver (confirmed: `acpi-battery` automatically reclaims it, no gap in battery reporting, no reboot needed). The real DSDT was never touched, so there is no fallback-boot-entry scenario to worry about here the way there would be for a table patch.

**Because §10 uses real deep S3 sleep:** the EC/SMBus genuinely loses power during a real S3 cycle (unlike `s2idle`, where it never does), and a read landing during its post-resume settle window can occasionally return garbage. The shipped module already guards against this (a sanity ceiling on the charge registers rejects anything implausible and keeps the last known-good value instead), but if you ever see `upower` briefly report an absurd `energy-full` right after a real S3 resume, it should self-correct within one poll cycle (10s); `sudo systemctl restart upower.service` clears a stale cached value immediately if it doesn't.

**Charging status near 100%:** this pack's firmware doesn't always flip its own completion bits promptly, and can keep delivering a real, slowly-tapering top-balance trickle current for many minutes after `charge_now` already equals `charge_full`. The module reports `Full` as soon as the charge registers themselves say 100%, rather than waiting on the EC's own (sometimes late) completion signal, so `upower`/`status` won't sit on `Charging` for the whole trickle tail.

---

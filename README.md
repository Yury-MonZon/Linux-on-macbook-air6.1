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
sudo sed -i -E 's/^#?MemorySleepMode=.*/MemorySleepMode=s2idle/' /etc/systemd/sleep.conf
sudo sed -i -E 's/^#?HibernateDelaySec=.*/HibernateDelaySec=15min/' /etc/systemd/sleep.conf

sudo sed -i -E 's/^#?HandleLidSwitch=.*/HandleLidSwitch=suspend-then-hibernate/' /etc/systemd/logind.conf
```

Kernel cmdline additions go in **`/etc/default/limine` `KERNEL_CMDLINE`, not `/etc/kernel/cmdline`** (this system's `limine-entry-tool` ignores that file once `KERNEL_CMDLINE[default]` is set; run `sudo limine-update` after any cmdline edit to rebuild both initramfs images + `limine.conf`):

```bash
for param in \
  'rtc_cmos.use_acpi_alarm=1' \
  'acpi_sleep=nonvs' \
  'button.lid_init_state=open' \
  'i915.enable_dc=0 i915.enable_fbc=0 i915.enable_psr=0' \
  'nvme_core.default_ps_max_latency_us=0 nvme.noacpi=1' ; do
  grep -qF "$param" /etc/default/limine || \
    echo "KERNEL_CMDLINE[default]+=\" $param\"" | sudo tee -a /etc/default/limine
done
sudo limine-update
```

(`rtc_cmos.use_acpi_alarm=1` is THE critical fix, see below.)

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

```bash
sudo tee /etc/systemd/system-sleep/facetimehd > /dev/null << 'EOF'
#!/bin/bash
case $1 in
  pre)  modprobe -r facetimehd 2>/dev/null ;;
  post) modprobe facetimehd 2>/dev/null ;;
esac
EOF
sudo chmod +x /etc/systemd/system-sleep/facetimehd
```

(unloads/reloads the camera driver around every sleep transition: the driver failing to cleanly re-suspend right after its own fragile s2idle-resume firmware reinit was the root cause of a genuine kernel freeze at hibernation-entry, requiring a hard cold boot to recover)

```bash
sudo tee /etc/systemd/system-sleep/disable-d3cold > /dev/null << 'EOF'
#!/bin/sh
echo 0 > /sys/bus/pci/devices/*/d3cold_allowed 2>/dev/null || true
EOF
sudo chmod +x /etc/systemd/system-sleep/disable-d3cold
```

```bash
systemctl disable NetworkManager-wait-online.service   # minor boot-time win
```

**Why this exact config:** the root cause of the old "intermittent suspend-then-hibernate" problem was an RTC wakealarm handshake bug between systemd-sleep and the `rtc_cmos` driver: it would self-retrigger every ~102 seconds (sometimes 18+ times in a row) instead of waiting out `HibernateDelaySec`, before eventually settling into a real hibernate. `rtc_cmos.use_acpi_alarm=1` makes the `rtc_cmos` driver arm/read the wake alarm through ACPI-mediated methods instead of raw legacy CMOS register pokes, letting firmware stay in the loop for the whole handshake. This fixed it, validated across many full lid-close→suspend→hibernate→resume cycles with zero retrigger-loop recurrences. XHC1 disable, `nonvs`, `button.lid_init_state=open`, and the i915 power-saving disables are separate, complementary fixes for other quirks found the same session (see `cachyos_patches_applied.txt` for the full research trail and citations). The facetimehd hook fixes a *different*, unrelated genuine kernel freeze (not the retrigger loop).

**Do NOT set `MemorySleepMode=deep`.** Tested directly: kernel-level suspend/resume completes cleanly, but the screen does not wake on lid-open, requiring a power-button press to restore the display. This is the documented black-screen-on-resume symptom for real S3 on Mac hardware, not folklore.

**Known, long-standing, unfixed limitation** (not touched by any of the above, don't chase it as a new bug): opening the lid does **not** wake this machine from s2idle suspend. The lid event genuinely is received at the ACPI level, but under s2idle the kernel only treats an SCI as a real wake if it came from a GPE armed specifically for wake, and this one isn't, so the event is processed without ending the s2idle loop. Enabling EC/LID0/SPIT in `/proc/acpi/wakeup` does not change this. **The actual workflow: open the lid, then tap a key.** The keyboard wakes it fine (goes via xHCI, a real armed wake source). Waking from a genuinely *completed* hibernate (not just suspend) always requires the power button on this hardware too. This is also long-standing and also not fixable here (neither RTC nor the lid switch has a valid S4/power-on wake path, only S3).

Separately, the physical lid switch itself sometimes sends genuine spurious Lid-closed/Lid-opened ACPI events at short (30-90s) intervals with no user interaction; root cause not identified (hinge/sensor wear vs. firmware quirk). Currently just tolerated as-is.

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
   DefinitionBlock ("", "SSDT", 2, "LOCAL", "BATBIX", 0x00000001)
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

---


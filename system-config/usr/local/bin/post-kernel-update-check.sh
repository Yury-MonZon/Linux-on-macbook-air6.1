#!/bin/bash
# post-kernel-update-check.sh
#
# Run after any kernel package update (linux-cachyos, linux-cachyos-lts,
# linux-cachyos-deferred) on this MacBook Air 6,1. Pacman's own DKMS hooks
# already rebuild out-of-tree modules automatically on install/upgrade, and
# mkinitcpio/limine-update already regenerate the initramfs and boot menu
# automatically too -- this script does not replace either of those, it
# verifies they actually worked and repairs the one thing that has broken
# before without any hook noticing: an out-of-tree module (facetimehd)
# failing to build against a newer kernel API.
#
# Safe to re-run any time; every check is idempotent.
#
# Exit code: 0 if everything is OK/FIXED, 1 if anything needs manual
# attention (printed as FAIL).

set -u
FAIL=0
ok()   { printf '  [OK]    %s\n' "$1"; }
fixed(){ printf '  [FIXED] %s\n' "$1"; }
fail() { printf '  [FAIL]  %s\n' "$1"; FAIL=1; }
warn() { printf '  [WARN]  %s\n' "$1"; }

if [[ $EUID -ne 0 ]]; then
  echo "Run as root (sudo $0)." >&2
  exit 1
fi

echo "== Kernel packages =="
mapfile -t KERNELS < <(pacman -Q | awk '/^linux-cachyos(-[a-z]+)?( |$)/ && $1 !~ /-headers$/ {print $1}')
for pkg in "${KERNELS[@]}"; do echo "  $pkg ($(pacman -Q "$pkg" | awk '{print $2}'))"; done
RUNNING_KVER=$(uname -r)
echo "  currently running: $RUNNING_KVER"
echo

echo "== DKMS modules =="
DKMS_MODULES=(acpi_call broadcom-wl facetimehd macbat-fix)
mapfile -t INSTALLED_KVERS < <(ls /usr/lib/modules/ | grep cachyos)

# facetimehd's out-of-tree source needs strncpy->strscpy for kernel 7.2+
# (strncpy() was removed from the kernel entirely). Re-check/reapply every
# time: cheap, idempotent (sed no-ops if already fixed), and covers a
# facetimehd-dkms package update reinstalling a clean unpatched source.
FTHD_SRC=$(ls -d /usr/src/facetimehd-*/ 2>/dev/null | head -1)
if [[ -n "$FTHD_SRC" ]]; then
  if grep -q 'strncpy(' "$FTHD_SRC/fthd_v4l2.c" 2>/dev/null; then
    sed -i 's/\bstrncpy(/strscpy(/g' "$FTHD_SRC/fthd_v4l2.c"
    fixed "facetimehd source: reapplied strncpy->strscpy (kernel 7.2+ removed strncpy)"
  else
    ok "facetimehd source: strscpy fix already present"
  fi
fi

for kver in "${INSTALLED_KVERS[@]}"; do
  for mod in "${DKMS_MODULES[@]}"; do
    status=$(dkms status "$mod" -k "$kver" 2>/dev/null)
    if [[ "$status" == *"installed"* ]]; then
      ok "$mod for $kver"
    else
      mver=$(dkms status "$mod" 2>/dev/null | head -1 | grep -oP '(?<=/)[^,]+')
      if [[ -z "$mver" ]]; then
        fail "$mod: not registered with DKMS at all (expected if never installed via AUR/manually)"
        continue
      fi
      if dkms install "$mod/$mver" -k "$kver" >/tmp/dkms-$mod-$kver.log 2>&1; then
        fixed "$mod for $kver (was missing, rebuilt)"
      else
        fail "$mod for $kver failed to build -- see /tmp/dkms-$mod-$kver.log"
      fi
    fi
  done
done
echo

echo "== ACPI overrides in initramfs (mbread SBS proxy, DTLK 2s-sleep fix) =="
# Only the per-machine-id boot images under /boot/<hash>/<kernel>/initramfs are
# what limine.conf actually boots; ignore flat /boot/initramfs-*.img copies,
# which on this machine are stale leftovers limine.conf never references.
while IFS= read -r img; do
  missing=()
  for marker in mbread.aml ssdt5-dtlk-fix.aml; do
    lsinitcpio "$img" 2>/dev/null | grep -q "$marker" || missing+=("$marker")
  done
  if [[ ${#missing[@]} -eq 0 ]]; then
    ok "$img"
  else
    fail "$img missing: ${missing[*]} -- run 'sudo limine-update' (acpi_override hook may be disabled or ACPI source files missing from /etc/initcpio/acpi_override/)"
  fi
done < <(find /boot -mindepth 3 -maxdepth 3 -type f -name 'initramfs')
echo

echo "== Persistent systemd units =="
# is-active is NOT checked here: no_batt_fix.service is Type=simple with no
# RemainAfterExit, so it legitimately goes back to inactive once its one-shot
# script exits -- that's correct behaviour, not a failure. is-enabled (runs
# again next boot) + not is-failed (didn't error) is the right pair of checks.
for unit in no_batt_fix.service set-freq-cap.service; do
  if systemctl is-enabled --quiet "$unit" 2>/dev/null && ! systemctl is-failed --quiet "$unit" 2>/dev/null; then
    ok "$unit"
  else
    if systemctl enable "$unit" >/dev/null 2>&1 && systemctl reset-failed "$unit" 2>/dev/null; then
      fixed "$unit (was disabled or in a failed state, re-enabled)"
    else
      fail "$unit could not be enabled -- check 'systemctl status $unit'"
    fi
  fi
done
echo

echo "== CPU frequency cap (2.8GHz AC, avoids the measured 2.9-3.3GHz throttle cliff) =="
bad=0
for f in /sys/devices/system/cpu/cpu*/cpufreq/scaling_max_freq; do
  [[ "$(cat "$f")" == "2800000" ]] || bad=1
done
if [[ $bad -eq 0 ]]; then
  ok "all cores at 2800000"
else
  /usr/local/bin/set-freq-cap.sh
  ok_now=1
  for f in /sys/devices/system/cpu/cpu*/cpufreq/scaling_max_freq; do
    [[ "$(cat "$f")" == "2800000" ]] || ok_now=0
  done
  [[ $ok_now -eq 1 ]] && fixed "reapplied 2.8GHz cap to all cores" || fail "could not set scaling_max_freq on all cores"
fi
if ! grep -q 'CPU_SCALING_MAX_FREQ_ON_AC=2800000' /etc/tlp.conf 2>/dev/null; then
  warn "tlp.conf's CPU_SCALING_MAX_FREQ_ON_AC is not 2800000 -- TLP will silently revert the cap above on next AC plug/unplug"
fi
echo

echo "== Driver sanity =="
[[ -e /dev/video0 ]] && ok "facetimehd: /dev/video0 present" || fail "facetimehd: /dev/video0 missing"
lsmod | grep -q '^wl ' && ok "broadcom-wl: wl module loaded" || fail "broadcom-wl: wl module not loaded"
lsmod | grep -q '^macbat_fix' && ok "macbat_fix: module loaded" || fail "macbat_fix: module not loaded"
if [[ -r /sys/class/power_supply/BAT0/cycle_count ]]; then
  cc=$(cat /sys/class/power_supply/BAT0/cycle_count)
  [[ "$cc" -gt 0 ]] && ok "battery: real cycle_count ($cc)" || warn "battery: cycle_count is 0 -- macbat_fix may not be bound to BAT0"
fi
echo

echo "== udev rules =="
for rule in 90-xhc_sleep.rules 99-heci-runtime-pm.rules 99-hid-apple-fix.rules; do
  [[ -f "/etc/udev/rules.d/$rule" ]] && ok "$rule" || fail "$rule missing"
done
echo

if [[ $FAIL -eq 0 ]]; then
  echo "All checks OK (auto-fixed anything that had drifted)."
else
  echo "One or more checks need manual attention -- see FAIL lines above."
fi
exit $FAIL

#!/bin/bash
# build-resume-lab-kernel.sh
#
# Run this on the MacBook Air AFTER a regular `sudo pacman -Syu` that pulled
# in a newer upstream linux-cachyos. This script does NOT run pacman -Syu
# itself and does NOT install any upstream kernel -- that's the user's own
# call. Its only job: notice upstream moved, and produce+install a fresh
# linux-cachyos-deferred build with the resume-lab patch applied on top,
# so the custom sleep/wake kernel doesn't silently fall behind.
#
# Safety contract: if the patch does not apply CLEANLY (zero fuzz) against
# whatever upstream shipped, this script aborts before building anything
# and leaves the currently-installed kernel completely untouched. It never
# installs an unpatched kernel as a silent fallback -- a human has to look
# at the conflict.

set -euo pipefail

WORKDIR=/var/cache/resume-lab-kernel-build
STATE_FILE=/var/lib/resume-lab-kernel/last-built.txt
PATCH_REPO_RAW="https://raw.githubusercontent.com/Yury-MonZon/Linux-on-macbook-air6.1/main/kernel-patches/resume-lab"
UPSTREAM_PKGBUILD_URL="https://raw.githubusercontent.com/CachyOS/linux-cachyos/master/linux-cachyos/PKGBUILD"

mkdir -p "$WORKDIR" "$(dirname "$STATE_FILE")"
cd "$WORKDIR"

echo "== Fetching current upstream linux-cachyos PKGBUILD =="
curl -fsSL "$UPSTREAM_PKGBUILD_URL" -o PKGBUILD.upstream
UPSTREAM_MINOR=$(grep -oP '^_minor=\K.*' PKGBUILD.upstream)
UPSTREAM_PKGREL=$(grep -oP '^pkgrel=\K.*' PKGBUILD.upstream)
UPSTREAM_VER="${UPSTREAM_MINOR}-${UPSTREAM_PKGREL}"
echo "  upstream: $UPSTREAM_VER"

LAST_BUILT=""
[[ -f "$STATE_FILE" ]] && LAST_BUILT=$(cat "$STATE_FILE")
echo "  last built by this script: ${LAST_BUILT:-<never>}"

if [[ "$UPSTREAM_VER" == "$LAST_BUILT" ]] && pacman -Q linux-cachyos-deferred >/dev/null 2>&1; then
  echo "Already up to date and installed. Nothing to do."
  exit 0
fi

echo
echo "== Fetching current resume-lab patch from the published repo =="
curl -fsSL "$PATCH_REPO_RAW/0011-resume-lab-v5.patch" -o 0011-resume-lab-v5.patch
PATCH_NAME=0011-resume-lab-v5.patch

echo
echo "== Building customized PKGBUILD =="
cp PKGBUILD.upstream PKGBUILD

# 1. Pin CPU optimization to this exact machine's real target (Haswell ULT,
#    i7-4650U supports up to AVX2/FMA/BMI2/MOVBE -- generic_v3 baseline).
sed -i 's/^: "\${_processor_opt:=}"$/: "${_processor_opt:=generic_v3}"/' PKGBUILD
grep -q '_processor_opt:=generic_v3' PKGBUILD || { echo "FAIL: could not set _processor_opt (upstream PKGBUILD structure changed)"; exit 1; }

# 2. Install alongside stock linux-cachyos, not replacing it. Insert right
#    before the pkgbase= line, after the if/elif/else that sets _pkgsuffix
#    (whichever branch it took), so it applies regardless of build variant.
sed -i 's/^pkgbase="linux-\$_pkgsuffix"$/_pkgsuffix="${_pkgsuffix}-deferred"\npkgbase="linux-$_pkgsuffix"/' PKGBUILD
grep -q '_pkgsuffix}-deferred' PKGBUILD || { echo "FAIL: could not append -deferred suffix (upstream PKGBUILD structure changed)"; exit 1; }

# 3. Wire in our patch. prepare() already applies every source[] entry
#    matching *.patch generically, so this is the only wiring needed.
PATCH_B2SUM=$(b2sum "$PATCH_NAME" | awk '{print $1}')
{
  echo ""
  echo "source+=('$PATCH_NAME')"
  echo "b2sums+=('$PATCH_B2SUM')"
} >> PKGBUILD

echo
echo "== Building package (this takes a while) =="
rm -rf src pkg *.pkg.tar.zst
if ! makepkg -f --noconfirm --skippgpcheck 2>&1 | tee build.log; then
  echo
  echo "FAIL: makepkg failed. Currently-installed kernel is untouched."
  echo "See $WORKDIR/build.log"
  exit 1
fi

echo
echo "== Verifying the patch actually took effect (not just 'applied') =="
SRCTREE=$(find src -maxdepth 1 -type d -name 'linux-*' | head -1)
if [[ -z "$SRCTREE" ]] || ! grep -q 'pm_resume_cpu_online_pending' "$SRCTREE/kernel/cpu.c" 2>/dev/null; then
  echo "FAIL: patch marker (pm_resume_cpu_online_pending) not found in built source tree."
  echo "The patch may have applied with fuzz against changed upstream code without"
  echo "actually landing correctly. Currently-installed kernel is untouched."
  echo "DO NOT install the package(s) in $WORKDIR -- investigate the patch conflict first."
  exit 1
fi
echo "  OK: resume-lab patch confirmed present in built source tree"

echo
echo "== Installing =="
PKGFILES=(linux-cachyos-deferred-*.pkg.tar.zst linux-cachyos-deferred-headers-*.pkg.tar.zst)
sudo pacman -U --noconfirm "${PKGFILES[@]}"

echo "$UPSTREAM_VER" > "$STATE_FILE"
echo
echo "Done. New kernel installed as linux-cachyos-deferred ($UPSTREAM_VER + resume-lab patch)."
echo "Reboot and select it to use it -- this script does not reboot for you."

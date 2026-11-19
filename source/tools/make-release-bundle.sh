#!/bin/bash
# make-release-bundle.sh — assemble the flat, no-toolchain release bundle.
#
# The repo commits ONE copy of each binary (source/boot/dl/). The flat bundle
# that INSTALL.md documents (kernel + initramfs + data.img next to boot-test.sh)
# is GENERATED from those, never committed: a second 10.8 MB copy of the kernel
# and an 8 MB factory disk image in the repo is 19 MB of duplication for a
# bundle that can be rebuilt in two seconds.
#
# Usage:
#   bash tools/make-release-bundle.sh [outdir]     # default: dist/aios-bundle
#   bash tools/make-release-bundle.sh --zip        # also write dist/aios-v1.1.zip
#
# Requires: a built image (make -f boot/Makefile boot-image) and python3
# (only here, for mkfs_ext2.py — the end user of the bundle needs neither).
set -euo pipefail
cd "$(dirname "$0")/.."

OUT=${1:-dist/aios-bundle}
[[ "$OUT" == --zip ]] && { OUT=dist/aios-bundle; ZIP=1; }

KERNEL=boot/dl/kernel/boot/vmlinuz-virt
INITRD=boot/build/aios-initramfs.cpio.xz

[ -f "$KERNEL" ] || { echo "[bundle] missing $KERNEL (run bash boot/fetch.sh)" >&2; exit 1; }
[ -f "$INITRD" ] || { echo "[bundle] missing $INITRD (run make -f boot/Makefile boot-image)" >&2; exit 1; }

rm -rf "$OUT"; mkdir -p "$OUT"
cp "$KERNEL" "$OUT/vmlinuz-virt"
cp "$INITRD" "$OUT/aios-initramfs.cpio.xz"

# Factory data disk, formatted here so the bundle user needs no mkfs.
python3 tools/mkfs_ext2.py "$OUT/data.img" >/dev/null
cp tools/mkfs_ext2.py "$OUT/mkfs_ext2.py"      # kept: boot-test.sh regenerates

# The three scripts the flat layout expects, plus the docs a user reads.
cp boot/boot-test.sh boot/boot-gui-test.sh boot/console-test.sh "$OUT"/
chmod +x "$OUT"/*.sh

if [ -f boot/run-qemu.sh ]; then cp boot/run-qemu.sh "$OUT"/; chmod +x "$OUT/run-qemu.sh"; fi

for doc in BOOT-GUIDE.md INSTALL.md; do
    [ -f "$doc" ] && cp "$doc" "$OUT/"
done
cp ../LICENSE "$OUT"/

if [ "${ZIP:-0}" = 1 ]; then
    (cd dist && zip -qr aios-v1.1.zip aios-bundle)
    echo "[bundle] wrote dist/aios-v1.1.zip ($(du -h dist/aios-v1.1.zip | cut -f1))"
fi

echo "[bundle] ready: $OUT"
du -sh "$OUT"
ls "$OUT"

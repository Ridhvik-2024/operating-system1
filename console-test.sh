#!/bin/bash
# console-test.sh — boots AI-OS v1.1 interactively (no aios.mode=test),
# drives the aiosh console over serial with scripted input, and asserts
# that the shell actually answers. This validates the real user path,
# including the M8 durable status command.
set -u
# repo root, or the flat release bundle root (kernel sits next to us)
SD="$(cd "$(dirname "$0")" && pwd)"
if [ -f "$SD/vmlinuz-virt" ]; then cd "$SD"; else cd "$SD/.."; fi

QEMU=${AIOS_QEMU:-qemu-system-x86_64}
QL=()
[ -d /tmp/qemu-root/usr/share/seabios ] && QL+=(-L /tmp/qemu-root/usr/share/seabios -L /tmp/qemu-root/usr/share/qemu)
LDLP=""
[ -d /tmp/qemu-root/usr/lib/x86_64-linux-gnu ] && LDLP="/tmp/qemu-root/usr/lib/x86_64-linux-gnu:/tmp/qemu-root/usr/lib"

mkdir -p boot/build
KERNEL=boot/dl/kernel/boot/vmlinuz-virt
INITRD=boot/build/aios-initramfs.cpio.xz
if [ ! -f "$KERNEL" ] && [ -f vmlinuz-virt ]; then
    KERNEL=vmlinuz-virt
    INITRD=aios-initramfs.cpio.xz
fi

# M8: make sure a data disk exists (shipped in bundles, mkfs'd in-repo)
DATAIMG=""
for CAND in data.img boot/build/data.img; do
    [ -f "$CAND" ] && { DATAIMG="$CAND"; break; }
done
if [ -z "$DATAIMG" ] && [ -f tools/mkfs_ext2.py ]; then
    python3 tools/mkfs_ext2.py boot/build/data.img >/dev/null && DATAIMG=boot/build/data.img
elif [ -z "$DATAIMG" ] && [ -f mkfs_ext2.py ]; then
    python3 mkfs_ext2.py data.img >/dev/null && DATAIMG=data.img
fi
DQ=()
[ -n "$DATAIMG" ] && DQ+=(-drive "file=$DATAIMG,format=raw,if=virtio")

LOG=boot/build/console-test.log
rm -f "$LOG"

# feed commands slowly; keep stdin open long enough for the boot + shell
{
    sleep 12
    printf 'version\r'
    sleep 2
    printf '6*7\r'
    sleep 2
    printf 'dur\r'
    sleep 2
    printf 'lm classify fix the console asap\r'
    sleep 2
    printf 'lm gen about the scheduler\r'
    sleep 2
    printf 'recall AI-OS\r'
    sleep 3
    printf 'kernel\r'
    sleep 3
    printf 'autopilot\r'
    sleep 3
} | if [ -n "$LDLP" ]; then
    LD_LIBRARY_PATH="$LDLP" timeout 45 "$QEMU" "${QL[@]}" \
        -machine accel=tcg -m 512M -smp 2 \
        -kernel "$KERNEL" \
        -initrd "$INITRD" \
        "${DQ[@]}" \
        -append "console=ttyS0 rdinit=/init" \
        -display none -serial stdio -monitor none -no-reboot -nic none \
        > "$LOG" 2>&1
else
    timeout 45 "$QEMU" "${QL[@]}" \
        -machine accel=tcg -m 512M -smp 2 \
        -kernel "$KERNEL" \
        -initrd "$INITRD" \
        "${DQ[@]}" \
        -append "console=ttyS0 rdinit=/init" \
        -display none -serial stdio -monitor none -no-reboot -nic none \
        > "$LOG" 2>&1
fi

echo "---- console transcript (tail) ----"
tail -35 "$LOG"
echo "-----------------------------------"

fail=0
have() { grep -q "$1" "$LOG" 2>/dev/null && echo "  OK   $1" || { echo "  MISS $1"; fail=1; }; }
have 'AI-OS console ready'
have 'aiosh>'
have '1.1.0-m10 (v1.1 desktop)'
have '^42'
have 'durable: '
have 'class=urgent'
have 'kernel bridge:'
have 'autopilot: mode='
[ $fail -eq 0 ] && echo "[console-test] PASS" || echo "[console-test] FAIL"
exit $fail

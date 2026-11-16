#!/bin/bash
# run-qemu.sh — boot the AI-OS v1.0 image interactively in QEMU.
# The AI shell (aiosh) is your console. Exit the VM with: poweroff (busybox)
# or Ctrl-A X (kill QEMU).
#
# M8 durable: a data.img next to the kernel (or in the repo root) is
# attached as the ext2 side disk — memory, jobs, audit and autopilot
# state survive reboots across runs. Delete data.img for a factory reset.
#
# QEMU resolution: $AIOS_QEMU, else qemu-system-x86_64 from PATH.
# Extra ROM search dirs are added automatically for locally extracted QEMU.
set -euo pipefail
# repo root, or the flat release bundle root (kernel sits next to us)
SD="$(cd "$(dirname "$0")" && pwd)"
if [ -f "$SD/vmlinuz-virt" ]; then cd "$SD"; else cd "$SD/.."; fi

KERNEL=boot/dl/kernel/boot/vmlinuz-virt
INITRD=boot/build/aios-initramfs.cpio.xz
# flat release bundle: kernel + initramfs sit next to the script
if [ ! -f "$KERNEL" ] && [ -f vmlinuz-virt ]; then
    KERNEL=vmlinuz-virt
    INITRD=aios-initramfs.cpio.xz
fi
MEM=${AIOS_MEM:-512M}

QEMU=${AIOS_QEMU:-qemu-system-x86_64}
QL=()
if [[ -n "${AIOS_QEMU_SHARE:-}" ]]; then QL+=(-L "$AIOS_QEMU_SHARE"); fi
# auto-detect the no-root extracted toolchain used by boot-test
if [[ -d /tmp/qemu-root/usr/share/seabios ]]; then
    QL+=(-L /tmp/qemu-root/usr/share/seabios -L /tmp/qemu-root/usr/share/qemu)
fi

# M8 durable side disk: attach when present (created by mkfs_ext2.py,
# shipped in the release bundle, or left from a previous session)
DATA=()
for CAND in data.img boot/build/data.img; do
    if [ -f "$CAND" ]; then
        DATA=(-drive "file=$CAND,format=raw,if=virtio")
        break
    fi
done

exec "$QEMU" "${QL[@]}" \
    -machine accel=tcg -m "$MEM" -smp 2 \
    -kernel "$KERNEL" -initrd "$INITRD" \
    "${DATA[@]}" \
    -append "console=ttyS0 rdinit=/init ${AIOS_CMDLINE:-}" \
    -nographic -no-reboot -nic none

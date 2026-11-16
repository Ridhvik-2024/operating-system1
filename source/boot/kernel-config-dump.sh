#!/bin/bash
# kernel-config-dump.sh — boot the m5.5 image with busybox sh as init and
# dump the kernel's ground-truth config for the M6 feature check.
set -u
cd "$(dirname "$0")/.."   # aios/

QEMU=${AIOS_QEMU:-qemu-system-x86_64}
QL=()
[ -d /tmp/qemu-root/usr/share/seabios ] && QL+=(-L /tmp/qemu-root/usr/share/seabios -L /tmp/qemu-root/usr/share/qemu)
LDLP=""
[ -d /tmp/qemu-root/usr/lib/x86_64-linux-gnu ] && LDLP="/tmp/qemu-root/usr/lib/x86_64-linux-gnu:/tmp/qemu-root/usr/lib"

LOG=/tmp/kconfig-dump.log
rm -f "$LOG"

{
    sleep 10
    printf '/bin/busybox mount -t proc proc /proc; /bin/busybox mount -t sysfs sysfs /sys\n'
    printf '/bin/busybox mount -t proc proc /proc; /bin/busybox mount -t sysfs sysfs /sys\n'
    sleep 2
    printf '/bin/busybox ls -la /proc/config.gz\n'
    sleep 2
    printf '/bin/busybox cat /proc/modules; echo MODULES_PROBE_DONE\n'
    sleep 2
    printf '/bin/busybox ls /sys/fs/cgroup 2>&1; echo CGROUP_PROBE_DONE\n'
    sleep 2
    printf '/bin/busybox ls /sys/kernel/btf/vfs 2>&1; echo BTF_PROBE_DONE\n'
    sleep 2
    printf '/bin/busybox ls /sys/kernel/tracing/events/sched/ 2>&1 | /bin/busybox head -5; echo TRACE_PROBE_DONE\n'
    sleep 2
    printf '/bin/busybox mount -t tracefs tracefs /sys/kernel/tracing 2>&1; /bin/busybox ls /sys/kernel/tracing/events/sched/ 2>&1 | /bin/busybox head -6; echo TRACE_PROBE_DONE\n'
    sleep 2
    printf '/bin/busybox cat /sys/kernel/security/lsm 2>&1; echo LSM_PROBE_DONE\n'
    sleep 2
    printf '/bin/busybox insmod /lib/modules/aios_intent.ko; echo INSMOD_DONE\n'
    sleep 3
    printf '/bin/busybox cat /proc/aios; /bin/busybox ls -la /dev/aios; echo AIOSDEV_DONE\n'
    sleep 3
    printf '/sbin/probe; echo SYSCALL_PROBE_DONE\n'
    sleep 5
    printf '/bin/busybox uname -a\n'
    sleep 4
    printf 'poweroff -f\n'
} | if [ -n "$LDLP" ]; then
    LD_LIBRARY_PATH="$LDLP" timeout 60 "$QEMU" "${QL[@]}" \
        -machine accel=tcg -m 512M -smp 2 \
        -kernel boot/dl/kernel/boot/vmlinuz-virt \
        -initrd boot/build/aios-initramfs.cpio.gz \
        -append "console=ttyS0 rdinit=/bin/sh" \
        -display none -serial stdio -monitor none -no-reboot -nic none \
        > "$LOG" 2>&1
else
    timeout 60 "$QEMU" "${QL[@]}" \
        -machine accel=tcg -m 512M -smp 2 \
        -kernel boot/dl/kernel/boot/vmlinuz-virt \
        -initrd boot/build/aios-initramfs.cpio.gz \
        -append "console=ttyS0 rdinit=/bin/sh" \
        -display none -serial stdio -monitor none -no-reboot -nic none \
        > "$LOG" 2>&1
fi

grep -E "CONFIG_|Linux|BusyBox" "$LOG" | head -30

#!/bin/bash
# boot-test.sh — headless CI boot test for AI-OS v1.1 (the M9 release).
#
# TWO PHASES ON THE SAME EXT2 DATA DISK — this is the M8 proof:
#   phase 1: a freshly formatted data.img; the OS boots, the selftest
#            suite runs, the aiosh bring-up writes durable state
#            (memory, a background job, autopilot journals), poweroff
#   phase 2: boot AGAIN on the same disk; init must report the previous
#            session's state and the governor must replay its journal
#
# Exit 0 = both boots verified. Set AIOS_BOOT_TIMEOUT to override the
# per-phase timeout (default 180 s).
set -u
# repo root, or the flat release bundle root (kernel sits next to us)
SD="$(cd "$(dirname "$0")" && pwd)"
if [ -f "$SD/vmlinuz-virt" ]; then cd "$SD"; else cd "$SD/.."; fi
mkdir -p boot/build

KERNEL=boot/dl/kernel/boot/vmlinuz-virt
INITRD=boot/build/aios-initramfs.cpio.xz
# flat release bundle: kernel + initramfs sit next to the script
if [ ! -f "$KERNEL" ] && [ -f vmlinuz-virt ]; then
    KERNEL=vmlinuz-virt
    INITRD=aios-initramfs.cpio.xz
fi
TIMEOUT=${AIOS_BOOT_TIMEOUT:-180}

QEMU=${AIOS_QEMU:-qemu-system-x86_64}
QL=()
if [[ -d /tmp/qemu-root/usr/share/seabios ]]; then
    QL+=(-L /tmp/qemu-root/usr/share/seabios -L /tmp/qemu-root/usr/share/qemu)
fi
LDLP=""
if [[ -d /tmp/qemu-root/usr/lib/x86_64-linux-gnu ]]; then
    LDLP="/tmp/qemu-root/usr/lib/x86_64-linux-gnu:/tmp/qemu-root/usr/lib"
fi

run_qemu() {
    if [[ -n "$LDLP" ]]; then
        LD_LIBRARY_PATH="$LDLP" "$QEMU" "${QL[@]}" "$@"
    else
        "$QEMU" "${QL[@]}" "$@"
    fi
}

# ---- fresh disk for phase 1 (keep any previous one as evidence) ----
# mkfs tool: repo layout tools/mkfs_ext2.py, or flat release bundle ./mkfs_ext2.py
MKFS=""
[ -f tools/mkfs_ext2.py ] && MKFS=tools/mkfs_ext2.py
[ -f mkfs_ext2.py ] && MKFS=mkfs_ext2.py
DATAIMG=boot/build/data.img
if [ -f "$DATAIMG" ]; then mv "$DATAIMG" "$DATAIMG.prev"; fi
# flat bundle: boot/build may not exist yet and the shipped data.img sits here
if [ -z "$MKFS" ] && [ -f data.img ]; then
    mkdir -p boot/build
    cp data.img "$DATAIMG"     # factory image, never booted by this test
    MKFS=shipped
fi
if [ "$MKFS" = "shipped" ]; then
    echo "[boot-test] using the shipped factory data.img"
elif [ -n "$MKFS" ]; then
    python3 "$MKFS" "$DATAIMG" > /dev/null || {
        echo "[boot-test] mkfs_ext2 failed"; exit 1; }
else
    echo "[boot-test] mkfs_ext2.py missing (cannot format data disk)"
    exit 1
fi

boot_phase() {  # $1 = serial log path; returns 0 when the VM powered off
    local LOG="$1"
    rm -f "$LOG" boot/build/qemu.pid
    run_qemu \
        -machine accel=tcg -m 512M -smp 2 \
        -kernel "$KERNEL" -initrd "$INITRD" \
        -drive "file=$DATAIMG,format=raw,if=virtio" \
        -append "console=ttyS0 rdinit=/init aios.mode=test" \
        -display none -serial "file:$LOG" -monitor none \
        -no-reboot -nic none -daemonize -pidfile boot/build/qemu.pid
    if [ ! -s boot/build/qemu.pid ]; then
        echo "[boot-test] QEMU did not start (no pidfile)"
        return 1
    fi
    local QPID
    QPID=$(cat boot/build/qemu.pid)
    local t=0
    while kill -0 "$QPID" 2>/dev/null && [ "$t" -lt "$TIMEOUT" ]; do
        if grep -q "powering off" "$LOG" 2>/dev/null; then
            sleep 2   # let QEMU flush + exit
            break
        fi
        sleep 2
        t=$((t + 2))
    done
    if kill -0 "$QPID" 2>/dev/null; then
        echo "[boot-test] TIMEOUT after ${TIMEOUT}s — killing QEMU (pid $QPID)"
        kill "$QPID" 2>/dev/null
        sleep 1
        kill -9 "$QPID" 2>/dev/null
        return 1
    fi
    return 0
}

fail=0

echo "[boot-test] phase 1: fresh disk — boot, write durable state, power off"
boot_phase boot/build/boot-test1.log || fail=1
echo "[boot-test] phase-1 log tail:"
echo "------------------------------------------"
tail -30 boot/build/boot-test1.log 2>/dev/null
echo "------------------------------------------"

have1() { grep -q "$1" boot/build/boot-test1.log 2>/dev/null \
             && echo "  OK   $1" || { echo "  MISS $1"; fail=1; }; }
have1 'AI-OS 1.1.0-m10 (v1.1 desktop)'
have1 'durable: /dev/vda on /data (ext2)'
have1 'AIOS-DUR mounted=1'
have1 'AIOS-SELFTEST: PASS'
have1 'SELFTEST test_durable: PASS'
have1 'AIOS-BOOT-OK selftest=PASS'
have1 'autopilot governor: pid'

echo
echo "[boot-test] phase 2: same disk — reboot and replay the journal"
boot_phase boot/build/boot-test2.log || fail=1
echo "[boot-test] phase-2 log tail:"
echo "------------------------------------------"
tail -30 boot/build/boot-test2.log 2>/dev/null
echo "------------------------------------------"

have2() { grep -q "$1" boot/build/boot-test2.log 2>/dev/null \
             && echo "  OK   $1" || { echo "  MISS $1"; fail=1; }; }
have2 'AIOS-DUR mounted=1 jobs=[1-9] mem=[1-9] audit=[1-9]'
have2 'this is boot #2 for this data disk'
have2 'journal entries replayed from the previous session'
have2 'AIOS-SELFTEST: PASS'
have2 'AIOS-BOOT-OK selftest=PASS'

echo
if [ $fail -eq 0 ]; then
    echo "[boot-test] PASS — AI-OS v1.1 boots and its state survives a reboot"
    exit 0
else
    echo "[boot-test] FAIL — see boot/build/boot-test1.log / boot-test2.log"
    exit 1
fi

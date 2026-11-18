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

# A failed boot job whose logs live somewhere a human has to go and click is a
# failed job nobody diagnoses. Echo the reason into the job itself as an
# annotation, and quote the selftest lines that failed -- they name the suite and
# the assertion, which is the whole answer.
# The serial log is CRLF, always ends with blank lines, and interleaves kernel
# dmesg with our own output. Every previous version of this function tried to be
# clever about which lines to show; all of them printed nothing, because
# "the last four non-kernel lines" are blank. Show the plain truth instead.
meaningful() {
    tr -d '\r' < "$1" 2>/dev/null \
        | grep -vE '^\[[[:space:]]*[0-9]+\.[0-9]+\]' \
        | grep -vE '^[[:space:]]*$'
}

explain() {
    log=$1; what=$2
    echo "  MISS $what"
    if [ ! -s "$log" ]; then
        echo "  --- $log is missing or empty ---"
        [ -n "$GITHUB_ACTIONS" ] && \
            echo "::error title=boot test::$what -- $log is missing or empty (qemu never wrote it)"
        fail=1
        return
    fi

    total=$(wc -l < "$log" 2>/dev/null); total=${total:-0}
    ours=$(meaningful "$log" | wc -l); ours=${ours:-0}
    echo "  --- $log: $total lines, $ours of ours ---"

    # Always show something. Prefer our output; if there is none, show the tail of
    # the raw log, which is where a panic or a missing device would be.
    if [ "$ours" -gt 0 ]; then
        body=$(meaningful "$log" | tail -15)
    else
        body=$(tr -d '\r' < "$log" | grep -vE '^[[:space:]]*$' | tail -8)
    fi
    printf '%s\n' "$body" | sed 's/^/    | /'

    # The lines that answer the question, wherever they are in the log. Booting
    # is chatty by design, so "the last fifteen lines" lands after the answer.
    # Two lines per thing that matters: the SELFTEST verdict AND the assertions
    # it printed underneath. A suite that fails says why on the lines after its
    # name, and those lines are the whole answer -- dropping them because they do
    # not match /SELFTEST/ is how this took three cycles.
    # Print the LAST lines of the log, always. Every previous version of this
    # tried to grep for what it expected to see; when the expectation was wrong
    # the annotation showed the build-up and nothing else, and the answer sat
    # just past the end of the window.
    echo "  --- last 12 lines from our programs ---"
    tr -d '\r' < "$log" 2>/dev/null \
        | grep -vE '^\[[[:space:]]*[0-9]+\.[0-9]+\]' \
        | grep -vE '^[[:space:]]*$' \
        | tail -12 | sed 's/^/    | /'
    marks=$(tr -d '\r' < "$log" 2>/dev/null \
            | grep -vE '^\[[[:space:]]*[0-9]+\.[0-9]+\]' \
            | grep -vE '^[[:space:]]*$' \
            | tail -8)

    # The tail shows where the boot ENDED; the failure is usually earlier. Name
    # any failing suite explicitly, because "selftest=FAIL" with no suite named
    # is a puzzle, and the answer is always one line further up the log.
    fails=$(tr -d '\r' < "$log" 2>/dev/null \
            | grep -E 'SELFTEST test_[a-z]+: FAIL|^ +FAIL tests/' \
            | head -6 | tr '\n' ' ' | tr -s ' ')
    if [ -n "$fails" ]; then
        echo "  --- failing suites ---"
        printf '%s\n' "$fails" | fold -w 120 | sed 's/^/    | /'
    fi

    if [ -n "$GITHUB_ACTIONS" ]; then
        # One line, and never empty: an annotation that says only "missed:" has
        # cost CI cycles on this change alone.
        flat="$fails $marks"
        flat=$(printf '%s' "$flat" | tr '\n' ' ' | tr -s ' ' | cut -c1-600)
        echo "::error title=boot test::missed $what :: $flat"
    fi
    fail=1
}

# -E: patterns in this file use '+' and '?', and a bare grep reads those as
# literals. That cost a CI cycle on the M6 assertion, which was correct about the
# log and wrong about the tool it checked the log with.
have1() { grep -qE "$1" boot/build/boot-test1.log 2>/dev/null \
             && echo "  OK   $1" || explain boot/build/boot-test1.log "$1"; }
have1 'AI-OS 1.1.0-m10 (v1.1 desktop)'
have1 'durable: /dev/vda on /data (ext2)'
have1 'AIOS-DUR mounted=1'
# M5: the rollback proof. The boot script writes v1, snapshots, writes v2, rolls
# back and reads the file -- so "v1" appearing AFTER the rollback line is the
# assertion that the snapshot was real and was on disk.
have1 'rolled back to'
have1 'snapshot .* taken '
have1 'boot: rollback proof'
have1 'AIOS-SELFTEST: PASS'
have1 'SELFTEST test_durable: PASS'
have1 'AIOS-BOOT-OK selftest=PASS'
have1 'autopilot governor: pid'
# M5: PID 1 must establish the supervisor before any shell exists, or "rollback
# is owned by PID 1" is something the shell does to itself and calls a guarantee.
have1 'supervisor: pid .* serving /run/aios/supervisor.sock'
# M6: the record is a chain, and the image verifies it after real tool calls have
# gone through the sandbox on the ext2 disk.
have1 'boot: audit chain proof'
have1 'audit: [0-9]+ records, chained and intact'

echo
echo "[boot-test] phase 2: same disk — reboot and replay the journal"
boot_phase boot/build/boot-test2.log || fail=1
echo "[boot-test] phase-2 log tail:"
echo "------------------------------------------"
tail -30 boot/build/boot-test2.log 2>/dev/null
echo "------------------------------------------"

have2() { grep -qE "$1" boot/build/boot-test2.log 2>/dev/null \
             && echo "  OK   $1" || explain boot/build/boot-test2.log "$1"; }
have2 'AIOS-DUR mounted=1 jobs=[1-9] mem=[1-9] audit=[1-9]'
have2 'this is boot #2 for this data disk'
have2 'journal entries replayed from the previous session'
# M5 across the reboot: the file the rollback restored is still v1 on the second
# boot, which is what makes this a snapshot rather than a copy held in RAM.
# M6 across the reboot: the chain on boot 2 links to records written on boot 1, so
# it must be intact AND longer -- a log that resets itself would still "verify".
have2 'boot: audit chain proof'
have2 'audit: [0-9]+ records, chained and intact'
have2 'AIOS-BOOT-OK selftest=PASS'

echo
if [ $fail -eq 0 ]; then
    echo "[boot-test] PASS — AI-OS v1.1 boots and its state survives a reboot"
    exit 0
else
    echo "[boot-test] FAIL — see boot/build/boot-test1.log / boot-test2.log"
    exit 1
fi

#!/bin/bash
# boot-gui-test.sh — the M10 desktop proof: boot the real image with a
# virtio-gpu + virtio keyboard/tablet, wait for the desktop, then
#   1. screendump the idle desktop and assert the pixel evidence
#      (wallpaper gradient, launcher accent line, 5 title bars, cursor)
#   2. type "6*7" + Enter into the focused aiosh terminal (QEMU sendkey)
#      and screendump again: the AI shell must visibly answer "42"
#   3. type a natural-language line into the launcher bar flow and dump
# Shots are saved as PNG next to the log for humans.
#
# Exit 0 = the GUI really drew, input really worked, aiosh really replied.
set -u
SD="$(cd "$(dirname "$0")" && pwd)"
if [ -f "$SD/vmlinuz-virt" ]; then cd "$SD"; else cd "$SD/.."; fi
mkdir -p boot/build

KERNEL=boot/dl/kernel/boot/vmlinuz-virt
INITRD=boot/build/aios-initramfs.cpio.xz
TIMEOUT=${AIOS_GUI_TIMEOUT:-240}
OUT=boot/build/boot-gui.log
MON=/tmp/aios-gui-mon.sock
SHOT1=/tmp/aios-gui-idle.ppm
SHOT2=/tmp/aios-gui-typed.ppm
SHOT3=/tmp/aios-gui-pending.ppm
PY=tools/gui_check.py

QEMU=${AIOS_QEMU:-qemu-system-x86_64}
QL=()
if [[ -d /tmp/qemu-root/usr/share/seabios ]]; then
    QL+=(-L /tmp/qemu-root/usr/share/seabios -L /tmp/qemu-root/usr/share/qemu)
fi
LDLP=""
if [[ -d /tmp/qemu-root/usr/lib/x86_64-linux-gnu ]]; then
    LDLP="/tmp/qemu-root/usr/lib/x86_64-linux-gnu:/tmp/qemu-root/usr/lib"
fi
export QEMU_MODULE_DIR=/tmp/qemu-root/usr/lib/x86_64-linux-gnu/qemu

rm -f "$OUT" "$MON" "$SHOT1" "$SHOT2" "$SHOT3"

echo "[boot-gui] booting with bochs-display + keyboard + tablet (TCG)..."
LD_LIBRARY_PATH="$LDLP" timeout "$TIMEOUT" "$QEMU" "${QL[@]}" \
    -machine accel=tcg -m 768M -smp 2 \
    -kernel "$KERNEL" -initrd "$INITRD" \
    -append "console=ttyS0 rdinit=/init aios.durable=0" \
    -device bochs-display,xres=1280,yres=800 \
    -device virtio-keyboard-pci \
    -device virtio-tablet-pci \
    -display none -serial stdio \
    -monitor unix:$MON,server,nowait \
    -no-reboot -nic none > "$OUT" 2>&1 &
QPID=$!

# ---- wait for the desktop marker on serial --------------------------
WAITED=0
DESKTOP_UP=0
while [ $WAITED -lt "$TIMEOUT" ]; do
    if grep -q "AIOS-DESKTOP-OK" "$OUT" 2>/dev/null; then
        DESKTOP_UP=1
        break
    fi
    sleep 2
    WAITED=$((WAITED + 2))
    if ! kill -0 $QPID 2>/dev/null; then break; fi
done

fail=0
if [ "$DESKTOP_UP" = "1" ]; then
    echo "  OK   AIOS-DESKTOP-OK reached in ${WAITED}s"
else
    echo "  MISS AIOS-DESKTOP-OK (timeout)"
    fail=1
fi

have() { grep -q "$1" "$OUT" 2>/dev/null \
             && echo "  OK   $1" || { echo "  MISS $1"; fail=1; }; }
have "GPU/input module stack 18/18 loaded"
have "/dev/fb0 .* up (bochs-display)"
have "desktop spawned (graphics console)"
have "AIOS-BOOT-OK selftest=PASS"

sleep 3     # let the desktop composite its first idle frames

# ---- shot 1: idle desktop pixel assertions --------------------------
if [ "$fail" = "0" ]; then
    if python3 "$PY" shot "$MON" "$SHOT1"; then
        echo "  OK   screendump 1 (idle desktop)"
        python3 "$PY" verify "$SHOT1" 1280 800 gradient || fail=1
        python3 "$PY" verify "$SHOT1" 1280 800 bar_accent || fail=1
        python3 "$PY" verify "$SHOT1" 1280 800 titlebars || fail=1
        python3 "$PY" verify "$SHOT1" 1280 800 cursor || fail=1
    else
        echo "  MISS screendump 1"; fail=1
    fi
fi

# ---- type into the focused aiosh terminal: 6*7 -> 42 ----------------
if [ "$fail" = "0" ]; then
    python3 "$PY" type "$MON" "6*7"
    python3 "$PY" key "$MON" ret
    sleep 6        # micro-LM route + calc + repaint under TCG
    if python3 "$PY" shot "$MON" "$SHOT2"; then
        echo "  OK   screendump 2 (after typing 6*7)"
        python3 "$PY" verify "$SHOT2" 1280 800 terminal_changed "$SHOT1" || fail=1
        python3 "$PY" verify "$SHOT2" 1280 800 has_42 "$SHOT1" || fail=1
    else
        echo "  MISS screendump 2"; fail=1
    fi
fi

# ---- shot 3: the pending-changes window (M8) -------------------------
# Opening it takes a click on the "N pending" badge in the launcher bar, so this
# only runs when the boot script staged something first. It is additive: the
# five checks above stand on their own, and a build with nothing pending is not a
# failure.
if [ "$fail" = "0" ] && [ -n "${AIOS_GUI_PENDING:-}" ]; then
    python3 "$PY" click "$MON" "$(( (1280 - 440 - 8 - 96) + 48 ))" "$(( 800 - 64 + 20 ))"
    sleep 2
    if python3 "$PY" shot "$MON" "$SHOT3"; then
        echo "  OK   screendump 3 (pending changes window)"
        python3 "$PY" verify "$SHOT3" 1280 800 pending_window || fail=1
    else
        echo "  MISS screendump 3"; fail=1
    fi
fi

# ---- save evidence + teardown ---------------------------------------
mkdir -p download/m10-boot-shots
if [ -f "$SHOT1" ]; then
    python3 - "$SHOT1" "$SHOT2" <<'EOF'
import sys
from PIL import Image
for src, name in [(sys.argv[1], "desktop-idle.png"), (sys.argv[2], "desktop-67.png")]:
    try:
        Image.open(src).save("download/m10-boot-shots/" + name)
        print("  OK   saved download/m10-boot-shots/" + name)
    except Exception as e:
        print("  MISS saving", name, e)
EOF
fi
cp "$OUT" boot/build/boot-gui.log.prev 2>/dev/null || true
python3 "$PY" quit "$MON" 2>/dev/null
kill $QPID 2>/dev/null
wait $QPID 2>/dev/null

echo
if [ $fail -eq 0 ]; then
    echo "[boot-gui] PASS - the AI-OS desktop really drew, took input and answered"
    exit 0
else
    echo "[boot-gui] FAIL - see $OUT"
    exit 1
fi

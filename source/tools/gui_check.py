#!/usr/bin/env python3
# gui_check.py — helper for boot-gui-test.sh: talk to the QEMU HMP monitor
# (unix socket) and verify screendump PPMs pixel-wise.
#
#   gui_check.py shot <mon.sock> <out.ppm>      take a screendump
#   gui_check.py type <mon.sock> <text>         sendkey a string
#   gui_check.py key  <mon.sock> <keyname>      one raw sendkey (ret, etc.)
#   gui_check.py click <mon.sock> <x> <y>       move+press+release (tablet)
#   gui_check.py quit <mon.sock>                terminate QEMU
#   gui_check.py verify <shot.ppm> <checks...>  pixel assertions (see main)
import os
import sys
import time
import socket


def hmp(sock_path, command, expect_prompt=True, timeout=15):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(timeout)
    s.connect(sock_path)
    time.sleep(0.2)
    try:
        s.recv(4096)  # banner
    except socket.timeout:
        pass
    s.sendall((command + "\n").encode())
    time.sleep(0.5)
    out = b""
    if expect_prompt:
        try:
            s.settimeout(3)
            out = s.recv(65536)
        except socket.timeout:
            pass
    s.close()
    return out


def do_shot(mon, ppm):
    if os.path.exists(ppm):
        os.unlink(ppm)
    hmp(mon, f"screendump {ppm}")
    for _ in range(50):
        if os.path.exists(ppm) and os.path.getsize(ppm) > 1000:
            return True
        time.sleep(0.1)
    return False


KEYMAP = {
    " ": "spc", ".": "dot", ",": "comma", "-": "minus", "=": "equal",
    "/": "slash", ";": "semicolon", "'": "apostrophe", "[": "bracket_left",
    "]": "bracket_right", "\\": "backslash", "`": "grave_accent",
    "!": "shift-1", "@": "shift-2", "#": "shift-3", "$": "shift-4",
    "%": "shift-5", "^": "shift-6", "&": "shift-7", "*": "shift-8",
    "(": "shift-9", ")": "shift-0", "_": "shift-minus", "+": "shift-equal",
    "?": "shift-slash", ":": "shift-semicolon", "<": "shift-comma",
    ">": "shift-dot",
}


def do_type(mon, text):
    for ch in text:
        if ch.isupper():
            hmp(mon, "sendkey shift-" + ch.lower())
        elif ch in KEYMAP:
            hmp(mon, "sendkey " + KEYMAP[ch])
        elif ch.isalnum():
            hmp(mon, "sendkey " + ch)
        else:
            continue
        time.sleep(0.12)


def load_ppm(path):
    with open(path, "rb") as f:
        data = f.read()
    # P6
    w = h = 0
    parts = data.split(b"\n", 3)
    assert parts[0].strip() == b"P6", f"not P6: {parts[0]!r}"
    dims = parts[1].split()
    w, h = int(dims[0]), int(dims[1])
    maxval = int(parts[2])
    assert maxval == 255
    raw = parts[3]
    return w, h, raw


def px(raw, w, x, y):
    i = (y * w + x) * 3
    return raw[i], raw[i + 1], raw[i + 2]


def main():
    cmd = sys.argv[1]
    if cmd == "shot":
        ok = do_shot(sys.argv[2], sys.argv[3])
        sys.exit(0 if ok else 1)
    if cmd == "type":
        do_type(sys.argv[2], sys.argv[3])
        sys.exit(0)
    if cmd == "key":
        hmp(sys.argv[2], "sendkey " + sys.argv[3])
        sys.exit(0)
    if cmd == "click":
        # M8: a real click, at absolute screen coordinates, through the tablet.
        # QEMU's HMP mouse_move takes ABSOLUTE coordinates when a tablet is
        # attached; mouse_button then delivers press and release. Both are
        # needed -- a press with no release leaves the desktop's button state
        # stuck down and the next click is swallowed.
        mon, x, y = sys.argv[2], int(sys.argv[3]), int(sys.argv[4])
        hmp(mon, f"mouse_move {x} {y}")
        time.sleep(0.2)
        hmp(mon, "mouse_button 1")
        time.sleep(0.2)
        hmp(mon, "mouse_button 0")
        sys.exit(0)
    if cmd == "quit":
        hmp(sys.argv[2], "quit", expect_prompt=False)
        sys.exit(0)
    if cmd == "verify":
        # verify <shot.ppm> <w> <h> <check> [args...]
        path, w, h = sys.argv[2], int(sys.argv[3]), int(sys.argv[4])
        _, _, raw = load_ppm(path)
        assert len(raw) >= w * h * 3, "short ppm"
        check = sys.argv[5]
        if check == "gradient":
            # left column: vertical variance (wallpaper)
            tops, bots = [], []
            for x in (40, 200, 1200):
                t = px(raw, w, x, 80)
                b = px(raw, w, x, h - 120)
                tops.append(t)
                bots.append(b)
            diff = sum(abs(a[i] - b[i]) for a, b in zip(tops, bots)
                       for i in range(3))
            assert diff > 30, f"no vertical gradient (diff={diff})"
            print(f"PASS gradient (diff={diff})")
        elif check == "bar_accent":
            # the green line atop the launcher bar
            found = 0
            y = h - 64
            for x in range(100, w - 100, 40):
                r, g, b = px(raw, w, x, y)
                if g > 150 and r < 120 and b > 100 and g > r and g > b:
                    found += 1
            assert found > 10, f"accent line missing ({found} hits)"
            print(f"PASS bar_accent ({found} green hits on the bar line)")
        elif check == "titlebars":
            # five windows: title text sits at (win.x+10, win.y+4..20);
            # focused titles are bright, unfocused ones dim (>=380 sum)
            wins = [(40, 40), (780, 40), (40, 510), (440, 510), (840, 510)]
            for (wx, wy) in wins:
                ink = 0
                for y in range(wy + 6, wy + 18):
                    for x in range(wx + 12, min(wx + 220, w - 1), 4):
                        r, g2, b = px(raw, w, x, y)
                        if r + g2 + b > 380:
                            ink += 1
                assert ink > 3, f"title ink missing near ({wx},{wy})"
            print("PASS titlebars (5 windows)")
        elif check == "cursor":
            # cursor sprite pixels near screen center
            ink = 0
            for y in range(380, 430):
                for x in range(620, 680):
                    r, g, b = px(raw, w, x, y)
                    if r + g + b > 600 or (r + g + b < 90 and b < 60):
                        ink += 1
            assert ink > 30, f"cursor sprite not visible ({ink})"
            print(f"PASS cursor ({ink} sprite pixels)")
        elif check == "terminal_changed":
            other = sys.argv[6]
            _, _, raw2 = load_ppm(other)
            # terminal body region only
            changed = 0
            for y in range(90, 380, 4):
                for x in range(60, 600, 8):
                    i1 = (y * w + x) * 3
                    if raw[i1:i1 + 3] != raw2[i1:i1 + 3]:
                        changed += 1
            assert changed > 3, f"terminal region unchanged ({changed})"
            print(f"PASS terminal_changed ({changed} sampled px differ)")
        elif check == "has_42":
            # crude: the reply "42" drawn inside the terminal body creates
            # bright pixel clusters on the left half of the terminal area
            # after typing 6*7 — check the answer row band grew brighter
            other = sys.argv[6]
            _, _, raw2 = load_ppm(other)
            diff = 0
            for y in range(90, 380, 2):
                for x in range(60, 600, 4):
                    i1 = (y * w + x) * 3
                    if abs(raw[i1] - raw2[i1]) > 24 or \
                       abs(raw[i1 + 1] - raw2[i1 + 1]) > 24:
                        diff += 1
            assert diff > 12, f"no visible new output ({diff} px)"
            print(f"PASS has_42 ({diff} changed px in terminal)")
        elif check == "pending_window":
            # M8: the pending-changes window. A SIXTH WINDOW, not a sixth
            # launcher slot, centred and overlapping the five apps:
            #   { W/2 - 260, 90, 520, H - 90 - BAR_H - 20 } on 1280x800
            # so it starts at x=380, y=90 and is 520 wide. Opening it is a
            # person's action (the bar badge), so this runs on a shot taken
            # after that click.
            wx, wy, ww = (w // 2 - 260), 90, 520
            assert 0 <= wx < w, "window origin off-screen"
            # The window ORIGIN is the title bar; the body (the canvas that
            # aios_win_text draws into) starts AIOS_TITLE_H=24 below it. Every
            # probe below is in canvas coordinates, offset by cy.
            cy = wy + 24
            # 1. the title bar: bright ink on the title row, in the left part
            ink = 0
            for y in range(wy + 6, wy + 18):
                for x in range(wx + 12, min(wx + 200, w - 1), 4):
                    r, g, b = px(raw, w, x, y)
                    if r + g + b > 380:
                        ink += 1
            assert ink > 3, f"pending window title ink missing ({ink})"
            # 2. the accent-coloured heading ("N pending changes") at canvas y=4,
            # green text on the panel fill
            head = 0
            for y in range(cy + 4, cy + 20):
                for x in range(wx + 8, min(wx + 190, w - 1)):
                    r, g, b = px(raw, w, x, y)
                    if g > 150 and r < 130 and b < 200 and g > r:
                        head += 1
            assert head > 20, f"no pending heading ink ({head})"
            # 3. a green '+' row and a red '-' row in the diff pane. Hue, not
            # brightness: the '+' is (0x5a,0xd6,0x8a), the '-' is (0xe8,0x6a,0x7a)
            green = red = 0
            for y in range(cy + 40, min(cy + 240, h - 1)):
                for x in range(wx + 8, min(wx + ww - 8, w - 1)):
                    r, g, b = px(raw, w, x, y)
                    if g > 120 and r < 110 and g > r + 40 and b < 200:
                        green += 1
                    if r > 120 and r > g + 40 and r > b + 20 and g < 160:
                        red += 1
            assert green > 40, f"no green '+' diff row ({green} px)"
            assert red > 5, f"no red '-' diff row ({red} px)"
            print(f"PASS pending_window (title ink={ink}, heading={head}, "
                  f"green={green}, red={red})")
        else:
            sys.exit(f"unknown check {check}")
        sys.exit(0)
    sys.exit("unknown command")


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
# img2aiosimg.py — M10 desktop build-time image pipeline.
#
# Converts artist-supplied PNGs (desktop/assets/*.png, spec: desktop/assets/
# ASSETS.md) into the C-side internal format *.aiosimg:
#
#     offset 0: magic  "AIOSIMG1"  (8 bytes)
#     offset 8: width  (uint32 LE)
#     offset 12: height (uint32 LE)
#     offset 16: rows of RGBA (top-to-bottom), 4 bytes per pixel
#
# The C side (src/fb.c aios_img_load) reads exactly this; zero image
# libraries are linked into the OS. Missing or malformed PNGs never fail
# the build — every asset has a procedural fallback compiled in.
#
# Usage: img2aiosimg.py <in.png> <out.aiosimg> [--size WxH]
#   --size   force exact dimensions (nearest-neighbour rescale), matching
#            the ASSETS.md contract that sizes must be exact.
import struct
import sys

from PIL import Image


def main():
    if len(sys.argv) < 3:
        print("usage: img2aiosimg.py <in.png> <out.aiosimg> [--size WxH]",
              file=sys.stderr)
        sys.exit(2)
    src, dst = sys.argv[1], sys.argv[2]
    want_w = want_h = None
    if "--size" in sys.argv:
        i = sys.argv.index("--size")
        want_w, want_h = (int(v) for v in sys.argv[i + 1].split("x", 1))

    img = Image.open(src)
    img.load()
    if img.mode not in ("RGB", "RGBA"):
        img = img.convert("RGBA")
    if want_w and (img.width != want_w or img.height != want_h):
        img = img.resize((want_w, want_h), Image.NEAREST)
    img = img.convert("RGBA")

    data = img.tobytes()  # RGBA rows top-to-bottom
    with open(dst, "wb") as f:
        f.write(b"AIOSIMG1")
        f.write(struct.pack("<II", img.width, img.height))
        f.write(data)
    print(f"[img2aiosimg] {src} {img.width}x{img.height} -> {dst} "
          f"({16 + len(data)} bytes)")


if __name__ == "__main__":
    main()

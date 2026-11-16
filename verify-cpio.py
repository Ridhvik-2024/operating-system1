#!/usr/bin/env python3
"""verify-cpio.py — structural verification of a cpio newc initramfs.

Parses every header, checks magic/field widths, name/data 4-byte alignment,
that file sizes match record sizes, prints the full entry list and validates
that required AI-OS entries exist and executables are non-empty.
Accepts gzip- (m7 and earlier) or xz-compressed (m8+) images.
"""
import gzip
import lzma
import struct
import sys

def main(path):
    data = open(path, "rb").read()
    if data[:6] == b"\xfd7zXZ\x00":      # m8+: initramfs ships xz-compressed
        data = lzma.decompress(data)
    elif data[:2] == b"\x1f\x8b":        # m7 and earlier: gzip
        data = gzip.decompress(data)
    pos = 0
    entries = []
    while True:
        if data[pos:pos + 6] != b"070701":
            print(f"FATAL: bad magic at offset {pos}: {data[pos:pos+6]!r}")
            return 1
        hdr = data[pos:pos + 110]
        f = [int(hdr[6 + i * 8:14 + i * 8], 16) for i in range(13)]
        (ino, mode, uid, gid, nlink, mtime, filesize, devmaj, devmin,
         rmaj, rmin, namesize, check) = f
        name_start = pos + 110
        name = data[name_start:name_start + namesize - 1].decode()
        data_start = name_start + namesize
        data_start += (4 - data_start % 4) % 4
        if name == "TRAILER!!!":
            entries.append(("trailer", 0))
            break
        blob = data[data_start:data_start + filesize]
        entries.append((name.lstrip("/"), mode, filesize, blob,
                        rmaj, rmin))
        pos = data_start + filesize
        pos += (4 - pos % 4) % 4

    ok = True
    print(f"entries: {len(entries)}")
    for e in entries:
        if e[0] == "trailer":
            continue
        name, mode, size = e[0], e[1], e[2]
        kind = {0o040755: "dir", 0o120777: "sym"}.get(mode & 0o777777)
        if kind is None:
            kind = "chr" if (mode & 0o170000) == 0o020000 else "file"
        print(f"  {kind:4} {oct(mode & 0o7777):>7} {size:>9}  {name}")

    names = {e[0] for e in entries if e[0] != "trailer"}
    required = [
        "init", "bin/busybox", "bin/sh", "sbin/aiosh", "sbin/tool-exec",
        "sbin/aios-memctl", "sbin/aios-governor", "usr/bin/aios-selftest",
        "etc/aios/boot.aiosh", "etc/os-release", "dev/console", "dev/null",
        "lib/modules/ext2.ko", "lib/modules/mbcache.ko",
        "lib/modules/virtio_blk.ko",
    ]
    for r in required:
        if r not in names:
            print(f"MISSING required entry: {r}")
            ok = False
    tests = [n for n in names if n.startswith("usr/lib/aios/tests/test_")]
    print(f"test binaries in image: {len(tests)}")
    if len(tests) != 10:
        print("MISSING test binaries (expected 10)")
        ok = False
    blobs = {e[0]: e[3] for e in entries if len(e) == 6}
    for r in ["init", "sbin/aiosh", "usr/bin/aios-selftest"]:
        if r in blobs and len(blobs[r]) == 0:
            print(f"EMPTY file: {r}")
            ok = False
    # selftest script must be a shell script; binaries must be ELF
    if blobs.get("init", b"")[:4] != b"\x7fELF":
        print("FATAL: /init is not ELF")
        ok = False
    if not blobs.get("usr/bin/aios-selftest", b"").startswith(b"#!/bin/sh"):
        print("FATAL: aios-selftest is not a sh script")
        ok = False
    # /dev/console must be char 5:1
    cons = [e for e in entries if len(e) == 6 and e[0] == "dev/console"]
    if not cons or cons[0][1] & 0o170000 != 0o020000 or (cons[0][4], cons[0][5]) != (5, 1):
        print("FATAL: /dev/console char device 5:1 missing")
        ok = False
    else:
        print("/dev/console: char 5:1 ok")
    print("VERIFY-CPIO:", "PASS" if ok else "FAIL")
    return 0 if ok else 1

if __name__ == "__main__":
    sys.exit(main(sys.argv[1]))

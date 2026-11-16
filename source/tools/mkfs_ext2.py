#!/usr/bin/env python3
"""mkfs_ext2.py — deterministic minimal ext2 image builder for AI-OS M8.

AI-OS needs a small ext2 side disk for durable state (M8). Host machines
and CI sandboxes have no mkfs.ext2, and QEMU guests obviously cannot
format their own disk before the filesystem driver is even loaded. This
tool builds the empty image on the host with zero dependencies.

Layout (single block group, the simplest valid ext2):
  8 MiB total, 1 KiB blocks  ->  8192 blocks (one group, max 8192/group)
  block 0        boot block (zeros)
  block 1        superblock (rev 1, filetype feature, 128-byte inodes)
  block 2        group descriptor table (1 x 32-byte descriptor)
  block 3        block bitmap
  block 4        inode bitmap
  blocks 5..260  inode table (2048 inodes x 128 B = 256 blocks)
  block 261      root directory data
  block 262      /aios directory data (the OS durable store root)

The result mounts read-write with the Linux ext2 driver (validated in the
M8 boot test). Everything is deterministic: fixed UUID, zero timestamps.

usage: python3 mkfs_ext2.py <image-file> [size-mib]
"""
import struct
import sys

SECTOR = 512
BLOCK = 1024
TOTAL_BLOCKS = 8192          # 8 MiB / 1 KiB
TOTAL_INODES = 2048
ITABLE_START = 5
ITABLE_BLOCKS = TOTAL_INODES * 128 // BLOCK   # 256
ROOT_DATA = ITABLE_START + ITABLE_BLOCKS      # 261
AIOS_DATA = ROOT_DATA + 1                     # 262
USED_BLOCKS = AIOS_DATA + 1                   # boot..262 inclusive = 263
USED_INODES = 11                              # 1..10 reserved + root(2) + aios(11)
AIOS_INO = 11

UUID = b"aios-m8-durable1"                    # exactly 16 bytes, deterministic
VOLUME = b"aios-durable"


def build_superblock() -> bytes:
    sb = bytearray(1024)
    struct.pack_into("<I", sb, 0, TOTAL_INODES)     # s_inodes_count
    struct.pack_into("<I", sb, 4, TOTAL_BLOCKS)     # s_blocks_count
    struct.pack_into("<I", sb, 8, 0)                # s_r_blocks_count
    struct.pack_into("<I", sb, 12, TOTAL_BLOCKS - USED_BLOCKS)  # s_free_blocks_count
    struct.pack_into("<I", sb, 16, TOTAL_INODES - USED_INODES)  # s_free_inodes_count
    struct.pack_into("<I", sb, 20, 1)               # s_first_data_block (1K blocks)
    struct.pack_into("<I", sb, 24, 0)               # s_log_block_size (1<<0 = 1K)
    struct.pack_into("<I", sb, 28, 0)               # s_log_frag_size
    struct.pack_into("<I", sb, 32, TOTAL_BLOCKS)    # s_blocks_per_group
    struct.pack_into("<I", sb, 36, TOTAL_BLOCKS)    # s_frags_per_group
    struct.pack_into("<I", sb, 40, TOTAL_INODES)    # s_inodes_per_group
    struct.pack_into("<I", sb, 44, 0)               # s_mtime
    struct.pack_into("<I", sb, 48, 0)               # s_wtime
    struct.pack_into("<H", sb, 52, 0)               # s_mnt_count
    struct.pack_into("<h", sb, 54, -1)              # s_max_mnt_count (never warn)
    struct.pack_into("<H", sb, 56, 0xEF53)          # s_magic
    struct.pack_into("<H", sb, 58, 1)               # s_state (clean)
    struct.pack_into("<H", sb, 60, 1)               # s_errors (continue)
    struct.pack_into("<H", sb, 62, 0)               # s_minor_rev_level
    struct.pack_into("<I", sb, 64, 0)               # s_lastcheck
    struct.pack_into("<I", sb, 68, 0)               # s_checkinterval
    struct.pack_into("<I", sb, 72, 0)               # s_creator_os (Linux)
    struct.pack_into("<I", sb, 76, 1)               # s_rev_level (dynamic)
    struct.pack_into("<H", sb, 80, 0)               # s_def_resuid
    struct.pack_into("<H", sb, 82, 0)               # s_def_resgid
    struct.pack_into("<I", sb, 84, AIOS_INO)        # s_first_ino
    struct.pack_into("<H", sb, 88, 128)             # s_inode_size
    struct.pack_into("<H", sb, 90, 0)               # s_block_group_nr
    struct.pack_into("<I", sb, 92, 0)               # s_feature_compat
    struct.pack_into("<I", sb, 96, 0x2)             # s_feature_incompat: FILETYPE
    struct.pack_into("<I", sb, 100, 0)              # s_feature_ro_compat
    sb[104:120] = UUID                              # s_uuid (16 bytes)
    sb[120:120 + len(VOLUME)] = VOLUME              # s_volume_name
    assert len(sb) == 1024, f"superblock must be 1024 B, got {len(sb)}"
    return bytes(sb)


def build_group_desc() -> bytes:
    """On-disk ext2 group descriptor (as written by mke2fs): the block
    pointers come first, the free counts follow at offset 12."""
    gd = bytearray(32)
    struct.pack_into("<I", gd, 0, 3)      # bg_block_bitmap
    struct.pack_into("<I", gd, 4, 4)      # bg_inode_bitmap
    struct.pack_into("<I", gd, 8, 5)      # bg_inode_table
    struct.pack_into("<H", gd, 12, TOTAL_BLOCKS - USED_BLOCKS)  # free blocks
    struct.pack_into("<H", gd, 14, TOTAL_INODES - USED_INODES)  # free inodes
    struct.pack_into("<H", gd, 16, 2)     # used dirs: / + /aios
    return bytes(gd)


def build_block_bitmap() -> bytes:
    bm = bytearray(BLOCK)
    for b in range(USED_BLOCKS):            # bits 0..262 set
        bm[b // 8] |= 1 << (b % 8)
    return bytes(bm)


def build_inode_bitmap() -> bytes:
    bm = bytearray(BLOCK)
    for i in range(USED_INODES):            # inodes 1..11 (bit 0..10) used
        bm[i // 8] |= 1 << (i % 8)
    # inodes beyond inodes_count don't exist: mke2fs marks their bits used
    # so e2fsck never sees phantom free inodes
    for i in range(TOTAL_INODES, BLOCK * 8):
        bm[i // 8] |= 1 << (i % 8)
    return bytes(bm)


def make_inode(mode: int, size: int, links: int, data_block: int) -> bytes:
    ino = bytearray(128)
    struct.pack_into("<H", ino, 0, mode)          # i_mode
    struct.pack_into("<H", ino, 2, 0)             # i_uid
    struct.pack_into("<I", ino, 4, size)          # i_size
    struct.pack_into("<I", ino, 8, 0)             # i_atime
    struct.pack_into("<I", ino, 12, 0)            # i_ctime
    struct.pack_into("<I", ino, 16, 0)            # i_mtime
    struct.pack_into("<I", ino, 20, 0)            # i_dtime
    struct.pack_into("<H", ino, 24, 0)            # i_gid
    struct.pack_into("<H", ino, 26, links)        # i_links_count
    struct.pack_into("<I", ino, 28, BLOCK * 2 // SECTOR)  # i_blocks: 1K in 512B sectors
    struct.pack_into("<I", ino, 32, 0)            # i_flags
    struct.pack_into("<I", ino, 40, data_block)   # i_block[0]
    return bytes(ino)


def dir_entry(ino: int, name: str, ftype: int, rec_len: int) -> bytes:
    nb = name.encode()
    e = struct.pack("<IHBB", ino, rec_len, len(nb), ftype) + nb
    return e + b"\x00" * (rec_len - len(e))


def build_root_block() -> bytes:
    b = bytearray(BLOCK)
    b[0:12] = dir_entry(2, ".", 2, 12)
    b[12:24] = dir_entry(2, "..", 2, 12)
    b[24:] = dir_entry(AIOS_INO, "aios", 2, BLOCK - 24)
    return bytes(b)


def build_aios_block() -> bytes:
    b = bytearray(BLOCK)
    b[0:12] = dir_entry(AIOS_INO, ".", 2, 12)
    b[12:] = dir_entry(2, "..", 2, BLOCK - 12)
    return bytes(b)


def build_image(size_bytes: int) -> bytes:
    # NOTE: all writes use img[off:off+len(x)] = x — a plain img[a:b] = x
    # would RESIZE the bytearray when len(x) != b-a (python slice semantics).
    img = bytearray(size_bytes)
    img[1024:1024 + 1024] = build_superblock()
    img[2 * BLOCK:2 * BLOCK + 32] = build_group_desc()
    img[3 * BLOCK:4 * BLOCK] = build_block_bitmap()
    img[4 * BLOCK:5 * BLOCK] = build_inode_bitmap()
    itable = bytearray(ITABLE_BLOCKS * BLOCK)
    # inode 2 = root, inode AIOS_INO = /aios (1-based table slots)
    itable[(2 - 1) * 128:(2) * 128] = make_inode(0x41ED, BLOCK, 3, ROOT_DATA)
    slot = (AIOS_INO - 1) * 128
    itable[slot:slot + 128] = make_inode(0x41ED, BLOCK, 2, AIOS_DATA)
    off = ITABLE_START * BLOCK
    img[off:off + len(itable)] = itable
    img[ROOT_DATA * BLOCK:(ROOT_DATA + 1) * BLOCK] = build_root_block()
    img[AIOS_DATA * BLOCK:(AIOS_DATA + 1) * BLOCK] = build_aios_block()
    return bytes(img)


def verify(path: str) -> None:
    """Read the image back and sanity-check the on-disk structures."""
    with open(path, "rb") as f:
        img = f.read()
    magic = struct.unpack_from("<H", img, 1024 + 56)[0]
    assert magic == 0xEF53, f"bad magic {magic:#x}"
    inodes, blocks = struct.unpack_from("<II", img, 1024)
    assert (inodes, blocks) == (TOTAL_INODES, TOTAL_BLOCKS)
    gdt = struct.unpack_from("<3I4H", img, 2 * BLOCK)
    assert gdt[:3] == (3, 4, 5), f"GDT pointers wrong: {gdt[:3]}"
    blk = img[ROOT_DATA * BLOCK:(ROOT_DATA + 1) * BLOCK]
    names = []
    pos = 0
    while pos < len(blk):
        ino, rec, nlen, ftype = struct.unpack_from("<IHBB", blk, pos)
        if ino:
            names.append(blk[pos + 8:pos + 8 + nlen].decode())
        if rec == 0:
            break
        pos += rec
    assert "aios" in names, f"/aios missing from root: {names}"
    bmp = img[3 * BLOCK:4 * BLOCK]
    pop = sum(bin(b).count("1") for b in bmp)
    print(f"verify: ok — magic EF53, {names}, gdt={gdt[:3]}, "
          f"block bitmap popcount {pop}")


def main() -> None:
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(2)
    path = sys.argv[1]
    mib = int(sys.argv[2]) if len(sys.argv) > 2 else 8
    if mib != 8:
        print("error: only the 8 MiB single-group layout is supported", file=sys.stderr)
        sys.exit(2)
    img = build_image(mib * 1024 * 1024)
    with open(path, "wb") as f:
        f.write(img)
    print(f"mkfs_ext2: wrote {path} ({len(img)} bytes, ext2 rev1, 1K blocks, "
          f"{TOTAL_INODES} inodes, /aios precreated)")
    verify(path)


if __name__ == "__main__":
    main()

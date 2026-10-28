#!/usr/bin/env python3
"""Extract the game partition of an Xbox / Xbox 360 XDVDFS disc image.

usage: xdvdfs_extract.py <image.iso> <out_dir> [--list] [--only PATH ...]
"""
import os
import struct
import sys

SECTOR = 2048
MAGIC = b"MICROSOFT*XBOX*MEDIA"
# Game partition offsets: plain XISO, XGD1, XGD2, XGD3.
PARTITIONS = (0, 0x18300000, 0xFD90000, 0x2080000)


def find_partition(f):
    for base in PARTITIONS:
        f.seek(base + 32 * SECTOR)
        if f.read(len(MAGIC)) == MAGIC:
            return base
    raise SystemExit("no XDVDFS volume descriptor found")


def walk(f, base, sector, size, prefix=""):
    """Yield (path, is_dir, sector, size) for a directory table (AVL tree)."""
    f.seek(base + sector * SECTOR)
    table = f.read(size)
    stack, seen = [0], set()
    while stack:
        off = stack.pop()
        if off in seen or off + 14 > len(table):
            continue
        seen.add(off)
        left, right, start, length, attr, nlen = struct.unpack_from("<HHIIBB", table, off)
        if left == 0xFFFF:  # padding
            continue
        name = table[off + 14:off + 14 + nlen].decode("latin-1")
        if name in ("", ".", "..") or "/" in name or "\\" in name:
            raise SystemExit(f"refusing unsafe entry name {name!r}")
        path = f"{prefix}/{name}" if prefix else name
        is_dir = bool(attr & 0x10)
        yield path, is_dir, start, length
        if is_dir and length:
            yield from walk(f, base, start, length, path)
        if left:
            stack.append(left * 4)
        if right:
            stack.append(right * 4)


def main():
    args = sys.argv[1:]
    if len(args) < 2:
        raise SystemExit(__doc__)
    image, out = args[0], args[1]
    listing = "--list" in args
    only = args[args.index("--only") + 1:] if "--only" in args else []
    with open(image, "rb") as f:
        base = find_partition(f)
        f.seek(base + 32 * SECTOR + 20)
        root_sector, root_size = struct.unpack("<II", f.read(8))
        for path, is_dir, start, length in walk(f, base, root_sector, root_size):
            if listing:
                print(f"{'D' if is_dir else 'F'} {length:12d} {path}")
                continue
            if only and not any(path.lower() == o.lower() or path.lower().startswith(o.lower() + "/") for o in only):
                continue
            dest = os.path.join(out, *path.split("/"))
            if is_dir:
                os.makedirs(dest, exist_ok=True)
                continue
            os.makedirs(os.path.dirname(dest), exist_ok=True)
            f.seek(base + start * SECTOR)
            with open(dest, "wb") as o:
                left = length
                while left:
                    chunk = f.read(min(left, 1 << 22))
                    if not chunk:
                        raise SystemExit(f"short read in {path}")
                    o.write(chunk)
                    left -= len(chunk)


if __name__ == "__main__":
    main()

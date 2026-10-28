#!/usr/bin/env python3
"""Write each Xbox 360 shader container in an image to its own file.

usage: extract_shaders.py <image.bin> <out_dir>

Input is an image dumped with NG2_DUMP_IMAGE. Uses the same container test as
XenosRecomp's directory scan. Files are named <guest address>_<vs|ps>.bin.
"""
import os
import struct
import sys

IMAGE_BASE = 0x82000000


def containers(img):
    i = 0
    while i + 0x24 <= len(img):
        flags, virtual_size, physical_size = struct.unpack_from(">III", img, i)
        size = virtual_size + physical_size
        field1c, field20 = struct.unpack_from(">II", img, i + 0x1C)
        if ((flags & 0xFFFFFF00) == 0x102A1100 and 0 < size <= len(img) - i
                and field1c == 0 and field20 == 0):
            yield i, flags & 0xFF, img[i:i + size]
            i += size
        else:
            i += 4


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    img = open(sys.argv[1], "rb").read()
    out = sys.argv[2]
    os.makedirs(out, exist_ok=True)
    count = {0: 0, 1: 0}
    for off, kind, data in containers(img):
        name = f"{IMAGE_BASE + off:08X}_{'vs' if kind == 1 else 'ps'}.bin"
        with open(os.path.join(out, name), "wb") as f:
            f.write(data)
        count[kind] = count.get(kind, 0) + 1
    print(f"{count[1]} vertex, {count[0]} pixel shaders -> {out}")


if __name__ == "__main__":
    main()

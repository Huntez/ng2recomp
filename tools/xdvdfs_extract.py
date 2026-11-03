#!/usr/bin/env python3
"""Extract the game from an Xbox / Xbox 360 disc image (XDVDFS) or a Games on
Demand package (the header file next to its <name>.data folder).

usage: xdvdfs_extract.py <image.iso | GOD header> <out_dir> [--list] [--progress] [--only PATH ...]
  --progress  print the percentage copied, one line each time it changes
"""
import os
import struct
import sys

SECTOR = 2048
MAGIC = b"MICROSOFT*XBOX*MEDIA"
TITLE_ID = 0x544307D5
MAX_ENTRIES = 100000  # a damaged directory can't make the walk endless
# Game partition offsets: plain XISO, XGD1, XGD2, XGD3.
PARTITIONS = (0, 0x18300000, 0xFD90000, 0x2080000)


class Iso:
    """A disc image: sectors at the game partition's offset."""

    def __init__(self, path):
        self.f = open(path, "rb")
        for base in PARTITIONS:
            self.f.seek(base + 32 * SECTOR)
            if self.f.read(len(MAGIC)) == MAGIC:
                self.base = base
                return
        raise SystemExit("that file isn't a disc image or Games on Demand package of the game")

    def read(self, sector, size):
        self.f.seek(self.base + sector * SECTOR)
        data = self.f.read(size)
        if len(data) != size:
            raise SystemExit("the disc image is cut short")
        return data


class God:
    """A Games on Demand package (SVOD): the disc's sectors spread over the
    Data#### files of <header>.data, with a hash table before every 0x198
    sectors and one before every 0xA1C4 of those."""

    PER_L0, PER_L1, PER_FILE, FILE_SIZE = 0x198, 0xA1C4, 0x14388, 0xA290000

    def __init__(self, header):
        with open(header, "rb") as f:
            h = f.read(0x3A0)
        if len(h) < 0x3A0 or h[:4] not in (b"LIVE", b"PIRS", b"CON "):
            raise SystemExit("not a Games on Demand package")
        if struct.unpack_from(">I", h, 0x344)[0] != 0x7000 or \
                struct.unpack_from(">I", h, 0x360)[0] != TITLE_ID:
            raise SystemExit("that package isn't Ninja Gaiden II")
        vd = h[0x379:0x379 + 0x24]
        self.enhanced = bool(vd[0x18] & 0x40)
        self.start = int.from_bytes(vd[0x1C:0x1F], "little")
        folder = header + ".data"
        # Data0000, Data0001, ...: by number, so a missing one isn't skipped over.
        numbers = {int(n[4:]): n for n in os.listdir(folder)
                   if n.startswith("Data") and n[4:].isdigit()}
        if not numbers:
            raise SystemExit(f"no Data files in {folder}")
        for i in range(max(numbers) + 1):
            if i not in numbers:
                raise SystemExit(f"Data{i:04d} is missing from {folder}")
        self.files = [open(os.path.join(folder, numbers[i]), "rb") for i in range(len(numbers))]
        # Where the volume descriptor is tells the layout. A single file
        # carries the package header (0xB000) first.
        self.extra = 0
        if not self.enhanced and not self._magic_at(0x12000) and not self._magic_at(0x2000):
            self.extra = 0xB000
        if self.read(32, len(MAGIC)) != MAGIC:
            raise SystemExit("unsupported Games on Demand layout")

    def _magic_at(self, offset):
        self.files[0].seek(offset)
        return self.files[0].read(len(MAGIC)) == MAGIC

    def locate(self, sector):
        block = sector - self.start * 2 + (2 if self.enhanced else 0)
        if block < 0:
            raise SystemExit("the package's directory is damaged")
        index, block = divmod(block, self.PER_FILE)
        l0 = block // self.PER_L0 + 1
        address = block * SECTOR + (l0 + l0 // self.PER_L1 + 1) * 0x1000 + self.extra
        if address >= self.FILE_SIZE:
            index, address = index + 1, address % self.FILE_SIZE + 0x2000
        return index, address

    def read(self, sector, size):
        out = bytearray()
        while len(out) < size:
            index, address = self.locate(sector)
            # Sectors are contiguous up to the next hash table.
            run = (self.PER_L0 - (sector - self.start * 2 + (2 if self.enhanced else 0)) % self.PER_L0)
            want = min(size - len(out), run * SECTOR)
            if index >= len(self.files):
                raise SystemExit("the package's Data files are incomplete")
            f = self.files[index]
            f.seek(address)
            data = f.read(want)
            if len(data) != want:
                raise SystemExit("the package's Data files are incomplete")
            out += data
            sector += (want + SECTOR - 1) // SECTOR
        return bytes(out)


def open_image(path):
    with open(path, "rb") as f:
        head = f.read(4)
    if head in (b"LIVE", b"PIRS", b"CON "):
        if not os.path.isdir(path + ".data"):
            raise SystemExit("that package isn't the game (a Games on Demand package has a .data folder next to it)")
        return God(path)
    return Iso(path)


def walk(img, sector, size, prefix="", budget=None):
    """Yield (path, is_dir, sector, size) for a directory table (AVL tree)."""
    budget = budget if budget is not None else [MAX_ENTRIES, 256 << 20]  # entries, table bytes
    budget[1] -= size
    if prefix.count("/") >= 32 or size > 64 << 20 or budget[1] < 0:
        raise SystemExit("the disc's directory is damaged")
    table = img.read(sector, size)
    folders = []  # walked after this table is done with
    stack, seen = [0], set()
    while stack:
        off = stack.pop()
        if off in seen or off + 14 > len(table):
            continue
        seen.add(off)
        left, right, start, length, attr, nlen = struct.unpack_from("<HHIIBB", table, off)
        if left == 0xFFFF:  # padding
            continue
        if off + 14 + nlen > len(table):
            raise SystemExit("the disc's directory is damaged")
        budget[0] -= 1
        if budget[0] < 0:
            raise SystemExit("the disc's directory is damaged")
        name = table[off + 14:off + 14 + nlen].decode("latin-1")
        if name in ("", ".", "..") or any(c in name for c in "/\\:") or not name.isascii():
            raise SystemExit(f"refusing unsafe entry name {name!r}")
        path = f"{prefix}/{name}" if prefix else name
        is_dir = bool(attr & 0x10)
        yield path, is_dir, start, length
        if is_dir and length:
            folders.append((start, length, path))
        if left:
            stack.append(left * 4)
        if right:
            stack.append(right * 4)
    del table
    for start, length, path in folders:
        yield from walk(img, start, length, path, budget)


def main():
    args = sys.argv[1:]
    if len(args) < 2:
        raise SystemExit(__doc__)
    image, out = args[0], args[1]
    listing = "--list" in args
    progress = "--progress" in args
    only = args[args.index("--only") + 1:] if "--only" in args else []
    img = open_image(image)
    root_sector, root_size = struct.unpack("<II", img.read(32, 28)[20:28])
    # The console's system update ($SystemUpdate) isn't the game's.
    entries = [e for e in walk(img, root_sector, root_size)
               if listing or e[0].split("/")[0].lower() != "$systemupdate"]
    total = sum(length for _, is_dir, _, length in entries if not is_dir) or 1
    copied, shown = 0, -1
    for path, is_dir, start, length in entries:
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
        with open(dest, "wb") as o:
            done, chunk = 0, 1 << 22
            while done < length:
                n = min(chunk, length - done)
                o.write(img.read(start + done // SECTOR, n))
                done += n
                copied += n
                if progress and copied * 100 // total != shown:
                    shown = copied * 100 // total
                    print(shown, flush=True)


if __name__ == "__main__":
    main()

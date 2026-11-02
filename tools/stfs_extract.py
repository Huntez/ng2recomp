#!/usr/bin/env python3
"""Extract one file from an Xbox 360 content package (STFS: LIVE, PIRS, CON).

usage: stfs_extract.py <package> <file name> <output file>

Title updates hold default.xexp; this pulls it out for the game folder.
Supports files stored in consecutive blocks and fragmented ones (followed
through the level-0 hash tables).
"""
import struct
import sys

BLOCK = 0x1000
PER_TABLE = 0xAA  # data blocks per level-0 hash table
PER_L1 = 0x70E4   # data blocks per level-1 hash table


def main():
    package, wanted, out = sys.argv[1:4]
    d = open(package, 'rb').read()
    if d[:4] not in (b'LIVE', b'PIRS', b'CON '):
        sys.exit(f'{package}: not a content package')
    header_size = struct.unpack_from('>I', d, 0x340)[0]
    vd = d[0x379:0x379 + 0x24]
    # One hash table per level in read-only packages, two otherwise.
    shift = 0 if vd[2] & 1 else 1
    table_blocks = struct.unpack_from('<H', vd, 3)[0]
    table_start = vd[5] | vd[6] << 8 | vd[7] << 16
    first = (header_size + 0xFFF) & ~0xFFF

    def backing(b):
        # Data block number -> block number in the file (hash tables between).
        r = (((b + PER_TABLE) // PER_TABLE) << shift) + b
        if b < PER_TABLE:
            return r
        if b < PER_L1:
            return r + (((b + PER_L1) // PER_L1) << shift)
        return (1 << shift) + r + (((b + PER_L1) // PER_L1) << shift)

    def offset(b):
        return first + backing(b) * BLOCK

    def hash_table(b):
        # Level-0 hash table covering data block b.
        n = (b // PER_TABLE) * (PER_TABLE + (1 << shift))
        if b >= PER_TABLE:
            n += ((b // PER_L1) + 1) << shift
        if b >= PER_L1:
            n += 1 << shift
        return first + n * BLOCK

    def next_block(b):
        e = hash_table(b) + (b % PER_TABLE) * 0x18
        return d[e + 0x15] << 16 | d[e + 0x16] << 8 | d[e + 0x17]

    b = table_start
    for _ in range(table_blocks):
        table = d[offset(b):offset(b) + BLOCK]
        for i in range(BLOCK // 0x40):
            e = table[i * 0x40:(i + 1) * 0x40]
            flags = e[0x28]
            name = e[:flags & 0x3F].decode('ascii', 'replace')
            # At the package's root (path indicator 0xFFFF), by name.
            at_root = struct.unpack_from('>H', e, 0x32)[0] == 0xFFFF
            if not name or flags & 0x80 or not at_root or name.lower() != wanted.lower():
                continue
            size = struct.unpack_from('>I', e, 0x34)[0]
            block = e[0x2F] | e[0x30] << 8 | e[0x31] << 16
            if shift and not flags & 0x40:
                # Two hash tables per level: following a chain needs the
                # active one, which this doesn't pick (updates are
                # read-only packages with one table).
                sys.exit(f'{package}: fragmented file in a writable package')
            data = bytearray()
            while len(data) < size:
                chunk = d[offset(block):offset(block) + BLOCK]
                if not chunk:
                    sys.exit(f'{package}: truncated')
                data += chunk
                block = block + 1 if flags & 0x40 else next_block(block)
            with open(out, 'wb') as f:
                f.write(data[:size])
            return
        if shift and table_blocks > 1:
            sys.exit(f'{package}: file table spans blocks in a writable package')
        b = next_block(b)
    sys.exit(f'{package}: no {wanted} in it')


main()

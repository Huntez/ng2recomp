#!/usr/bin/env python3
# Lists a Xenia GPU trace's (.xtr) draws with their vertex shader constants and fetch constant 95,
# for comparing a frame against our renderer (docs/cutscene-eyes.md).
#
#   tools/xtr_draws.py <trace.xtr> [--consts 21-24] [--vb <hex physical>] [--dump <dir>]
#
# Replays register writes (type 0 packets, SET_CONSTANT, LOAD_ALU_CONSTANT) and memory reads; at
# each draw prints the index count, fetch 95's address and the selected float constants. --dump
# writes each matching draw's vertex buffer bytes.

import argparse
import os
import struct
import sys


def snappy_decompress(data):
    pos = 0
    length = 0
    shift = 0
    while True:
        b = data[pos]
        pos += 1
        length |= (b & 0x7F) << shift
        shift += 7
        if not b & 0x80:
            break
    out = bytearray()
    while pos < len(data):
        tag = data[pos]
        pos += 1
        kind = tag & 3
        if kind == 0:
            n = tag >> 2
            if n >= 60:
                extra = n - 59
                n = int.from_bytes(data[pos:pos + extra], 'little')
                pos += extra
            n += 1
            out += data[pos:pos + n]
            pos += n
            continue
        if kind == 1:
            n = ((tag >> 2) & 7) + 4
            offset = ((tag >> 5) << 8) | data[pos]
            pos += 1
        elif kind == 2:
            n = (tag >> 2) + 1
            offset = int.from_bytes(data[pos:pos + 2], 'little')
            pos += 2
        else:
            n = (tag >> 2) + 1
            offset = int.from_bytes(data[pos:pos + 4], 'little')
            pos += 4
        assert 0 < offset <= len(out)
        start = len(out) - offset
        for i in range(n):
            out.append(out[start + i])
    assert len(out) == length
    return bytes(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('trace')
    ap.add_argument('--consts', default='21-24')
    ap.add_argument('--vb', default=None)
    ap.add_argument('--dump', default=None)
    ap.add_argument('--regs', default='', help='comma-separated register indices (hex) to print at each draw')
    args = ap.parse_args()
    c0, c1 = (int(x) for x in args.consts.split('-'))
    want_vb = int(args.vb, 16) if args.vb else None

    data = open(args.trace, 'rb').read()
    regs = [0] * 0x10000
    memory = {}  # physical page (4 KB) -> bytearray

    def mem_write(base, blob):
        at = base
        off = 0
        while off < len(blob):
            page = at >> 12
            inpage = at & 0xFFF
            n = min(4096 - inpage, len(blob) - off)
            buf = memory.setdefault(page, bytearray(4096))
            buf[inpage:inpage + n] = blob[off:off + n]
            at += n
            off += n

    def mem_read(base, size):
        out = bytearray()
        at = base
        while len(out) < size:
            page = at >> 12
            inpage = at & 0xFFF
            n = min(4096 - inpage, size - len(out))
            buf = memory.get(page)
            out += buf[inpage:inpage + n] if buf else bytes(n)
            at += n
        return bytes(out)

    pos = 48
    pending = None
    draws = 0
    while pos < len(data):
        (kind,) = struct.unpack_from('<I', data, pos)
        if kind in (0, 2):  # primary/indirect buffer start
            pos += 12
        elif kind in (1, 3, 5):  # ends
            if kind == 5 and pending:
                draws += handle_packet(pending, regs, mem_read, c0, c1, want_vb, args.dump, draws,
                                       [int(r, 16) for r in args.regs.split(',') if r])
                pending = None
            pos += 4
        elif kind == 4:
            _, base, count = struct.unpack_from('<III', data, pos)
            pos += 12
            pending = struct.unpack_from('>%dI' % count, data, pos)
            pos += 4 * count
        elif kind in (6, 7):
            _, base, enc, enc_len, dec_len = struct.unpack_from('<IIIII', data, pos)
            pos += 20
            blob = data[pos:pos + enc_len]
            pos += enc_len
            if enc == 1:
                blob = snappy_decompress(blob)
            mem_write(base & 0x1FFFFFFF, blob)
        elif kind == 8:
            _, enc, enc_len = struct.unpack_from('<III', data, pos)
            pos += 12 + enc_len
        elif kind == 9:
            pos += 8
            print('swap')
        elif kind == 10:
            _, first, count, _cb, enc, enc_len = struct.unpack_from('<IIIIII', data, pos)
            pos += 24
            blob = data[pos:pos + enc_len]
            pos += enc_len
            if enc == 1:
                blob = snappy_decompress(blob)
            values = struct.unpack('<%dI' % count, blob)
            regs[first:first + count] = values
        elif kind == 11:
            _, _rw, enc, enc_len = struct.unpack_from('<IIII', data, pos)
            pos += 16 + enc_len
        else:
            sys.exit('unknown command %d at %d' % (kind, pos))


def handle_packet(words, regs, mem_read, c0, c1, want_vb, dump, draws, extra):
    i = 0
    drew = 0
    while i < len(words):
        h = words[i]
        t = h >> 30
        if t == 0:
            count = ((h >> 16) & 0x3FFF) + 1
            base = h & 0x7FFF
            one = (h >> 15) & 1
            for k in range(count):
                regs[base if one else base + k] = words[i + 1 + k]
            i += 1 + count
        elif t == 1:
            regs[h & 0x7FF] = words[i + 1]
            regs[(h >> 11) & 0x7FF] = words[i + 2]
            i += 3
        elif t == 2:
            i += 1
        else:
            count = ((h >> 16) & 0x3FFF) + 1
            op = (h >> 8) & 0x7F
            body = words[i + 1:i + 1 + count]
            if len(body) < count:  # truncated (a bad packet, or the end of the recording)
                i += 1 + count
                continue
            if op == 0x2D and body:  # SET_CONSTANT
                kind = (body[0] >> 16) & 0xFF
                index = body[0] & 0x7FF
                base = [0x4000, 0x4800, 0x4900, 0x4908, 0x2000][kind] if kind < 5 else 0
                for k, v in enumerate(body[1:]):
                    regs[base + index + k] = v
            elif op == 0x55 and body:  # SET_CONSTANT2: a register index, no type
                index = body[0] & 0xFFFF
                for k, v in enumerate(body[1:]):
                    regs[index + k] = v
            elif op == 0x2F and len(body) >= 3:  # LOAD_ALU_CONSTANT
                address = body[0] & 0x3FFFFFFF
                kind = (body[1] >> 16) & 0xFF
                index = body[1] & 0x7FF
                size = body[2] & 0xFFF
                base = [0x4000, 0x4800, 0x4900, 0x4908, 0x2000][kind] if kind < 5 else 0
                blob = mem_read(address & 0x1FFFFFFF, size * 4)
                for k in range(size):
                    regs[base + index + k] = struct.unpack_from('>I', blob, k * 4)[0]
            elif op in (0x22, 0x36) and len(body) >= (2 if op == 0x22 else 1):  # DRAW_INDX, DRAW_INDX_2
                initiator = body[1] if op == 0x22 else body[0]
                n = initiator >> 16
                # vertex fetch constant 95: fetch constants at 0x4800, two dwords per vertex fetch.
                f0 = regs[0x4800 + 95 * 2]
                f1 = regs[0x4800 + 95 * 2 + 1]
                vb = f0 & 0x1FFFFFFC
                size = ((f1 >> 2) & 0xFFFFFF) * 4
                if want_vb is None or vb == want_vb:
                    floats = []
                    for c in range(c0, c1 + 1):
                        floats.append(struct.unpack('<4f', struct.pack('<4I', *regs[0x4000 + c * 4:0x4000 + c * 4 + 4])))
                    m = ' | '.join(' '.join('%.4f' % x for x in row) for row in floats)
                    shown = ' '.join('%X=%08X' % (r, regs[r]) for r in extra)
                    # Indices from memory (source select 0): DRAW_INDX carries base and size
                    # after the initiator, DRAW_INDX_2 right after it.
                    dma = body[2:4] if op == 0x22 else body[1:3]
                    if (initiator >> 6) & 3 == 0 and len(dma) == 2:
                        index_size = 4 if (initiator >> 11) & 1 else 2
                        index_base = dma[0] & 0x1FFFFFFF & ~(index_size - 1)
                        index_bytes = (dma[1] & 0xFFFFFF) * index_size
                        shown += ' indices %08X+%X' % (index_base, index_bytes)
                        if dump:
                            os.makedirs(dump, exist_ok=True)
                            open(os.path.join(dump, 'ib-%d-%08X.bin' % (draws + drew, index_base)), 'wb').write(
                                mem_read(index_base, index_bytes))
                    print('draw %d count %d vb %08X size %d %s c%d-%d: %s' % (draws + drew, n, vb, size, shown, c0, c1, m))
                    if dump and size:
                        os.makedirs(dump, exist_ok=True)
                        open(os.path.join(dump, 'vb-%d-%08X.bin' % (draws + drew, vb)), 'wb').write(mem_read(vb, size))
                drew += 1
            i += 1 + count
    return drew


if __name__ == '__main__':
    main()

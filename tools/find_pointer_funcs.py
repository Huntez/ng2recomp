#!/usr/bin/env python3
"""List code addresses that data points at but codegen did not make functions.

Indirect calls (vtables without RTTI, callback tables) to such addresses die at
runtime with "Call to invalid or unregistered function". This scans an image
dumped with NG2_DUMP_IMAGE for big-endian words outside the code range that
point into it, and keeps those that look like a function entry: not already a
function or a label inside one, and right after a terminator or padding.

usage: find_pointer_funcs.py <image.bin> <generated_dir> [--toml]
"""
import glob
import os
import re
import struct
import sys

IMAGE_BASE = 0x82000000
CODE_BASE = 0x822F0000
CODE_END = CODE_BASE + 0x1797D30  # Title Updates 2 and 3 (1.0: 0x1771D30)

BLR, BCTR = 0x4E800020, 0x4E800420
# mr r8,r8: the compiler brackets __try regions with these. SEH scope tables
# point at __except/__finally labels inside the parent, never at functions.
SEH_MARKER = 0x7D084378
# Primary opcodes whose rt field is a source (stores, branches, compares, traps).
STORE_OPS = {2, 3, 10, 11, 16, 17, 18, 19, 36, 37, 38, 39, 44, 45, 47, 52, 53, 54, 55, 62}
# Opcode 31 extended opcodes that store rt (stwx, stbx, sthx, stdx, ... ).
X_STORES = {149, 151, 150, 183, 215, 247, 407, 439, 662, 918, 181, 214, 0, 32, 4, 68}


def read_generated(gen_dir):
    funcs, labels = set(), set()
    fn = re.compile(r"DEFINE_REX_FUNC\((?:sub|rexcrt)_([0-9A-Fa-f]{8})\)")
    lab = re.compile(r"^loc_([0-9A-F]{8}):")
    for path in glob.glob(os.path.join(gen_dir, "*_recomp.*.cpp")):
        with open(path) as f:
            for line in f:
                m = fn.search(line)
                if m:
                    funcs.add(int(m.group(1), 16))
                    continue
                m = lab.match(line)
                if m:
                    labels.add(int(m.group(1), 16))
    # Registered names (named and rexcrt functions keep their address here).
    reg = os.path.join(gen_dir, "ng2_register.cpp")
    if os.path.exists(reg):
        with open(reg) as f:
            for m in re.finditer(r"SetFunction\(0x([0-9A-F]{8})", f.read()):
                funcs.add(int(m.group(1), 16))
    return funcs, labels


def word(img, addr):
    off = addr - IMAGE_BASE
    return struct.unpack_from(">I", img, off)[0]


def is_terminator(w):
    if w in (BLR, BCTR, 0):
        return True
    return (w >> 26) == 18 and (w & 1) == 0  # b / ba, not bl


def main():
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    img = open(sys.argv[1], "rb").read()
    funcs, labels = read_generated(sys.argv[2])
    refs = {}
    # Addresses built in code: lis rX,hi then addi/ori rY,rX,lo.
    hi = {}
    for addr in range(CODE_BASE, CODE_END, 4):
        w = word(img, addr)
        op, rt, ra = w >> 26, (w >> 21) & 31, (w >> 16) & 31
        if op == 15 and ra == 0:  # lis rt,hi
            hi[rt] = (w & 0xFFFF) << 16
            continue
        # addi rt,ra,lo reads ra and writes rt; ori ra,rs,lo reads rs (rt field).
        src, dst = (ra, rt) if op == 14 else (rt, ra) if op == 24 else (None, None)
        if src is not None and src in hi:
            lo = w & 0xFFFF
            if op == 14 and lo & 0x8000:
                lo -= 0x10000
            v = (hi[src] + lo) & 0xFFFFFFFF
            if CODE_BASE <= v < CODE_END and not v & 3 and v not in funcs and v not in labels:
                refs.setdefault(v, []).append(addr)
        if w in (BLR, BCTR):
            hi.clear()
        elif dst is not None:
            hi.pop(dst, None)
        elif op not in STORE_OPS and op != 31:
            hi.pop(rt, None)  # D-form loads and arithmetic write rt
        elif op == 31 and ((w >> 1) & 0x3FF) not in X_STORES:
            hi.pop(rt, None)
    for off in range(0, len(img) - 3, 4):
        addr = IMAGE_BASE + off
        if CODE_BASE <= addr < CODE_END:
            continue
        v = struct.unpack_from(">I", img, off)[0]
        if not (CODE_BASE <= v < CODE_END) or v & 3:
            continue
        if v in funcs or v in labels:
            continue
        refs.setdefault(v, []).append(addr)
    found = []
    for t in sorted(refs):
        w = word(img, t)
        if w == 0 or not is_terminator(word(img, t - 4)):
            continue
        if CODE_BASE <= w < CODE_END:  # a jump table after its bctr
            continue
        if any(word(img, t - 4 * i) == SEH_MARKER for i in range(2, 8)):
            continue
        found.append(t)
    if "--toml" in sys.argv:
        for t in found:
            print(f"0x{t:08X} = {{}}  # from 0x{refs[t][0]:08X}")
    else:
        for t in found:
            print(f"{t:08X} refs={len(refs[t])} first=0x{refs[t][0]:08X} insn={word(img, t):08X}")
        print(f"{len(found)} candidates ({len(refs)} unmatched code pointers)", file=sys.stderr)


if __name__ == "__main__":
    main()

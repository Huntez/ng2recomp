"""Map the guest addresses the code uses from one build of the game to another (1.0 -> TU2).

usage: map_addresses.py <image_1.0> <image_tu2> <repo>

Code: masked instruction windows (branch displacements and lis-built address
immediates dropped), matched by rolling hash, must be unique in TU2.
Data/rdata: find 1.0 code sites building the address (lis + addi/load/store),
map each site, read the address TU2 builds there; vote.
"""
import re, struct, sys, os, collections
import numpy as np

BASE = 0x82000000


def sections(img):
    pe = struct.unpack_from('<I', img, 0x3c)[0]
    ns = struct.unpack_from('<H', img, pe + 6)[0]
    osz = struct.unpack_from('<H', img, pe + 20)[0]
    o = pe + 24 + osz
    out = {}
    for _ in range(ns):
        n = img[o:o + 8].rstrip(b'\0').decode()
        vs, va = struct.unpack_from('<II', img, o + 8)
        out.setdefault(n, (BASE + va, BASE + va + vs))
        o += 40
    return out


class Img:
    def __init__(self, path):
        self.b = open(path, 'rb').read()
        self.sec = sections(self.b)
        self.t0, self.t1 = self.sec['.text']
        raw = np.frombuffer(self.b, dtype='>u4')
        self.ins = raw[(self.t0 - BASE) // 4:(self.t1 - BASE) // 4].astype(np.uint32)
        self.mask = self._masked()

    def _masked(self):
        ins = self.ins
        op = ins >> 26
        m = ins.copy()
        # b/bl: keep opcode + AA/LK
        sel = op == 18
        m[sel] &= 0xFC000003
        # bc: keep BO/BI + AA/LK
        sel = op == 16
        m[sel] &= 0xFFFF0003
        # lis (addis rD,0,imm): drop imm
        lis = (op == 15)
        m[lis] &= 0xFFFF0000
        # D-form using a register set by a lis in the previous 8 insns: drop imm
        rA = (ins >> 16) & 31
        lis_rd = np.where(lis, (ins >> 21) & 31, 32)
        dform = np.isin(op, [14, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 48, 49, 50, 51, 52, 53, 54, 55, 58, 62])
        hit = np.zeros(len(ins), bool)
        for k in range(1, 9):
            prev = np.full(len(ins), 32, dtype=np.int64)
            prev[k:] = lis_rd[:-k]
            hit |= dform & (prev == rA) & (rA != 0)
        m[hit] &= 0xFFFF0000
        return m

    def word(self, a):
        return struct.unpack_from('>I', self.b, a - BASE)[0]


W = 12


def window_hashes(m):
    # rolling 64-bit hash over W masked words
    v = m.astype(np.uint64)
    h = np.zeros(len(v) - W + 1, dtype=np.uint64)
    P = np.uint64(0x100000001B3)
    with np.errstate(over='ignore'):
        for k in range(W):
            h = h * P + v[k:k + len(h)] * np.uint64(0x9E3779B97F4A7C15) + np.uint64(k)
    return h


def build_index(h):
    order = np.argsort(h, kind='stable')
    return h[order], order


def lookup(idx, key):
    hs, order = idx
    lo = np.searchsorted(hs, key, 'left')
    hi = np.searchsorted(hs, key, 'right')
    return order[lo:hi]


def map_code(a, A, B, hA, idxB, anchor=None):
    """Map a code address: windows at a+-4k; among candidates prefer the one
    nearest the expected (anchor) delta."""
    i = (a - A.t0) // 4
    for back in (0, 4, 8, 2, 6, 10, 1, 3, -4, -8, -12, -16, -24, -32):
        j = i - back
        if j < 0 or j >= len(hA):
            continue
        cands = lookup(idxB, hA[j])
        if len(cands) == 1:
            return B.t0 + 4 * (int(cands[0]) + back), f'window{back:+d}'
        if 1 < len(cands) <= 8 and anchor is not None:
            ts = sorted((abs(B.t0 + 4 * (int(c) + back) - (a + anchor)), B.t0 + 4 * (int(c) + back)) for c in cands)
            if ts[0][0] < 0x4000 and ts[1][0] > 0x40000:
                return ts[0][1], f'nearest-of-{len(cands)}'
    return None, 'no unique window'


ANCH = None


def anchors(A, B, hA, idxB):
    """Global map of unique windows every 64 insns -> sorted (a, delta)."""
    out = []
    for j in range(0, len(hA), 64):
        c = lookup(idxB, hA[j])
        if len(c) == 1:
            out.append((A.t0 + 4 * j, B.t0 + 4 * int(c[0]) - (A.t0 + 4 * j)))
    return out


def expected_delta(anc, a):
    import bisect
    k = bisect.bisect(anc, (a, 1 << 40))
    near = anc[max(0, k - 5):k + 5]
    ds = sorted(d for _, d in near)
    return ds[len(ds) // 2]


def build_sites(A, addr):
    """1.0 sites that build addr: lis rX,hi then D-form using rX within 8 insns."""
    ins = A.ins
    hi = (addr + 0x8000) >> 16 & 0xFFFF
    lo = addr & 0xFFFF
    op = ins >> 26
    lis_idx = np.nonzero((op == 15) & (((ins >> 16) & 31) == 0) & ((ins & 0xFFFF) == hi))[0]
    sites = []
    for i in lis_idx:
        rd = (int(ins[i]) >> 21) & 31
        for k in range(1, 9):
            j = i + k
            if j >= len(ins):
                break
            w = int(ins[j])
            o = w >> 26
            if o in (14, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 48, 49, 50, 51, 52, 53, 54, 55) and ((w >> 16) & 31) == rd and (w & 0xFFFF) == lo:
                sites.append((int(i), j - int(i)))
                break
    return sites


def built_addr(B, i_lis, k):
    w0 = int(B.ins[i_lis]); w1 = int(B.ins[i_lis + k])
    if w0 >> 26 != 15:
        return None
    hi = w0 & 0xFFFF
    lo = w1 & 0xFFFF
    if lo & 0x8000:
        lo -= 0x10000
    return ((hi << 16) + lo) & 0xFFFFFFFF


def map_data(a, A, B, hA, idxB):
    votes = collections.Counter()
    sites = build_sites(A, a)
    for i, k in sites[:40]:
        for back in (0, 2, 4, 6):
            j = i - back
            if j < 0 or j >= len(hA):
                continue
            c = lookup(idxB, hA[j])
            if len(c) == 1:
                t = built_addr(B, int(c[0]) + back, k)
                if t is not None:
                    votes[t] += 1
                break
    if not sites:
        for d in range(4, 0x2000, 4):
            s2 = build_sites(A, a - d)
            if s2:
                t, how = map_data(a - d, A, B, hA, idxB)
                if t is not None:
                    return t + d, f'base {a - d:08X}+{d:#x}: ' + how
                break
        return None, 'no lis site'
    if not votes:
        return None, f'{len(sites)} sites, none mapped'
    (best, n), = votes.most_common(1)
    tot = sum(votes.values())
    return best, f'{n}/{tot} sites agree' + ('' if n == tot else ' (conflict)')


def collect(repo):
    pat = re.compile(r'(?:0x|sub_|loc_)(8[2-5][0-9A-Fa-f]{6})\b')
    refs = collections.defaultdict(list)
    files = []
    for root in ('src',):
        for dp, _, fs in os.walk(os.path.join(repo, root)):
            files += [os.path.join(dp, f) for f in fs if f.endswith(('.cpp', '.h'))]
    files += [os.path.join(repo, 'ng2_codegen.toml')]
    for f in files:
        for ln, line in enumerate(open(f, errors='replace'), 1):
            for m in pat.finditer(line):
                refs[int(m.group(1), 16)].append(f'{os.path.relpath(f, repo)}:{ln}')
    return refs


def main():
    A = Img(sys.argv[1]); B = Img(sys.argv[2]); repo = sys.argv[3]
    hA = window_hashes(A.mask); hB = window_hashes(B.mask)
    idxB = build_index(hB)
    refs = collect(repo)
    anc = anchors(A, B, hA, idxB)
    res = collections.Counter()
    rows = []
    for a in sorted(refs):
        if A.t0 <= a < A.t1:
            kind = 'code'
            exp = expected_delta(anc, a)
            t, how = map_code(a, A, B, hA, idxB, exp)
            if t is not None and abs((t - a) - exp) > 0x4000:
                how += f' FAR (expected ~{a + exp:08X})'
                t = None
        elif a < A.t0:
            kind = 'rdata'
            t, how = map_data(a, A, B, hA, idxB)
        else:
            kind = 'data'
            t, how = map_data(a, A, B, hA, idxB)
        status = 'ok' if t is not None and 'conflict' not in how else ('conflict' if t else 'manual')
        res[(kind, status)] += 1
        rows.append((a, kind, t, status, how, refs[a]))
    for a, kind, t, status, how, r in rows:
        ts = f'{t:08X}' if t is not None else '--------'
        print(f'{a:08X} {kind:5s} -> {ts} {status:8s} {how:28s} {len(r)} refs: {", ".join(sorted(set(x.split(":")[0] for x in r)))}')
    print()
    for k, v in sorted(res.items()):
        print(k, v)
    print('total', len(rows))


main()

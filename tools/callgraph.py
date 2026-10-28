#!/usr/bin/env python3
"""Query the call graph of the generated code.

usage: callgraph.py <generated_dir> callers <addr>...
       callgraph.py <generated_dir> callees <addr>...
       callgraph.py <generated_dir> imports <addr>...   (kernel imports called)
       callgraph.py <generated_dir> asm <addr>          (instruction comments)
       callgraph.py <generated_dir> users <import>      (functions calling an import)

Addresses are hex without 0x. The index is cached next to the generated code.
"""
import glob
import json
import os
import re
import sys

FN = re.compile(r"DEFINE_REX_FUNC\((?:sub|rexcrt)_([0-9A-Fa-f]{8})\)")
CALL = re.compile(r"\b(sub_[0-9A-F]{8}|__imp__\w+|rexcrt_\w+)\(ctx")


def build(gen_dir):
    funcs = {}
    for path in sorted(glob.glob(os.path.join(gen_dir, "*_recomp.*.cpp"))):
        cur = None
        with open(path) as f:
            for i, line in enumerate(f, 1):
                m = FN.search(line)
                if m:
                    cur = m.group(1).upper()
                    funcs[cur] = {"file": os.path.basename(path), "line": i,
                                  "calls": [], "n": 0}
                    continue
                if cur is None:
                    continue
                s = line.strip()
                if s.startswith("// "):
                    funcs[cur]["n"] += 1
                for c in CALL.findall(line):
                    if c not in funcs[cur]["calls"]:
                        funcs[cur]["calls"].append(c)
    return funcs


def load(gen_dir):
    cache = os.path.join(gen_dir, ".callgraph.json")
    stamp = os.path.join(gen_dir, "codegen.stamp")
    if os.path.exists(cache) and (not os.path.exists(stamp) or
                                  os.path.getmtime(cache) > os.path.getmtime(stamp)):
        with open(cache) as f:
            return json.load(f)
    funcs = build(gen_dir)
    with open(cache, "w") as f:
        json.dump(funcs, f)
    return funcs


def asm(gen_dir, funcs, addr):
    info = funcs[addr]
    out = []
    with open(os.path.join(gen_dir, info["file"])) as f:
        for i, line in enumerate(f, 1):
            if i < info["line"]:
                continue
            if i > info["line"] and line.startswith("}"):
                break
            s = line.strip()
            if s.startswith("// ") or s.startswith("loc_"):
                out.append(s[3:] if s.startswith("// ") else s)
    return out


def main():
    if len(sys.argv) < 4:
        raise SystemExit(__doc__)
    gen_dir, cmd, args = sys.argv[1], sys.argv[2], [a.upper() for a in sys.argv[3:]]
    funcs = load(gen_dir)
    if cmd == "callees":
        for a in args:
            print(a, funcs[a]["n"], " ".join(funcs[a]["calls"]))
    elif cmd == "imports":
        for a in args:
            print(a, " ".join(c for c in funcs[a]["calls"] if c.startswith("__imp__")))
    elif cmd == "callers":
        for a in args:
            who = sorted(k for k, v in funcs.items() if "sub_" + a in v["calls"])
            print(a, len(who), " ".join(who))
    elif cmd == "users":
        name = "__imp__" + sys.argv[3]
        print(" ".join(sorted(k for k, v in funcs.items() if name in v["calls"])))
    elif cmd == "asm":
        print("\n".join(asm(gen_dir, funcs, args[0])))
    else:
        raise SystemExit(__doc__)


if __name__ == "__main__":
    main()

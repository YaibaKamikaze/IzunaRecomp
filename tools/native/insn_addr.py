#!/usr/bin/env python3
"""Print guest addresses of instructions in a recompiled function.

  tools/native/insn_addr.py 0x8373D570 'stw r7,10776(r31)' ['bl 0x8373cfd0' ...]

Walks generated/default/*.cpp: each '\t// <insn>' comment is one 4-byte
instruction from the function start; 'loc_XXXXXXXX:' labels resync the count.
Used to place midasm hooks (config/hooks/*.toml) at a specific instruction.
"""
import glob, re, sys

start = int(sys.argv[1], 16)
wanted = sys.argv[2:]
name = f"DEFINE_REX_FUNC(sub_{start:08X})"
for path in glob.glob("generated/default/*.cpp"):
    text = open(path).read()
    i = text.find(name)
    if i < 0:
        continue
    addr = start
    for line in text[i:].splitlines()[1:]:
        if line.startswith("DEFINE_REX_FUNC"):
            break
        m = re.match(r"loc_([0-9A-F]{8}):", line)
        if m:
            addr = int(m.group(1), 16)
            continue
        if line.startswith("\t// "):
            insn = line[4:].strip()
            if not wanted or any(w == insn or w in insn for w in wanted):
                print(f"{addr:08X}  {insn}")
            addr += 4
    break

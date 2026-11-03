"""[native renderer, M0] Profile every function in NG2's Direct3D library block.

    python3 tools/native/d3d_map.py [generated/default] > docs/native/d3d_map.txt

Reads the recompiled code (game-derived: the output stays out of commits like
the code itself) and, for each function in 0x83734000-0x83750000, reports:
its size, who calls it from outside the block (the game's calls: those are the
public Direct3D entry points), its callees in the block, the kernel exports it
calls, the GPU command packet headers it builds (type 3: opcode and count), and
the device-structure offsets it touches through the pointer it received in r3.
"""
import collections
import glob
import os
import re
import sys

LO, HI = 0x83734000, 0x83750000

PM4_OPCODES = {  # Xenos type-3 packet opcodes (Xenia's xenos.h names)
    0x10: "NOP", 0x12: "REG_RMW", 0x13: "INTERRUPT", 0x15: "INDIRECT_BUFFER_PFD",
    0x22: "DRAW_INDX", 0x25: "XE_SWAP?", 0x2D: "SET_CONSTANT", 0x2E: "SET_CONSTANT2",
    0x2F: "LOAD_ALU_CONSTANT", 0x30: "SET_SHADER_CONSTANTS", 0x36: "DRAW_INDX_2",
    0x37: "INDIRECT_BUFFER", 0x3C: "WAIT_REG_MEM", 0x3D: "MEM_WRITE", 0x3E: "REG_TO_MEM",
    0x3F: "INDIRECT_BUFFER", 0x43: "IM_LOAD", 0x44: "COND_WRITE", 0x45: "IM_LOAD_IMMEDIATE",
    0x46: "EVENT_WRITE", 0x47: "SET_BIN_MASK_LO", 0x48: "SET_BIN_SELECT_LO",
    0x49: "EVENT_WRITE_SHD", 0x4A: "INVALIDATE_STATE", 0x4C: "VIZ_QUERY", 0x4D: "SET_SHADER_BASES?",
    0x50: "SET_BIN_MASK", 0x51: "SET_BIN_SELECT", 0x54: "MEM_WRITE_CNTR", 0x58: "EVENT_WRITE_EXT",
    0x59: "EVENT_WRITE_ZPD", 0x5A: "CONTEXT_UPDATE", 0x5B: "INTERRUPT", 0x5C: "XE_SWAP",
    0x60: "SET_BIN_MASK_HI", 0x61: "SET_BIN_SELECT_HI",
}


def load(gen):
    funcs = {}
    for path in glob.glob(os.path.join(gen, "ng2_recomp.*.cpp")):
        cur, body = None, []
        for line in open(path, errors="replace"):
            m = re.match(r"DEFINE_REX_FUNC\(sub_([0-9A-F]{8})\)", line)
            if m:
                if cur is not None:
                    funcs[cur] = body
                cur, body = int(m.group(1), 16), []
            elif cur is not None:
                body.append(line.rstrip("\n"))
        if cur is not None:
            funcs[cur] = body
    return funcs


def asm(body):
    return [l.strip()[3:] for l in body if l.strip().startswith("// ")]


def main():
    gen = sys.argv[1] if len(sys.argv) > 1 else "generated/default"
    funcs = load(gen)
    callers = collections.defaultdict(set)
    for a, body in funcs.items():
        for l in asm(body):
            m = re.match(r"bl 0x([0-9a-f]{8})$", l)
            if m:
                callers[int(m.group(1), 16)].add(a)
    block = sorted(a for a in funcs if LO <= a < HI)
    print(f"# NG2 Direct3D block 0x{LO:08X}-0x{HI:08X}: {len(block)} functions")
    print("# addr | instrs | ext callers | int callers | kernel calls | packets | device offsets (r3)")
    for a in block:
        ins = asm(funcs[a])
        ext = sorted(c for c in callers.get(a, ()) if not LO <= c < HI)
        intern = sorted(c for c in callers.get(a, ()) if LO <= c < HI)
        kernel = sorted(set(re.findall(r"__imp__([A-Za-z0-9_]+)\(ctx", "\n".join(funcs[a]))))
        # PM4 type-3 headers built with lis/ori pairs into the same register
        hi = {}
        packets = []
        for l in ins:
            m = re.match(r"lis (r\d+),(-?\d+)$", l)
            if m:
                hi[m.group(1)] = int(m.group(2)) & 0xFFFF
                continue
            m = re.match(r"ori (r\d+),(r\d+),(\d+)$", l)
            if m and m.group(2) in hi:
                v = (hi[m.group(2)] << 16) | int(m.group(3))
                if v >> 30 == 3:  # type 3
                    op = (v >> 8) & 0x7F
                    packets.append(f"{PM4_OPCODES.get(op, hex(op))}/{((v >> 16) & 0x3FFF) + 1}")
        # device offsets: registers that copy r3 at entry, then d(reg) accesses
        dev_regs = {"r3"}
        offs = collections.Counter()
        for l in ins[:12]:
            m = re.match(r"mr (r\d+),r3$", l)
            if m:
                dev_regs.add(m.group(1))
        for l in ins:
            m = re.match(r"(lwz|stw|lhz|sth|lbz|stb|ld|std|lfs|stfs)u? r\d+,(-?\d+)\((r\d+)\)$", l)
            if m and m.group(3) in dev_regs:
                offs[(m.group(1)[0] == "s" and "w" or "r") + m.group(2)] += 1
        top = " ".join(sorted(offs, key=lambda k: int(k[1:]))[:10])
        # The other arguments (r4-r6): offsets read through each (resource
        # structures: D3DTexture.Format at 0x1C, a buffer's fetch words at
        # 0x18/0x1C, ...), or "v" when the value is only used as a number.
        arginfo = []
        for reg in ("r4", "r5", "r6"):
            names = {reg}
            for l in ins[:16]:
                m = re.match(rf"mr (r\d+),{reg}$", l)
                if m:
                    names.add(m.group(1))
            ro = sorted({int(m.group(1)) for l in ins
                         for m in [re.match(r"(?:lwz|lhz|lbz|ld|lfs|stw|sth|stb)u? r\d+,(-?\d+)\((r\d+)\)$", l)]
                         if m and m.group(2) in names})
            used = any(re.search(rf"\b{r}\b", l) for l in ins for r in names)
            arginfo.append(f"{reg}:{'/'.join(hex(o) for o in ro[:6])}" if ro else (f"{reg}:v" if used else ""))
        print(f"{a:08X} | {len(ins):4d} | {len(ext):3d} | {len(intern):3d} | {','.join(kernel) or '-'} | "
              f"{','.join(sorted(set(packets))) or '-'} | {top} | {' '.join(x for x in arginfo if x)}")


if __name__ == "__main__":
    main()

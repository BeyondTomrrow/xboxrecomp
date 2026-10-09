"""Find functions that hand their flags back to the caller.

    python -m tools.recomp.flag_return_scan <xbe> <disasm-dir> <out.json>

A function whose every `ret` comes straight after a cmp or test (with only
flag-preserving instructions between) returns its answer in EFLAGS. Mod code
caves do this when they replace an inline `cmp` with a `call`: the caller's
next `jbe`/`jne` reads flags the callee set. Lifted one function at a time,
that branch has no flag setter and fell back to "never taken".

The output lists those functions. The lifter (RECOMP_FLAG_RETURN_FUNCS=<json>)
then makes each of them store its last cmp/test at return (g_rf_*), and makes
a conditional branch right after a direct call to one of them read it.
"""
import json
import struct
import sys

import capstone

# Instructions that leave EFLAGS alone and may sit between the compare and ret.
PRESERVE = {"mov", "movzx", "movsx", "pop", "push", "lea", "nop", "xchg", "cwde", "cdq",
            "popal", "popad", "pushal", "pushad", "movd", "movq", "movss", "movsd", "movaps", "movups",
            "fld", "fstp", "fst", "fxch", "fldz", "fld1"}


def sections(data):
    base = struct.unpack_from("<I", data, 0x104)[0]
    nsec, sa = struct.unpack_from("<II", data, 0x11C)
    so = sa - base
    out = []
    for i in range(nsec):
        fl, va, vsz, raw, rsz, na = struct.unpack_from("<IIIIII", data, so + i * 56)
        out.append((va, rsz, raw))
    return out


def read(data, secs, va, n):
    for sva, rsz, raw in secs:
        if sva <= va < sva + rsz:
            off = raw + va - sva
            return data[off:off + min(n, sva + rsz - va)]
    return b""


def main():
    xbe, disasm_dir, out = sys.argv[1:4]
    data = open(xbe, "rb").read()
    secs = sections(data)
    funcs = json.load(open(disasm_dir + "/functions.json"))
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    found = []
    for f in funcs:
        start = int(f["start"], 16)
        size = int(f.get("size") or 0)
        if size <= 0 or size > 0x20000:
            continue
        insns = list(md.disasm(read(data, secs, start, size), start))
        rets = 0
        ok = True
        for k, ins in enumerate(insns):
            if ins.mnemonic not in ("ret", "retn"):
                continue
            rets += 1
            j = k - 1
            while j >= 0 and insns[j].mnemonic in PRESERVE:
                j -= 1
            # XOR leaves the same condition flags as `test result,result`
            # (ZF/SF/PF from the result, CF/OF clear). Mod caves commonly use
            # `xor eax,eax; pop eax; ret` to return false in EFLAGS.
            if j < 0 or insns[j].mnemonic not in ("cmp", "test", "xor"):
                ok = False
                break
        if ok and rets:
            found.append(f["start"])
    # Call targets that are not function starts in this analysis (seeds and
    # tail splits move boundaries: SOFTDRINK v0.4's 128 MB detector 0x14EAE05
    # sits inside the function the analysis starts at 0x14EADF8). A direct call
    # target whose straight-line code (calls and forward branches allowed, no
    # jmp) reaches a ret right after a cmp/test/xor answers in EFLAGS too.
    listed = {int(x, 16) for x in found}
    targets = set()
    for f in funcs:
        start = int(f["start"], 16)
        size = int(f.get("size") or 0)
        if size <= 0 or size > 0x20000:
            continue
        for ins in md.disasm(read(data, secs, start, size), start):
            if ins.mnemonic == "call" and ins.op_str.startswith("0x"):
                targets.add(int(ins.op_str, 16))
    for t in sorted(targets - listed):
        insns = list(md.disasm(read(data, secs, t, 0x400), t))[:96]
        for k, ins in enumerate(insns):
            if ins.mnemonic in ("jmp", "int3", "hlt", "ud2"):
                break
            if ins.mnemonic in ("ret", "retn"):
                j = k - 1
                while j >= 0 and insns[j].mnemonic in PRESERVE:
                    j -= 1
                if j >= 0 and insns[j].mnemonic in ("cmp", "test", "xor"):
                    found.append("0x%08X" % t)
                break
    # Flag-entry functions: entered by a push-ret or jmp straight after a
    # cmp/test (a detour cave going back into the original code), whose first
    # block branches on those flags before setting any.
    starts = {int(f["start"], 16) for f in funcs}
    entry = set()
    pushret = 0
    for f in funcs:
        start = int(f["start"], 16)
        size = int(f.get("size") or 0)
        if size <= 0 or size > 0x20000:
            continue
        insns = list(md.disasm(read(data, secs, start, size), start))
        for k, ins in enumerate(insns):
            target = None
            if ins.mnemonic == "push" and k + 1 < len(insns) and insns[k + 1].mnemonic in ("ret", "retn")                     and not insns[k + 1].op_str and ins.op_str.startswith("0x"):
                target = int(ins.op_str, 16)
                pushret += 1
            elif ins.mnemonic == "jmp" and ins.op_str.startswith("0x"):
                t = int(ins.op_str, 16)
                if not (start <= t < start + size):
                    target = t
            if target is None or target not in starts:
                continue
            j = k - 1
            while j >= 0 and insns[j].mnemonic in PRESERVE:
                j -= 1
            if j >= 0 and insns[j].mnemonic in ("cmp", "test"):
                entry.add(target)
    # Direct call targets in code the retail XBE does not have (mod caves),
    # with the largest `ret N` they use. A cave can drop its own return
    # address and jump into the caller's function body ("call as jmp", e.g.
    # SOFTDRINK 2K28 v0.4's 0x014DDE5F for 0x1CEAC0); the guest then returns
    # past the call site. The lifter checks the stack after such calls.
    retail_like = {".text", "D3D", "DSOUND", "WMADEC", "XGRPH", "XNET", "XONLINE",
                   "PSFD_I", "PSFD_B", "PSFD_P", "PSFD00", "XPP", "XON_RD"}
    caves = {}
    base = struct.unpack_from("<I", data, 0x104)[0]
    nsec, sa = struct.unpack_from("<II", data, 0x11C)
    secs_named = []
    for i in range(nsec):
        fl, sva, vsz, raw, rsz, na = struct.unpack_from("<IIIIII", data, sa - base + i * 56)
        no = na - base
        secs_named.append((data[no:data.index(b"\0", no)].decode(errors="replace"), fl, sva, rsz, raw))
    for t in sorted(targets):
        sec = None
        for name, fl, sva, rsz, raw in secs_named:
            if sva <= t < sva + rsz:
                sec = name
                break
        if sec is None or sec in retail_like:
            continue
        # Only caves that drop their own return address (`add esp, 4` /
        # `lea esp, [esp + 4]` / `pop reg` before any push) can return past
        # the call. A cave that rebuilds a prologue and jumps into retail
        # code ends with that code's `ret N` (SOFTDRINK v0.4's 0x14DA41A
        # jumps into 0x17B016, `ret 4`): follow direct jmps for the max N.
        # Walk the straight-line code (branches fall through) tracking the
        # stack depth; the cave drops its return address when esp rises
        # above its entry value (pushfd; pushal; ...; popal; popfd;
        # add esp, 4; <prologue>; jmp body).
        drops = False
        depth = 0
        for ins in list(md.disasm(read(data, secs, t, 0x400), t))[:120]:
            m, o = ins.mnemonic, ins.op_str
            if m in ("push", "pushfd", "pushf"):
                depth += 4
            elif m in ("pushal", "pushad"):
                depth += 32
            elif m in ("pop", "popfd", "popf"):
                depth -= 4
            elif m in ("popal", "popad"):
                depth -= 32
            elif m in ("add", "sub") and o.startswith("esp, ") and o[5:].startswith("0x") or \
                    m in ("add", "sub") and o.startswith("esp, ") and o[5:].isdigit():
                n = int(o[5:], 0)
                depth += -n if m == "add" else n
            elif m == "lea" and o.startswith("esp, [esp + "):
                depth -= int(o[len("esp, [esp + "):-1], 0)
            if depth < 0:
                drops = True
                break
            if m in ("jmp", "ret", "retn", "int3"):
                break
        if not drops:
            continue
        maxret = 0
        pc, hops, budget = t, 0, 1200
        while budget > 0 and hops < 4:
            nxt = None
            for ins in md.disasm(read(data, secs, pc, 0x2000), pc):
                budget -= 1
                if ins.mnemonic in ("ret", "retn") and ins.op_str:
                    maxret = max(maxret, int(ins.op_str, 16))
                if ins.mnemonic == "jmp" and ins.op_str.startswith("0x"):
                    nxt = int(ins.op_str, 16)
                    break
                if ins.mnemonic == "int3" or budget <= 0:
                    break
            if nxt is None:
                break
            pc, hops = nxt, hops + 1
        caves["0x%08X" % t] = maxret
    # Call targets whose caller reads EFLAGS straight after the call (a jcc,
    # setcc, cmovcc, adc or sbb before any flag-setting instruction). Those
    # callees answer in the flags whatever set them last: cmp/test, an
    # arithmetic result (`xor eax,eax; inc eax; ret`), stc/clc, a parity test.
    READS = ("j", "set", "cmov", "adc", "sbb")
    consumed = set()
    for f in funcs:
        start = int(f["start"], 16)
        size = int(f.get("size") or 0)
        if size <= 0 or size > 0x20000:
            continue
        insns = list(md.disasm(read(data, secs, start, size), start))
        for k, ins in enumerate(insns):
            if ins.mnemonic != "call" or not ins.op_str.startswith("0x"):
                continue
            for nxt in insns[k + 1:k + 6]:
                mn = nxt.mnemonic
                if mn in PRESERVE:
                    continue
                if (mn.startswith("j") and mn != "jmp") or mn.startswith(READS[1:]) or mn in ("adc", "sbb"):
                    consumed.add(int(ins.op_str, 16))
                break
    for t in sorted(consumed):
        found.append("0x%08X" % t)
    json.dump({"returns": sorted(set(found)), "entries": ["0x%08X" % t for t in sorted(entry)], "caves": caves,
               "consumed": ["0x%08X" % t for t in sorted(consumed)]},
              open(out, "w"))
    print("[flag-return] %d of %d functions return their flags; %d flag-entry targets; %d push-ret jumps"
          % (len(found), len(funcs), len(entry), pushret))


if __name__ == "__main__":
    main()

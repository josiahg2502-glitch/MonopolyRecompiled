"""Find code addresses referenced from data (vtables, callback tables) that codegen didn't register.

Typical case: an empty virtual `blr`, or a function without a .pdata entry, folded into the
preceding function, which then crashes at runtime with "Call to invalid or unregistered function".

usage: find_missing_funcs.py <image.bin> <generated/default/monopoly_init.cpp> [config/monopoly.toml]
Without a config path, prints new [functions] entries. With one, merges them into it and
clamps every generated entry's end to the next known start; exit code 2 means it changed.
"""
import bisect, re, struct, sys

BASE = 0x82000000
TEXT = (0x821E0000, 0x826B4C5C)
DATA = [(0x82001000, 0x821BA860), (0x826C0000, 0x82B81C3C)]  # .rdata, .data
PDATA = (0x821BB000, 0x20F00)
BLR, BCTR = 0x4E800020, 0x4E800420


def word(img, addr):
    return struct.unpack_from(">I", img, addr - BASE)[0]


def in_text(v):
    return TEXT[0] <= v < TEXT[1] and not v & 3


def is_terminator(insn):
    # blr / bctr / unconditional b (no link) / zero padding
    return insn in (BLR, BCTR, 0) or (insn >> 26 == 18 and not insn & 1)


def main():
    img = open(sys.argv[1], "rb").read()
    registered = {int(m, 16) for m in re.findall(r"\{ 0x([0-9A-Fa-f]{8}), ", open(sys.argv[2]).read())}
    pdata = sorted(
        (begin, begin + ((data >> 8) & 0x3FFFFF) * 4)
        for begin, data in (struct.unpack_from(">II", img, PDATA[0] - BASE + i * 8) for i in range(PDATA[1] // 8))
        if begin)
    starts = [b for b, _ in pdata]

    def inside_pdata_function(t):  # SEH funclets and jump-table labels live strictly inside one
        i = bisect.bisect_right(starts, t) - 1
        return i >= 0 and starts[i] < t < pdata[i][1]

    found = {}
    for lo, hi in DATA:
        for addr in range(lo, hi - 4, 4):
            t = word(img, addr)
            if not in_text(t) or t in registered or inside_pdata_function(t):
                continue
            if not is_terminator(word(img, t - 4)):
                continue
            neighbours = [word(img, addr + d) for d in (-4, 4)]
            vtable_like = any(n in registered for n in neighbours)
            isolated = not any(in_text(n) for n in neighbours)  # a lone pointer, not a jump table
            if vtable_like or isolated or word(img, t) == BLR:
                found.setdefault(t, addr)

    # Function pointers built in code: lis rD,hi followed shortly by addi rX,rD,lo.
    # A target in the same function as the reference is a jump-table base label, not a function.
    reg_sorted = sorted(registered)

    def same_function(a, b):
        return bisect.bisect_right(reg_sorted, a) == bisect.bisect_right(reg_sorted, b)

    for addr in range(TEXT[0], TEXT[1] - 4, 4):
        lis = word(img, addr)
        if lis >> 26 != 15 or (lis >> 16) & 31:  # addis rD,0,imm
            continue
        rd = (lis >> 21) & 31
        for k in range(1, 9):
            insn = word(img, addr + 4 * k)
            if insn >> 26 == 14 and (insn >> 16) & 31 == rd:
                lo = insn & 0xFFFF
                t = (((lis & 0xFFFF) << 16) + (lo - 0x10000 if lo & 0x8000 else lo)) & 0xFFFFFFFF
                if (in_text(t) and t not in registered and not inside_pdata_function(t)
                        and not same_function(addr, t) and is_terminator(word(img, t - 4))):
                    found.setdefault(t, addr)
                break

    config_path = sys.argv[3] if len(sys.argv) > 3 else None
    lines = open(config_path, encoding="utf-8").read().splitlines() if config_path else []
    entry = re.compile(r"^0x([0-9A-F]{8}) = \{ end = 0x([0-9A-F]{8}) \}(.*)$")
    configured = {int(m.group(1), 16) for m in map(entry.match, lines) if m}
    new = {t: ref for t, ref in found.items() if t not in configured}

    # Each function runs to the next known start: .pdata, a registered function, or another entry.
    bounds = sorted(set(starts) | registered | set(found) | configured)

    def end_of(t):
        return bounds[bisect.bisect_right(bounds, t)]

    out = [f"0x{t:08X} = {{ end = 0x{end_of(t):08X} }}  # referenced from 0x{ref:08X}" for t, ref in sorted(new.items())]
    print(f"# {len(new)} new candidates", file=sys.stderr)
    if not config_path:
        print("\n".join(out))
        return
    # Merge: re-clamp existing generated entries (later passes find functions inside earlier ranges).
    for i, line in enumerate(lines):
        m = entry.match(line)
        if m:
            t = int(m.group(1), 16)
            lines[i] = f"0x{t:08X} = {{ end = 0x{min(int(m.group(2), 16), end_of(t)):08X} }}{m.group(3)}"
    open(config_path, "w", encoding="utf-8", newline="\n").write("\n".join(lines + out) + "\n")
    sys.exit(0 if not new else 2)  # 2 = config changed, rerun codegen


if __name__ == "__main__":
    main()

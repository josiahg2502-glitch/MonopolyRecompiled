"""List or extract EA BIGF archives (Monopoly's *.big).

usage: big.py <archive.big> [outdir]   (no outdir = list only)
Header: "BIGF", u32le archive size, u32be count, u32be header size; then per entry
u32be offset, u32be size, NUL-terminated name.
"""
import os, struct, sys


def entries(f):
    magic, _, count, _ = struct.unpack("<4sI", f.read(8)) + struct.unpack(">II", f.read(8))
    if magic != b"BIGF":
        sys.exit("not a BIGF archive")
    for _ in range(count):
        off, size = struct.unpack(">II", f.read(8))
        name = b""
        while (c := f.read(1)) not in (b"\0", b""):
            name += c
        yield name.decode("latin-1"), off, size


def main():
    out = sys.argv[2] if len(sys.argv) > 2 else None
    with open(sys.argv[1], "rb") as f:
        items = list(entries(f))
        for name, off, size in items:
            if not out:
                print(f"{size:>10}  {name}")
                continue
            dst = os.path.join(out, *name.replace("\\", "/").split("/"))
            if os.path.commonpath([os.path.abspath(out), os.path.abspath(dst)]) != os.path.abspath(out):
                sys.exit(f"refusing path outside outdir: {name}")
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            f.seek(off)
            with open(dst, "wb") as o:
                o.write(f.read(size))
        print(f"{len(items)} entries")


if __name__ == "__main__":
    main()

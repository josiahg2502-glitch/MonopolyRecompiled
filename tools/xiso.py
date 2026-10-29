"""List or extract an Xbox 360 XDVDFS (XGD) disc image.

usage: xiso.py <iso> [outdir]   (no outdir = list only)
"""
import os, struct, sys

SECTOR = 2048
MAGIC = b"MICROSOFT*XBOX*MEDIA"
PARTITIONS = (0, 0xFD90000, 0xFDA0000, 0x2080000, 0x18300000)  # rebuilt, XGD2 (redump), XGD2, XGD3, XGD1


def find_partition(f):
    for base in PARTITIONS:
        f.seek(base + 32 * SECTOR)
        if f.read(20) == MAGIC:
            return base
    sys.exit("not an XDVDFS image")


def walk(f, base, sector, size, path=""):
    f.seek(base + sector * SECTOR)
    table = f.read(size)
    stack, seen = [0], set()
    while stack:
        off = stack.pop()
        if off in seen or off + 14 > len(table):
            continue
        seen.add(off)
        left, right, start, length, attr, nlen = struct.unpack_from("<HHIIBB", table, off)
        if left == 0xFFFF:  # padding at end of a sector
            continue
        name = table[off + 14:off + 14 + nlen].decode("latin-1")
        full = f"{path}/{name}"
        if attr & 0x10:
            if length:
                yield from walk(f, base, start, length, full)
        else:
            yield full, base + start * SECTOR, length
        if left:
            stack.append(left * 4)
        if right:
            stack.append(right * 4)


def main():
    iso = sys.argv[1]
    out = sys.argv[2] if len(sys.argv) > 2 else None
    with open(iso, "rb") as f:
        base = find_partition(f)
        f.seek(base + 32 * SECTOR + 20)
        root, rsize = struct.unpack("<II", f.read(8))
        files = sorted(walk(f, base, root, rsize))
        total = 0
        for name, pos, length in files:
            total += length
            if not out:
                print(f"{length:>12}  {name}")
                continue
            dst = os.path.join(out, name.lstrip("/"))
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            f.seek(pos)
            with open(dst, "wb") as o:
                left = length
                while left:
                    chunk = f.read(min(left, 1 << 24))
                    assert chunk, f"short read in {name}"
                    o.write(chunk)
                    left -= len(chunk)
        print(f"{len(files)} files, {total / 2**20:.0f} MiB, partition 0x{base:X}")


if __name__ == "__main__":
    main()

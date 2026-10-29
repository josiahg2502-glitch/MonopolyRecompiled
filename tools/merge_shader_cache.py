"""Merge ReXGlue/Xenia shader and pipeline storage from play sessions into the shipped cache.

usage: merge_shader_cache.py <dest cache root> <source cache root> [more sources...]
Cache roots contain shaders/shareable/<title>.xsh and <title>.<mode>.<api>.xpso. Records are
deduplicated by their stored hash; the destination's records come first.
"""
import os, struct, sys

SHAREABLE = os.path.join("shaders", "shareable")
XSH_HEADER, XPSO_HEADER, XPSO_RECORD = 8, 12, 72


def read_xsh(path):
    data = open(path, "rb").read()
    header, records, pos = data[:XSH_HEADER], [], XSH_HEADER
    while pos + 12 <= len(data):
        dwords = struct.unpack_from("<I", data, pos + 8)[0] & 0x7FFFFFFF
        end = pos + 12 + dwords * 4
        if end > len(data):
            break  # torn write at the end of a killed session
        records.append(data[pos:end])
        pos = end
    return header, records


def read_xpso(path):
    data = open(path, "rb").read()
    header = data[:XPSO_HEADER]
    count = (len(data) - XPSO_HEADER) // XPSO_RECORD
    return header, [data[XPSO_HEADER + i * XPSO_RECORD:XPSO_HEADER + (i + 1) * XPSO_RECORD] for i in range(count)]


def merge(dest_root, sources):
    names = set()
    for root in [dest_root] + sources:
        folder = os.path.join(root, SHAREABLE)
        if os.path.isdir(folder):
            names.update(n for n in os.listdir(folder) if n.endswith((".xsh", ".xpso")))
    for name in sorted(names):
        reader = read_xsh if name.endswith(".xsh") else read_xpso
        header, seen, out = None, set(), []
        for root in [dest_root] + sources:
            path = os.path.join(root, SHAREABLE, name)
            if not os.path.isfile(path):
                continue
            h, records = reader(path)
            if header is None:
                header = h
            elif h != header:
                print(f"  skip {path}: different storage version/API")
                continue
            for r in records:
                if r[:8] not in seen:  # u64 ucode/description hash
                    seen.add(r[:8])
                    out.append(r)
        dest = os.path.join(dest_root, SHAREABLE, name)
        os.makedirs(os.path.dirname(dest), exist_ok=True)
        with open(dest, "wb") as f:
            f.write(header + b"".join(out))
        print(f"{name}: {len(out)} records")


def self_check():
    import tempfile
    with tempfile.TemporaryDirectory() as tmp:
        a, b = os.path.join(tmp, "a"), os.path.join(tmp, "b")
        for root, hashes in ((a, [1, 2]), (b, [2, 3])):
            os.makedirs(os.path.join(root, SHAREABLE))
            with open(os.path.join(root, SHAREABLE, "T.xsh"), "wb") as f:
                f.write(b"XESH\0\0\0\1" + b"".join(struct.pack("<QI", h, 1) + b"code" for h in hashes))
            with open(os.path.join(root, SHAREABLE, "T.rtv.d3d12.xpso"), "wb") as f:
                f.write(b"XEPS" * 3 + b"".join(struct.pack("<Q", h) + b"\0" * 64 for h in hashes))
        merge(a, [b])
        assert len(read_xsh(os.path.join(a, SHAREABLE, "T.xsh"))[1]) == 3
        assert len(read_xpso(os.path.join(a, SHAREABLE, "T.rtv.d3d12.xpso"))[1]) == 3


if __name__ == "__main__":
    if len(sys.argv) == 2 and sys.argv[1] == "--self-check":
        self_check()
        print("self-check passed")
    elif len(sys.argv) < 3:
        sys.exit(__doc__)
    else:
        merge(sys.argv[1], sys.argv[2:])

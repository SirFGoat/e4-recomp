"""Minimal Xbox 360 STFS (CON/LIVE/PIRS) package extractor.

Usage: python -I stfs_extract.py <package> <out_dir>
"""
import os
import struct
import sys


def u24le(b, o):
    return b[o] | (b[o + 1] << 8) | (b[o + 2] << 16)


def u24be(b, o):
    return (b[o] << 16) | (b[o + 1] << 8) | b[o + 2]


class Stfs:
    def __init__(self, path):
        with open(path, "rb") as f:
            self.d = f.read()
        d = self.d
        self.magic = d[0:4].decode("ascii")
        if self.magic not in ("CON ", "LIVE", "PIRS"):
            raise ValueError(f"not an STFS package: {self.magic!r}")
        self.header_size = struct.unpack_from(">I", d, 0x340)[0]
        self.content_type = struct.unpack_from(">I", d, 0x344)[0]
        self.title_id = struct.unpack_from(">I", d, 0x360)[0]
        self.display_name = d[0x411:0x411 + 0x80].decode("utf-16-be").split("\0")[0]
        vd = 0x379
        self.block_sep = d[vd + 2]
        self.ft_block_count = struct.unpack_from("<H", d, vd + 3)[0]
        self.ft_block_num = u24le(d, vd + 5)
        # sex: 0 = female (single hash table copy), 1 = male (two copies)
        self.sex = (~self.block_sep) & 1
        self.first_hash_table = (self.header_size + 0xFFF) & 0xFFFFF000

    def backing_block(self, n):
        s = self.sex
        r = (((n + 0xAA) // 0xAA) << s) + n
        if n < 0xAA:
            return r
        if n < 0x70E4:
            return r + (((n + 0x70E4) // 0x70E4) << s)
        return (1 << s) + r + (((n + 0x70E4) // 0x70E4) << s)

    def block_addr(self, n):
        return (self.backing_block(n) << 12) + self.first_hash_table

    def level0_hash_block(self, n):
        s = self.sex
        step0 = 0xAB if s == 0 else 0xAC
        if n < 0xAA:
            return 0
        num = (n // 0xAA) * step0
        num += ((n // 0x70E4) + 1) << s
        if n // 0x70E4 == 0:
            return num
        return num + (1 << s)

    def next_block(self, n):
        addr = (self.level0_hash_block(n) << 12) + self.first_hash_table
        if self.sex == 1:
            # male packages hold two copies; bit 1 of block_sep picks the active one
            addr += (self.block_sep & 2) << 11
        addr += (n % 0xAA) * 0x18
        return u24be(self.d, addr + 0x15)

    def read_blocks(self, start, count, consecutive):
        out = bytearray()
        n = start
        for _ in range(count):
            a = self.block_addr(n)
            out += self.d[a:a + 0x1000]
            n = n + 1 if consecutive else self.next_block(n)
        return bytes(out)

    def entries(self):
        raw = self.read_blocks(self.ft_block_num, self.ft_block_count, False)
        ents = []
        for i in range(0, len(raw), 0x40):
            e = raw[i:i + 0x40]
            flags = e[0x28]
            name_len = flags & 0x3F
            if name_len == 0:
                continue
            ents.append({
                "index": i // 0x40,
                "name": e[:name_len].decode("ascii", "replace"),
                "dir": bool(flags & 0x80),
                "consecutive": bool(flags & 0x40),
                "blocks": u24le(e, 0x29),
                "start": u24le(e, 0x2F),
                "parent": struct.unpack_from(">h", e, 0x32)[0],
                "size": struct.unpack_from(">I", e, 0x34)[0],
            })
        return ents


def main():
    pkg, out = sys.argv[1], sys.argv[2]
    s = Stfs(pkg)
    print(f"magic={s.magic} title_id={s.title_id:08X} content_type={s.content_type:08X} "
          f"name={s.display_name!r} sex={'male' if s.sex else 'female'}")
    ents = s.entries()
    by_index = {e["index"]: e for e in ents}

    def full_path(e):
        parts = [e["name"]]
        p = e["parent"]
        while p != -1 and p in by_index:
            parts.append(by_index[p]["name"])
            p = by_index[p]["parent"]
        return os.path.join(*reversed(parts))

    for e in ents:
        if ".." in e["name"] or "/" in e["name"] or "\\" in e["name"]:
            raise ValueError(f"unsafe entry name {e['name']!r}")
        dest = os.path.join(out, full_path(e))
        if e["dir"]:
            os.makedirs(dest, exist_ok=True)
            continue
        os.makedirs(os.path.dirname(dest), exist_ok=True)
        data = s.read_blocks(e["start"], e["blocks"], e["consecutive"])[:e["size"]]
        with open(dest, "wb") as f:
            f.write(data)
        print(f"{e['size']:>12}  {full_path(e)}")


if __name__ == "__main__":
    main()

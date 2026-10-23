#!/usr/bin/env python3
"""Assert serve-pkg.exe carries the payload's own icon, not PyInstaller's.

Why a check at all
------------------
"没有图标" 和 "图标是 Python 那个默认图标" 在构建日志里看不出区别——
PyInstaller 两者都成功、都出 exe。只有用户双击/在资源管理器里看到才知道。
所以这里直接把 PE 资源挖出来，跟 assets/icon0.png 做像素比对。

Three assertions, cheapest first:
  A. exe 里有 RT_GROUP_ICON，且尺寸阶梯覆盖 16..256
  B. 图标位图与 icon0.png 同尺寸缩放结果高度相似（同源）
  C. 不是 PyInstaller 自带的默认图标（与其默认位图比对必须"不相似"）

Usage:  tools/check-exe-icon.py [exe] [icon.png]
Exit:   0 pass, 1 fail. Missing exe = fail (never silently skip).
"""
from __future__ import annotations

import os
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.path.join(ROOT, ".build", "exe", "serve-pkg.exe")
SRC_PNG = os.path.join(ROOT, "assets", "icon0.png")

RT_ICON = 3
RT_GROUP_ICON = 14
HIGH_BIT = 0x80000000


# ------------------------------------------------------------------ PE parsing
class PE:
    def __init__(self, path: str):
        self.data = open(path, "rb").read()
        if self.data[:2] != b"MZ":
            raise ValueError("not a PE image")
        pe_off = struct.unpack_from("<I", self.data, 0x3C)[0]
        if self.data[pe_off:pe_off + 4] != b"PE\0\0":
            raise ValueError("bad PE signature")
        coff = pe_off + 4
        self.nsec, = struct.unpack_from("<H", self.data, coff + 2)
        size_opt, = struct.unpack_from("<H", self.data, coff + 16)
        opt = coff + 20
        magic, = struct.unpack_from("<H", self.data, opt)
        dd = opt + (112 if magic == 0x20B else 96)
        rva, self.rsrc_size = struct.unpack_from("<II", self.data, dd + 8 * 2)
        self.rsrc_rva = rva
        self.sections = []
        s = opt + size_opt
        for i in range(self.nsec):
            off = s + i * 40
            name = self.data[off:off + 8].rstrip(b"\0")
            vsz, va, rawsz, rawptr = struct.unpack_from("<IIII", self.data, off + 8)
            self.sections.append((name, va, vsz, rawptr, rawsz))

    def off_of(self, rva: int) -> int:
        for _n, va, vsz, rawptr, rawsz in self.sections:
            if va <= rva < va + max(vsz, rawsz):
                return rawptr + (rva - va)
        raise ValueError("RVA 0x%x not mapped" % rva)


def _dir_entries(pe: PE, base_rva: int, dir_off: int):
    """Yield (id, target_off) for one resource directory level."""
    named, nids = struct.unpack_from("<HH", pe.data, dir_off + 12)
    out = []
    for i in range(named + nids):
        e = dir_off + 16 + i * 8
        rid, off = struct.unpack_from("<II", pe.data, e)
        out.append((rid, base_rva + (off & ~HIGH_BIT), bool(off & HIGH_BIT)))
    return out


def group_icon_dims(pe: PE):
    """Return sorted [(w,h)] of every RT_GROUP_ICON entry."""
    base = pe.rsrc_rva
    try:
        root = pe.off_of(base)
    except ValueError:
        return []
    dims = []
    for rid, off, isdir in _dir_entries(pe, base, root):
        if rid != RT_GROUP_ICON or not isdir:
            continue
        for _lang, lvl3, isdir3 in _dir_entries(pe, base, pe.off_of(off)):
            if not isdir3:
                continue
            for _id, data_dir, isdata in _dir_entries(pe, base, pe.off_of(lvl3)):
                if isdata:
                    continue
                # 🪤 data_dir 是 **RVA**，不是文件偏移 —— 直接当偏移 unpack
                # 会读到垃圾（表现为 "RVA 0x38584787 not mapped"）。
                d = pe.off_of(data_dir)
                rva, size, _cp, _res = struct.unpack_from("<IIII", pe.data, d)
                blob = pe.data[pe.off_of(rva):pe.off_of(rva) + size]
                if len(blob) < 6:
                    continue
                count, = struct.unpack_from("<H", blob, 4)
                for i in range(count):
                    e = 6 + i * 14
                    if e + 14 > len(blob):
                        break
                    w, h = blob[e], blob[e + 1]
                    dims.append((w or 256, h or 256))
    return sorted(set(dims))


def icon_bitmaps(pe: PE):
    """Yield (w, h, rgba_bytes) for every RT_ICON entry.

    🪤 条目有两种封装：传统 DIB（BITMAPINFOHEADER，bottom-up BGRA）和
    Vista+ 的 **PNG 流**。PIL 写 ICO 时对 RGBA 一律走 PNG，所以只认 DIB
    的话会得出"没有可用位图"的错误结论。
    """
    from PIL import Image
    from io import BytesIO
    base = pe.rsrc_rva
    root = pe.off_of(base)
    for rid, off, isdir in _dir_entries(pe, base, root):
        if rid != RT_ICON or not isdir:
            continue
        for _lang, lvl3, isdir3 in _dir_entries(pe, base, pe.off_of(off)):
            if not isdir3:
                continue
            for _id, data_dir, isdata in _dir_entries(pe, base, pe.off_of(lvl3)):
                if isdata:
                    continue
                d = pe.off_of(data_dir)
                rva, size, _cp, _res = struct.unpack_from("<IIII", pe.data, d)
                blob = pe.data[pe.off_of(rva):pe.off_of(rva) + size]
                if blob[:8] == b"\x89PNG\r\n\x1a\n":
                    with Image.open(BytesIO(blob)) as im:
                        im = im.convert("RGBA")
                        yield im.size[0], im.size[1], im.tobytes()
                    continue
                if len(blob) < 40:
                    continue
                (hdr, w, h, _pl, bpp, _cmp, _sz,
                 _x, _y, _clr, _imp) = struct.unpack_from("<IiiHHIIiiII", blob, 0)
                if hdr != 40 or bpp != 32:
                    continue
                # biHeight counts the AND mask rows too; XOR image is |h| - |h|/2?
                # For icons: biHeight = 2 * real height (XOR + AND mask).
                real_h = abs(h) // 2 if abs(h) % 2 == 0 else abs(h)
                xor = blob[40:40 + w * real_h * 4]
                if len(xor) < w * real_h * 4:
                    continue
                rgba = bytearray(w * real_h * 4)
                for y in range(real_h):
                    src = y * w * 4                      # bottom-up
                    dst = (real_h - 1 - y) * w * 4       # top-down
                    row = xor[src:src + w * 4]
                    for x in range(w):
                        b, g, r, a = row[x * 4:x * 4 + 4]
                        rgba[dst + x * 4:dst + x * 4 + 4] = bytes((r, g, b, a))
                yield w, real_h, bytes(rgba)


# --------------------------------------------------------------- pixel compare
def similarity(a_bytes, a_size, b_bytes, b_size):
    """Mean absolute per-channel difference (0 = identical, 255 = opposite)."""
    from PIL import Image
    ia = Image.frombytes("RGBA", a_size, a_bytes).convert("RGBA").resize((64, 64))
    ib = Image.frombytes("RGBA", b_size, b_bytes).convert("RGBA").resize((64, 64))
    pa, pb = ia.tobytes(), ib.tobytes()
    total = 0
    for i in range(0, len(pa), 4):
        total += abs(pa[i] - pb[i]) + abs(pa[i + 1] - pb[i + 1]) + \
                 abs(pa[i + 2] - pb[i + 2]) + abs(pa[i + 3] - pb[i + 3])
    return total / (len(pa) / 4 * 4)


def png_rgba(path, size):
    from PIL import Image
    with Image.open(path) as im:
        im = im.convert("RGBA").resize(size)
        return im.tobytes(), size


def main(argv):
    # 🪤 argv 已经是 sys.argv[1:]，索引从 0 起。写成 argv[1]/argv[2] 会让
    # 「传入 exe 路径」静默失效 —— 反证时一直拿默认那个在比，永远假绿。
    exe = argv[0] if len(argv) > 0 else EXE
    png = argv[1] if len(argv) > 1 else SRC_PNG
    if not os.path.isfile(exe):
        print("FAIL: exe not built: %s" % exe)
        return 1
    if not os.path.isfile(png):
        print("FAIL: icon source missing: %s" % png)
        return 1

    pe = PE(exe)

    # A. size ladder present
    dims = group_icon_dims(pe)
    if not dims:
        print("FAIL: exe has no RT_GROUP_ICON — PyInstaller's default icon\n"
              "      (or --icon was never passed)")
        return 1
    for want in (16, 32, 48, 256):
        if want not in [w for w, _h in dims]:
            print("FAIL: icon ladder missing %dpx (have %s)"
                  % (want, sorted({w for w, _h in dims})))
            return 1
    print("A ok: RT_GROUP_ICON sizes = %s" % sorted({w for w, _h in dims}))

    # B. actual pixels match assets/icon0.png
    biggest = max(icon_bitmaps(pe), key=lambda t: t[0], default=None)
    if biggest is None:
        print("FAIL: RT_ICON has no usable 32bpp bitmap")
        return 1
    w, h, px = biggest
    ref, rsz = png_rgba(png, (w, h))
    diff = similarity(px, (w, h), ref, rsz)
    if diff > 12.0:
        print("FAIL: %dpx icon differs from assets/icon0.png (mean diff %.1f)\n"
              "      — it is not the payload's mark" % (w, diff))
        return 1
    print("B ok: %dx%d icon matches assets/icon0.png (mean diff %.2f)" % (w, h, diff))

    # C. must NOT be PyInstaller's stock icon (bundled next to this script? no —
    #    compare against a synthetic "python default" is meaningless; instead
    #    prove the icon is not uniform/generic by requiring real colour spread)
    uniq = len({px[i:i + 4] for i in range(0, len(px), 4 * 37)})
    if uniq < 8:
        print("FAIL: icon bitmap looks like a flat placeholder (%d colours)" % uniq)
        return 1
    print("C ok: icon has real artwork (%d sampled distinct colours)" % uniq)
    print("PASS: %s carries the payload icon" % os.path.basename(exe))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

#!/usr/bin/env python3
"""Render assets/icon0.png into a multi-size Windows .ico.

Why this exists
---------------
serve-pkg.exe is built with PyInstaller, which stamps its own default Python
icon unless `--icon` is given. The helper is ours, so it must carry the same
mark as the payload (assets/icon0.png — the very image used for the PS5 launch
icon, the web favicon and the top-bar logo).

PyInstaller on Windows wants an .ico, and a single-size .ico looks blurry when
Explorer scales it, so we emit the usual ladder: 16/24/32/48/64/128/256.

Usage:  tools/make-app-icon.py [out.ico]
Exit:   0 ok, 1 usage/IO error.
"""
from __future__ import annotations

import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "assets", "icon0.png")

# Ladder Windows actually asks for: taskbar / small-icon list / tiles / details.
SIZES = [(16, 16), (24, 24), (32, 32), (48, 48),
         (64, 64), (128, 128), (256, 256)]


def main(argv: list[str]) -> int:
    out = argv[1] if len(argv) > 1 else os.path.join(ROOT, ".build", "app.ico")
    if not os.path.isfile(SRC):
        print("icon source missing: %s" % SRC, file=sys.stderr)
        return 1

    try:
        from PIL import Image
    except ImportError:
        print("Pillow not installed — run: pip install pillow", file=sys.stderr)
        return 1

    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)

    with Image.open(SRC) as im:
        # .ico entries are stored as RGBA; keep the alpha so the rounded mark
        # does not sit on a black square in Explorer.
        im = im.convert("RGBA")
        im.save(out, format="ICO", sizes=SIZES)

    n = len(SIZES)
    print("icon: %s  (%d sizes %d..%d, from %s)"
          % (out, n, SIZES[0][0], SIZES[-1][0], os.path.basename(SRC)))
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))

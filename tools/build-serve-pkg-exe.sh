#!/usr/bin/env bash
# Build serve-pkg.exe — the Windows helper the payload offers as a download.
#
# ⚠️ NEW (Plan A / 2026-10-03): the .exe is NO LONGER embedded in the ELF — it
# would bloat the payload to ~10.8 MB for no runtime gain. It ships via the
# GitHub Release (assets/index.html links to it as an external download). Only
# serve-pkg.py is embedded now (see src/embed_serve_pkg.c, which .incbin's it).
#
# WHY THIS IS NOT PART OF `make`
# PyInstaller needs pip, a network (first run) and ~4 minutes. None of that
# belongs in a payload build, and the artefact is a generated binary that would
# otherwise have to be committed. So the exe is produced here, on demand, and
# the Makefile picks it up if it is present.
#
# The exe bundles a CPython runtime, so the user needs nothing installed: they
# double-click it and it serves the current directory over HTTP with Range
# support, which is what sceAppInstUtilInstallByPackage expects from a PC.
#
# Usage:  tools/build-serve-pkg-exe.sh [--no-embed]
#         --no-embed   build only; do not copy into assets/
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/.." && pwd)"
out="$root/.build/exe"
venv_python="${WFM_VENV_PYTHON:-}"

if [ -z "$venv_python" ]; then
  for cand in \
    "$HOME/.workbuddy/binaries/python/envs/default/Scripts/python.exe" \
    "$HOME/.workbuddy/binaries/python/envs/default/bin/python" \
    python3 python; do
    if command -v "$cand" >/dev/null 2>&1; then venv_python="$cand"; break; fi
    if [ -x "$cand" ]; then venv_python="$cand"; break; fi
  done
fi

if [ -z "$venv_python" ]; then
  echo "no python interpreter found; set WFM_VENV_PYTHON=/path/to/python" >&2
  exit 1
fi

echo "== interpreter: $venv_python"
"$venv_python" -m pip install --quiet --disable-pip-version-check pyinstaller

rm -rf "$out"
mkdir -p "$out"
cp "$root/tools/serve-pkg.py" "$out/serve-pkg.py"

# --onefile: a single .exe the user can double-click. No UPX: its compression
# trips Windows Defender's heuristics on an unsigned binary.
"$venv_python" -m PyInstaller --onefile --clean --noconfirm \
  --name serve-pkg --distpath "$out" --workpath "$out/build" \
  --specpath "$out" "$out/serve-pkg.py" >/dev/null

ls -l "$out/serve-pkg.exe"

if [ "${1:-}" != "--no-embed" ]; then
  # ⚠️ assets/ 下这份 .py 是 **ELF 内嵌、给 PS5 端当下载链接发出去**的那一份
  # （Makefile 收的是 assets/*，不是 tools/*）。不同步就会出现「exe 是新的、
  # 下载到的 py 还是旧版本」——用户拿到手的脚本里没有本轮的修复。
  # 两份必须永远同源，所以在这里一起 copy。
  cp "$root/tools/serve-pkg.py" "$root/assets/serve-pkg.py"
  echo "embedded: assets/serve-pkg.py  ($(stat -c%s "$root/assets/serve-pkg.py") bytes)"
  echo "rebuild the payload:  make all   (or .build/rebuild-elf.sh)"
  echo
  echo "⚠️ serve-pkg.exe 不再内嵌进 ELF（9.2MB 的 PyInstaller 整包 CPython 会让"
  echo "   payload 膨胀到 10.8MB）。它改由 GitHub Release 分发：本地保留"
  echo "   $out/serve-pkg.exe，发布时上传到同版本 tag："
  echo "   gh release upload \"$VERSION_TAG\" \"$out/serve-pkg.exe#serve-pkg.exe\""
  echo "   （VERSION_TAG 取 Makefile 里的 $(grep -m1 VERSION_TAG= \"$root/Makefile\" | cut -d= -f2)）"
fi

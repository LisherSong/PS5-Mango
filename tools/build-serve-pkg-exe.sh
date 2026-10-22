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
"$venv_python" -m pip install --quiet --disable-pip-version-check pyinstaller pillow

rm -rf "$out"
mkdir -p "$out"
cp "$root/tools/serve-pkg.py" "$out/serve-pkg.py"

# ⚠️ 图标：不指定 --icon 的话 PyInstaller 会盖它自己的 Python 默认图标，
# 用户看到的就是「一个 python 图标的 exe」，跟插件完全对不上号。
# 这里拿 assets/icon0.png（PS5 启动图标 / 网页 favicon / 顶栏 logo 同一份）
# 渲染成多尺寸 .ico，保证 helper 和 payload 是同一个标志。
icon="$out/serve-pkg.ico"
"$venv_python" "$root/tools/make-app-icon.py" "$icon" || {
  echo "icon render failed; continuing without a custom icon" >&2
  icon=""
}
[ -n "$icon" ] && icon_args=(--icon "$icon") || icon_args=()
# bash <4.4 + `set -u` 会把空数组展开当成未绑定 ⇒ 用 + 形式展开。

# --onefile: a single .exe the user can double-click. No UPX: its compression
# trips Windows Defender's heuristics on an unsigned binary.
"$venv_python" -m PyInstaller --onefile --clean --noconfirm \
  --name serve-pkg --distpath "$out" --workpath "$out/build" \
  --specpath "$out" ${icon_args[@]+"${icon_args[@]}"} "$out/serve-pkg.py" >/dev/null

ls -l "$out/serve-pkg.exe"

# ⚠️ 「构建成功」≠「图标对」。PyInstaller 漏掉 --icon 时同样 exit 0、同样出
# exe，只有在资源管理器里才看得出是系统默认图标。所以这里挖 PE 资源跟
# assets/icon0.png 做像素比对 —— 图标不对就让构建失败。
"$venv_python" "$root/tools/check-exe-icon.py" "$out/serve-pkg.exe" \
  "$root/assets/icon0.png" || {
  echo "FATAL: serve-pkg.exe is not carrying the payload icon" >&2
  exit 1
}
echo

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
  # 🪤 `set -u`：$VERSION_TAG 从来没被赋值过，直接用会在最后一行炸掉整脚本
  # （exe 其实已经生成好了，但 rc≠0 ⇒ 自动化会当成构建失败）。从 Makefile 读。
  vt="$(grep -m1 '^VERSION_TAG' "$root/Makefile" | cut -d= -f2 | tr -d ' ')"
  echo "   gh release upload \"$vt\" \"$out/serve-pkg.exe#serve-pkg.exe\""
  echo "   （VERSION_TAG 取 Makefile 里的 $vt）"
fi

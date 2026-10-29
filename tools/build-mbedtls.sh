#!/usr/bin/env bash
# build-mbedtls.sh —— 构建 mbedTLS 3.6.7 静态库并装进 third_party/mbedtls
#
# 配方来源：GitHub 参考项目 cobanov/PS5LM（scripts/build-mbedtls.sh），
# 其 mbedTLS 版 HTTPS 客户端已在 PS5 真机发版验证（payload 内经 mbedTLS
# 从 CDN 下载 2.6GB 模型）。两个线程开关是 TLS 1.3 的 PSA 全局密钥存储
# 线程安全要求（mbedtls docs/architecture/psa-thread-safety；pacbrew 的
# PS5 包同样开启）。
#
# 用法（WSL 内）：
#   # PS5 交叉构建（默认）：
#   wsl -d Ubuntu-22.04 -- bash -lc 'cd /path/to/PS5-Mango && tools/build-mbedtls.sh'
#   # 宿主机（glibc）构建，产物 third_party/mbedtls/lib-host：
#   wsl -d Ubuntu-22.04 -- bash -lc 'cd /path/to/PS5-Mango && MBEDTLS_HOST=1 tools/build-mbedtls.sh'
#
# 环境变量：
#   PS5_PAYLOAD_SDK  SDK 路径（默认 /opt/ps5-payload-sdk）
#   MBEDTLS_DEPS    源码与构建暂存目录（默认 /tmp/mbedtls-deps，可指向别处复用）
#   MBEDTLS_HOST    设 1 则构建宿主机（Linux glibc）版 -> third_party/mbedtls/lib-host
#   MBEDTLS_WIN_TEMP Windows 侧 %TEMP% 的 WSL 路径（可选；用于复用浏览器下载的 tarball）
#
# 产物：third_party/mbedtls/{lib,include}（PS5 静态库 + 头文件，进 git）；
#       MBEDTLS_HOST=1 时 third_party/mbedtls/lib-host/（宿主机静态库，进 git）。
#       third_party/mbedtls/VERSION-BUILD.txt（版本/sha256/开关记录）。
#
# ⚠️ 为什么宿主机也要一份：prospero-clang 产物引用 newlib-ism 符号
# （__error / __isthreaded / __inet_pton / sysctl），宿主 glibc 链接
# 无法用 defsym 全部消化；宿主侧 __linux__ 分支走 getrandom，符号干净。
# 主 Makefile 的 LINUX_LDADD 用 lib-host，PS5 的 LDADD 用 lib。

set -euo pipefail

VERSION=3.6.7
SHA256=a7e8bcbec0e6f761b4af24f25677626b35f762f68eef79c08677a363212d11f6

PS5_PAYLOAD_SDK="${PS5_PAYLOAD_SDK:-/opt/ps5-payload-sdk}"
DEPS="${MBEDTLS_DEPS:-/tmp/mbedtls-deps}"
HOST_BUILD="${MBEDTLS_HOST:-0}"
ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="$ROOT/third_party/mbedtls"

tarball="$DEPS/mbedtls-$VERSION.tar.bz2"
src="$DEPS/mbedtls-$VERSION"
build="$DEPS/build-ps5"
stage="$DEPS/stage-ps5"

if [ "$HOST_BUILD" = "1" ]; then
    build="$DEPS/build-host"
    stage="$DEPS/stage-host"
fi

mkdir -p "$DEPS"

if [ ! -f "$tarball" ]; then
    # 快通路：复用 Windows 侧已下载并校验过的 tarball（WSL /tmp 不跨调用
    # 存活，每次重下要挨代理限速 ~5 分钟）。用户名不写死：
    # 设 MBEDTLS_WIN_TEMP=/mnt/c/Users/<you>/AppData/Local/Temp 即可命中。
    for cand in ${MBEDTLS_WIN_TEMP:+"$MBEDTLS_WIN_TEMP/mbedtls-$VERSION.tar.bz2"} \
                "$HOME/AppData/Local/Temp/mbedtls-$VERSION.tar.bz2"; do
        [ -f "$cand" ] && cp "$cand" "$tarball" && break
    done
fi

if [ ! -f "$tarball" ]; then
    echo "build-mbedtls: downloading mbedTLS $VERSION ..."
    # 沙箱代理对 github.com 限速时，断点续传重试
    for i in $(seq 1 12); do
        curl -fsSL -C - --connect-timeout 15 --max-time 100 -o "$tarball" \
            "https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-$VERSION/mbedtls-$VERSION.tar.bz2" \
            && break || echo "build-mbedtls: attempt $i incomplete, resuming..."
        sleep 2
    done
fi

actual=$(sha256sum "$tarball" | cut -d' ' -f1)
[ "$actual" = "$SHA256" ] || {
    echo "build-mbedtls: checksum mismatch ($actual)" >&2
    exit 1
}

rm -rf "$src" "$build" "$stage"
tar xjf "$tarball" -C "$DEPS"

# 线程安全开关（TLS 1.3 PSA 全局密钥存储）
python3 "$src/scripts/config.py" -f "$src/include/mbedtls/mbedtls_config.h" set MBEDTLS_THREADING_C
python3 "$src/scripts/config.py" -f "$src/include/mbedtls/mbedtls_config.h" set MBEDTLS_THREADING_PTHREAD

if [ "$HOST_BUILD" = "1" ]; then
    cmake -S "$src" -B "$build" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$stage" \
        -DENABLE_PROGRAMS=OFF \
        -DENABLE_TESTING=OFF \
        -DUSE_SHARED_MBEDTLS_LIBRARY=OFF \
        -DUSE_STATIC_MBEDTLS_LIBRARY=ON \
        -DMBEDTLS_FATAL_WARNINGS=OFF
    cmake --build "$build"
    cmake --install "$build" > /dev/null

    rm -rf "$OUT/lib-host"
    mkdir -p "$OUT/lib-host"
    cp "$stage/lib/"*.a "$OUT/lib-host/"
    # include 与 PS5 版共用（同版本同开关）；仅首次缺失时补
    if [ ! -d "$OUT/include" ]; then
        cp -r "$stage/include" "$OUT/"
    fi
    echo "build-mbedtls: done (host) -> $OUT/lib-host"
    ls -l "$OUT/lib-host"
    exit 0
fi

[ -x "$PS5_PAYLOAD_SDK/bin/prospero-cmake" ] || {
    echo "build-mbedtls: prospero-cmake not found in $PS5_PAYLOAD_SDK" >&2
    exit 1
}

"$PS5_PAYLOAD_SDK/bin/prospero-cmake" -S "$src" -B "$build" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$stage" \
    -DENABLE_PROGRAMS=OFF \
    -DENABLE_TESTING=OFF \
    -DUSE_SHARED_MBEDTLS_LIBRARY=OFF \
    -DUSE_STATIC_MBEDTLS_LIBRARY=ON \
    -DMBEDTLS_FATAL_WARNINGS=OFF

cmake --build "$build"
cmake --install "$build" > /dev/null

rm -rf "$OUT"
mkdir -p "$OUT"
cp -r "$stage/lib" "$stage/include" "$OUT/"

{
    echo "mbedTLS $VERSION (PS5, prospero-cmake + Ninja, static)"
    echo "source sha256 : $SHA256"
    echo "config flags  : MBEDTLS_THREADING_C, MBEDTLS_THREADING_PTHREAD"
    echo "built         : $(date '+%Y-%m-%d %H:%M:%S %z')"
    echo "recipe        : cobanov/PS5LM scripts/build-mbedtls.sh (GitHub reference)"
    echo "libs          :"
    ls -l "$OUT/lib" | tail -n +2 | awk '{printf "  %s  %s bytes\n", $9, $5}'
    echo "lib-host      : MBEDTLS_HOST=1 构建的宿主机（glibc）静态库，主 Makefile"
    echo "               LINUX_LDADD 使用；PS5 产物含 newlib-ism 符号（__error 等）"
    echo "               无法在宿主链接。"
} > "$OUT/VERSION-BUILD.txt"

echo "build-mbedtls: done -> $OUT"
ls -l "$OUT/lib"
cat "$OUT/VERSION-BUILD.txt"

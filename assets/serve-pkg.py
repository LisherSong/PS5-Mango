#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""serve-pkg.py — PS5 Nexus 的 PC 端 PKG 文件服务器（单文件、零依赖）。

在 PS5 的「PKG管理 → 从 PC 安装」里填本机 IP:端口即可列出卡片并安装；
系统安装器直接从 PC 分段下载，PKG 全程不落 PS5 硬盘。

为什么不用 `python -m http.server`：Sony 下载器发 Range 请求（206 Partial
Content），标准库那套忽略 Range → PS5 中止安装（0x80B211C8）。本脚本实现了
Range/HEAD，并额外提供 /catalog（JSON 列表，带 CORS 头，供 PS5 浏览器跨域
fetch）。

用法（放在装着 .pkg 的文件夹里**就地**运行）：
    python serve-pkg.py [端口]        # 默认 9898
启动后会自动打印本机局域网 IP，在 PS5 Nexus 网页里填它即可。

取自 Loopayeh/pkg-sender 的 serve_pkg.py（MIT License），按本项目需要加了
/catalog、CORS 与中文文件名处理。
"""
import json
import os
import socket
import sys
import threading
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

# 中文 Windows 控制台默认 GBK(cp936)：中文能打，但任何无 GBK 映射的字符
# （emoji、某些符号）会直接抛 UnicodeEncodeError 把进程打死。这里把 stdout/stderr
# 的错误策略改成 replace —— 保住中文正常显示（编码不动），遇到打不出的字符退化成
# "?" 而不是崩溃。3.7+ 才有 reconfigure；更老的解释器没有也不会更糟。
for _s in (sys.stdout, sys.stderr):
    try:
        _s.reconfigure(errors="replace")
    except Exception:
        pass

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 9898
CHUNK = 1024 * 1024
PKG_EXTS = (".pkg", ".ffpkg", ".ffpfsc", ".exfat")


def lan_ip():
    """本机在局域网里的 IP：起一个 UDP socket「连」一个外部地址（不真正发包，
    只让内核选路由），读回自己这侧的地址。比枚举网卡可靠——它回答的正是
    「PS5 要填哪个 IP」这个问题本身。失败（无网络/受限沙箱）就退回枚举主机名。"""
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            s.connect(("192.168.255.255", 9))   # 不发包，仅选路由
            return s.getsockname()[0]
        finally:
            s.close()
    except OSError:
        try:
            return socket.gethostbyname(socket.gethostname())
        except OSError:
            return "127.0.0.1"


# ---------------------------------------------------------------------------
# 从 PKG 容器里抽 icon0.png（PC 卡片要显示游戏封面 —— PS5 端读不到 PC 的文件，
# 所以由 PC 自己抽好、当普通图片发给浏览器；PS5 端一行 C 网络代码都不用加）。
#
# 格式与 PS5 端 src/pkg_info.c 的 pkg_source_open() 完全一致（那份已在真机
# PS4/PS5 PKG 上验证过）：0xa0 头 → 判容器起始 → 0x20 一条的条目表 → 找
# id 0x1200..0x121f、未加密、且带 PNG 签名的条目。这里只做「读偏移 + 切片」，
# 不解析 SFO（封面用不着）。抽不出来返回 None ⇒ 卡片退回字母兜底，不报错。
# ---------------------------------------------------------------------------
PKG_CNT_MAGIC = 0x7F434E54
PKG_FIH_MAGIC = 0x7F464948
PKG_LIH_MAGIC = 0x7F4C4948
PKG_HEADER_SIZE = 0xA0
PKG_ENTRY_SIZE = 0x20
PKG_ENTRY_ICON0 = 0x1200
PKG_ENTRY_ICON0_LAST = 0x121F
PKG_ENTRY_FLAG_ENCRYPTED = 0x80000000
PKG_ENTRY_MAX = 65536
PKG_ICON_MAX = 32 * 1024 * 1024
PNG_SIG = b"\x89PNG\r\n\x1a\n"

_icon_cache = {}                 # (name, size, mtime) -> (offset, length) | None
_icon_lock = threading.Lock()


def _be32(b, off):
    return int.from_bytes(b[off:off + 4], "big")


def pkg_icon_span(path):
    """该 PKG 内部 icon0.png 的 (绝对偏移, 长度)；找不到返回 None。"""
    try:
        fd = open(path, "rb")
    except OSError:
        return None
    try:
        total = os.fstat(fd.fileno()).st_size
        if total < PKG_HEADER_SIZE:
            return None
        header = fd.read(PKG_HEADER_SIZE)
        if len(header) < PKG_HEADER_SIZE:
            return None
        magic = _be32(header, 0)
        if magic == PKG_FIH_MAGIC:
            container = int.from_bytes(header[0x58:0x60], "little")
        elif magic == PKG_LIH_MAGIC:
            container = int.from_bytes(header[0x30:0x38], "little")
        elif magic == PKG_CNT_MAGIC:
            container = 0
        else:
            return None
        if container:
            if container + PKG_HEADER_SIZE > total:
                return None
            fd.seek(container)
            header = fd.read(PKG_HEADER_SIZE)
            if len(header) < PKG_HEADER_SIZE or _be32(header, 0) != PKG_CNT_MAGIC:
                return None
        container_size = total - container

        entry_count = _be32(header, 0x10)
        table_offset = _be32(header, 0x18)
        if not entry_count or entry_count > PKG_ENTRY_MAX:
            return None
        table_size = entry_count * PKG_ENTRY_SIZE
        if table_offset + table_size > container_size:
            return None
        fd.seek(container + table_offset)
        table = fd.read(table_size)
        if len(table) < table_size:
            return None

        fallback = None          # icon0.png 优先；退而取第一个合法的本地化图标
        for i in range(entry_count):
            e = table[i * PKG_ENTRY_SIZE:(i + 1) * PKG_ENTRY_SIZE]
            eid = _be32(e, 0)
            flags1 = _be32(e, 0x08)
            off = _be32(e, 0x10)
            size = _be32(e, 0x14)
            if flags1 & PKG_ENTRY_FLAG_ENCRYPTED or not size:
                continue
            if not (PKG_ENTRY_ICON0 <= eid <= PKG_ENTRY_ICON0_LAST):
                continue
            if size > PKG_ICON_MAX or off + size > container_size:
                continue
            if fallback is not None and eid != PKG_ENTRY_ICON0:
                continue
            fd.seek(container + off)
            if fd.read(len(PNG_SIG)) != PNG_SIG:
                continue
            if eid == PKG_ENTRY_ICON0:
                return (container + off, size)      # icon0.png 命中即定
            fallback = (container + off, size)
        return fallback
    except OSError:
        return None
    finally:
        fd.close()


def icon_span_cached(name):
    """带缓存的 pkg_icon_span：键含 mtime/size，文件一改就重算。"""
    try:
        st = os.stat(name)
    except OSError:
        return None
    key = (name, st.st_size, int(st.st_mtime))
    with _icon_lock:
        if key in _icon_cache:
            return _icon_cache[key]
    span = pkg_icon_span(name)
    with _icon_lock:
        _icon_cache[key] = span
    return span


def _png_dimensions(path, span):
    """icon0.png 的宽高（IHDR 就在 PNG 头之后 16 字节处）。读不到返回 (0, 0)——
    只用来给 <img> 定 aspect，缺了不影响显示。"""
    off, size = span
    if size < 24:
        return (0, 0)
    try:
        with open(path, "rb") as f:
            f.seek(off + 16)
            ihdr = f.read(8)
    except OSError:
        return (0, 0)
    if len(ihdr) < 8:
        return (0, 0)
    return (_be32(ihdr, 0), _be32(ihdr, 4))


class Handler(BaseHTTPRequestHandler):
    server_version = "PS5-Nexus-serve-pkg/1.0"

    def log_message(self, fmt, *args):
        sys.stdout.write("%s - %s\n" % (self.client_address[0], fmt % args))
        sys.stdout.flush()

    # ---- 通用响应（统一带 CORS：PS5 浏览器跨域 fetch catalog/图标） ----
    def _send(self, code, ctype, body, extra=None):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Cache-Control", "no-store")
        for k, v in (extra or {}).items():
            self.send_header(k, v)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        try:
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError):
            pass

    # ---- /catalog：文件列表（PS5 端卡片数据源） ----
    def _catalog(self):
        items = []
        for name in sorted(os.listdir(".")):
            if not name.lower().endswith(PKG_EXTS):
                continue
            try:
                if not os.path.isfile(name):
                    continue
                size = os.path.getsize(name)
            except OSError:
                continue
            item = {
                "id": name,
                "name": os.path.splitext(name)[0],
                "size": size,
                "is_fpkg": name.lower().endswith((".ffpkg", ".ffpfsc", ".exfat")),
                "url": "/pkg/" + urllib.parse.quote(name),
            }
            # 有封面就把地址一并给出去（PS5 端卡片直接 <img src>，不用先探一次）。
            span = icon_span_cached(name)
            if span:
                w, h = _png_dimensions(name, span)
                item["icon"] = "/icon/" + urllib.parse.quote(name)
                if w:
                    item["icon_w"] = w
                    item["icon_h"] = h
            items.append(item)
        body = json.dumps({"ok": True, "items": items}).encode("utf-8")
        self._send(200, "application/json; charset=utf-8", body)

    # ---- /icon/<name>：从 PKG 里抽出来的 icon0.png ----
    def _send_icon(self, name):
        if not name or not os.path.isfile(name):
            return self._send(404, "text/plain; charset=utf-8", b"not found")
        span = icon_span_cached(name)
        if not span:
            # 抽不到不是错误：前端 <img onerror> 会摘掉它，卡片退回字母兜底。
            return self._send(404, "text/plain; charset=utf-8", b"no icon")
        off, size = span
        try:
            with open(name, "rb") as f:
                f.seek(off)
                data = f.read(size)
        except OSError:
            return self._send(500, "text/plain; charset=utf-8", b"read failed")
        self.send_response(200)
        self.send_header("Content-Type", "image/png")
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Access-Control-Allow-Origin", "*")
        # 封面本质不变（文件名相同内容就相同），给浏览器缓存一会儿，省重复读盘。
        self.send_header("Cache-Control", "max-age=3600")
        self.end_headers()
        try:
            self.wfile.write(data)
        except (BrokenPipeError, ConnectionResetError):
            pass

    # ---- PKG 文件：Range/HEAD 都支持 ----
    def _send_file(self, path, start, end, total, partial):
        try:
            f = open(path, "rb")
        except OSError:
            self._send(404, "text/plain; charset=utf-8",
                       b"not found")
            return
        f.seek(start)
        length = end - start + 1
        self.send_response(206 if partial else 200)
        if partial:
            self.send_header("Content-Range",
                             "bytes %d-%d/%d" % (start, end, total))
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(length))
        self.send_header("Accept-Ranges", "bytes")
        self.send_header("Access-Control-Allow-Origin", "*")
        self.end_headers()
        try:
            while length > 0:
                buf = f.read(min(CHUNK, length))
                if not buf:
                    break
                self.wfile.write(buf)
                length -= len(buf)
        except (BrokenPipeError, ConnectionResetError):
            pass
        finally:
            f.close()

    def do_GET(self):
        # Sony 下载器会在 URL 后附加 query（&product=..&serverIpAddr=..），剥掉
        raw = self.path.split("?", 1)[0]
        path = urllib.parse.unquote(raw)
        if path in ("/catalog", "/catalog/"):
            return self._catalog()
        if path.startswith("/icon/"):
            # 同上：只取文件名，不许进子目录 / 路径穿越
            return self._send_icon(os.path.basename(path[len("/icon/"):]))
        if path.startswith("/pkg/"):
            # 只取文件名，不进子目录、不许路径穿越
            name = os.path.basename(path[len("/pkg/"):])
            if not name or not os.path.isfile(name):
                return self._send(404, "text/plain; charset=utf-8",
                                  ("not found: %s" % name).encode("utf-8"))
            total = os.path.getsize(name)
            start, end = 0, total - 1
            partial = False
            rng = self.headers.get("Range")
            if rng and rng.startswith("bytes="):
                try:
                    spec = rng[6:].split("-", 1)
                    start = int(spec[0]) if spec[0] else 0
                    if len(spec) > 1 and spec[1]:
                        end = min(int(spec[1]), total - 1)
                    partial = True
                except ValueError:
                    pass
            if start >= total:
                self.send_error(416, "range past EOF")
                return
            return self._send_file(name, start, end, total, partial)
        self._send(404, "text/plain; charset=utf-8", b"not found")

    def do_HEAD(self):
        raw = self.path.split("?", 1)[0]
        path = urllib.parse.unquote(raw)
        if path.startswith("/pkg/"):
            name = os.path.basename(path[len("/pkg/"):])
            if not name or not os.path.isfile(name):
                self.send_error(404)
                return
            total = os.path.getsize(name)
            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Length", str(total))
            self.send_header("Accept-Ranges", "bytes")
            self.send_header("Access-Control-Allow-Origin", "*")
            self.end_headers()
            return
        self.send_error(404)


if __name__ == "__main__":
    srv = ThreadingHTTPServer(("0.0.0.0", PORT), Handler)
    cwd = os.getcwd()
    pkgs = [x for x in os.listdir(cwd)
            if x.lower().endswith(PKG_EXTS) and os.path.isfile(x)]
    ip = lan_ip()
    print("PS5 Nexus serve-pkg on port %d — %d 个 PKG — 目录: %s"
          % (PORT, len(pkgs), cwd), flush=True)
    if not pkgs:
        print("[!] 当前目录里没有 .pkg —— 请把本程序移到 PKG 所在目录再运行",
              flush=True)
    print("在 PS5 Nexus「PKG管理 → 从 PC 安装」里填: %s:%d" % (ip, PORT),
          flush=True)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass

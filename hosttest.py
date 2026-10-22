#!/usr/bin/env python3
"""Host test for the merged owendswang-backend + MANGO-frontend tree.

Emulates the MANGO front-end exactly:
  * apiJson(path, obj)  -> POST application/json, body = JSON.stringify(obj)
  * apiPost(path, obj)  -> POST application/x-www-form-urlencoded
  * uploadOne()         -> POST /api/upload-file with X-WFM-* headers + raw body
and checks the response shapes the front-end actually consumes.

Run inside WSL:  python3 hosttest.py
"""
import base64
import hashlib
import http.client
import json
import os
import re
import shutil
import signal
import socket
import sqlite3
import subprocess
import sys
import time
import urllib.parse
import zipfile

WORKDIR = os.path.dirname(os.path.abspath(__file__))


def _makefile_version():
    """Read VERSION_TAG out of the Makefile.

    $(LINUX_BIN) is PS5-Mango-linux-$(VERSION_TAG), so any hard-coded name goes
    stale on every version bump. Reading it back also gives the suite a second
    thing to check: that the running binary reports the version this tree claims
    — which is what catches a stale build that silently kept its old -D.
    """
    try:
        with open(os.path.join(WORKDIR, "Makefile"), encoding="utf-8") as f:
            m = re.search(r"^VERSION_TAG\s*:?=\s*(\S+)", f.read(), re.M)
        return m.group(1) if m else None
    except OSError:
        return None


VERSION_TAG = _makefile_version()


def _host_bin():
    name = "PS5-Mango-linux-" + VERSION_TAG if VERSION_TAG else "PS5-Mango-linux"
    return os.path.join(WORKDIR, name)


# WFM_BIN lets a counterfactual run point the whole suite at a deliberately
# re-broken binary (see .build/nas-counterfactual-wsl.sh) without touching the
# tree. Default is the normal build.
BIN = os.environ.get("WFM_BIN") or _host_bin()
ROOT = "/tmp/PS5-Mango-hosttest"
# SMB fixture port. Must stay OUT of the Windows reserved ranges
# (`netsh interface ipv4 show excludedportrange protocol=tcp` lists 1390-1489);
# see .build/nas-fixture.sh.
NAS_PORT = int(os.environ.get("WFM_NAS_PORT", "1500"))

PASS = 0
FAIL = 0
SKIP = []
FAILED = []


def nas_port_open(port):
    try:
        s = socket.create_connection(("127.0.0.1", port), timeout=1)
        s.close()
        return True
    except Exception:
        return False


def check(name, cond, detail=""):
    global PASS, FAIL
    if cond:
        PASS += 1
        print("  ok   %s" % name)
    else:
        FAIL += 1
        FAILED.append(name)
        print("  FAIL %s   %s" % (name, detail))


def encode_uri_component(s):
    # Matches JS encodeURIComponent (does not escape !'()*-._~ A-Za-z0-9)
    return urllib.parse.quote(s, safe="!'()*-._~")


# 2026..2123 会被 Windows/Hyper-V 的动态保留范围吃掉（netsh interface ipv4
# show excludedportrange —— 2026-10-10 实测 2024-2123），服务端
# find_available_port 会自动跳过它们落到 2124+，所以这里要扫得足够远。
def wait_port(ports=range(2026, 2141), timeout=20):
    end = time.time() + timeout
    while time.time() < end:
        for p in ports:
            try:
                c = http.client.HTTPConnection("127.0.0.1", p, timeout=2)
                c.request("GET", "/api/status")
                r = c.getresponse()
                body = r.read()
                c.close()
                # ⚠️ 不能只看 200：2026-10-20 实测 Windows 侧一个 Go 服务
                # （mirrored 网络共享进 WSL）监听着 2026-2140 段里的 2082，
                # 对任何 GET 都回 200 —— 把它当成服务器就是整场 221 连跪。
                # 必须确认应答体长得像我们的 /api/status（ok:true + version）。
                if r.status != 200 or not body:
                    continue
                j = json.loads(body.decode("utf-8", "replace"))
                if j.get("ok") is True and j.get("version") == VERSION_TAG \
                        and j.get("port") == p:
                    return p
            except Exception:
                pass
        time.sleep(0.2)
    return None


class Client:
    def __init__(self, port):
        self.port = port

    def raw(self, method, path, body=None, headers=None):
        conn = http.client.HTTPConnection("127.0.0.1", self.port, timeout=60)
        conn.request(method, path, body=body, headers=headers or {})
        r = conn.getresponse()
        data = r.read()
        st = r.status
        conn.close()
        return st, data

    def j(self, st, data):
        try:
            return json.loads(data.decode("utf-8"))
        except Exception:
            return None

    def get(self, path):
        return self.raw("GET", path)

    def get_json(self, path):
        st, d = self.raw("GET", path)
        return st, self.j(st, d)

    def api_json(self, path, obj):
        b = json.dumps(obj, ensure_ascii=False).encode("utf-8")  # JSON.stringify
        st, d = self.raw("POST", path, b, {"Content-Type": "application/json"})
        return st, self.j(st, d)

    def api_post(self, path, obj):
        b = urllib.parse.urlencode(obj).encode("utf-8")
        st, d = self.raw("POST", path, b,
                         {"Content-Type": "application/x-www-form-urlencoded"})
        return st, self.j(st, d)

    def upload(self, dst, name, data, rel=None, size=None, overwrite=None):
        h = {
            "Content-Type": "application/octet-stream",
            "X-WFM-Dir": encode_uri_component(dst),
            "X-WFM-Name": encode_uri_component(name),
            "X-WFM-Size": str(len(data) if size is None else size),
        }
        if rel is not None:
            h["X-WFM-Rel"] = encode_uri_component(rel)
        if overwrite is not None:
            h["X-WFM-Overwrite"] = overwrite
        st, d = self.raw("POST", "/api/upload-file", data, h)
        return st, self.j(st, d)

    def wait_idle(self, timeout=40):
        end = time.time() + timeout
        while time.time() < end:
            st, j = self.api_json("/api/tasks", {})
            tasks = (j or {}).get("tasks", [])
            active = [t for t in tasks
                      if t.get("state_name") in ("queued", "running", "paused")]
            if not active:
                return tasks
            time.sleep(0.2)
        return None


def list_names(c, path):
    st, j = c.api_json("/api/fs/list", {"scheme": "local", "path": path})
    if not j or not j.get("ok"):
        return None
    return {e["name"] for e in j.get("entries", [])}


def rmtree_rw(path):
    """Remove a tree even when a fixture deliberately made parts of it read-only.

    ⚠️ Why plain `shutil.rmtree(..., ignore_errors=True)` is not enough: the save
    / snapshot fixtures chmod directories to 0555 on purpose to exercise the
    "cannot write here" paths. unlinking a child needs write permission on the
    *directory*, so rmtree fails on that child, and `ignore_errors=True` turns
    that failure into silence — the read-only tree survives the wipe. The run
    then dies ~20 lines later with a PermissionError while merely opening
    `snaps/<title>/111/sd.bin`, which reads like an unrelated product bug. That
    is a "looks healthy but never actually cleans" criterion, so: retry with the
    permission fixed, and (see main) assert the wipe really happened.
    """
    def onerror(func, p, exc):
        for target in (os.path.dirname(p), p):
            try:
                os.chmod(target, 0o700)
            except OSError:
                pass
        try:
            func(p)
        except OSError:
            pass

    if os.path.isdir(path) and not os.path.islink(path):
        shutil.rmtree(path, onerror=onerror)
    else:
        os.remove(path)


def main():
    # ---- start server ----
    os.makedirs(ROOT, exist_ok=True)
    for n in os.listdir(ROOT):
        try:
            rmtree_rw(os.path.join(ROOT, n))
        except OSError as e:
            print("FATAL: cannot clear %s/%s: %s" % (ROOT, n, e), file=sys.stderr)
            return 9
    leftover = os.listdir(ROOT)
    if leftover:
        print("FATAL: %s not empty after wipe: %s" % (ROOT, leftover), file=sys.stderr)
        return 9

    save_root = os.path.join(ROOT, "saveroot")
    snap_dir = os.path.join(ROOT, "snaps")
    brew_dir = os.path.join(ROOT, "homebrew")
    # A title's live save dir + an old snapshot, so backup/restore have fixtures.
    # ★ 布局必须与真机一致：<account home>/savedata_prospero/<TITLE_ID>。
    #   WFM_SAVE_ROOT 扮演「一个账号的 home」，savedata_prospero 是它下面的 leaf。
    #   夹具里把两级写反的话，测试和真机会同时绿 —— 那种绿什么也没证明。
    title = "CUSA00001"
    os.makedirs(os.path.join(save_root, "savedata_prospero", title), exist_ok=True)
    with open(os.path.join(save_root, "savedata_prospero", title, "sd.bin"), "wb") as f:
        f.write(b"LIVE-V2")
    # 第二个存档 + 一个 meta 叶：/api/save/scan 要能同时列出两个叶下的条目。
    os.makedirs(os.path.join(save_root, "savedata_prospero", "GAME00002"), exist_ok=True)
    with open(os.path.join(save_root, "savedata_prospero", "GAME00002", "g2.bin"), "wb") as f:
        f.write(b"G2-SAVE-0")
    # DELTEST01：有 live 存档但**没有任何快照** —— 15d-quinquies 的 no_snapshot
    # 硬门槛断言用它（删前必须已存在快照，否则拒绝删除）。
    os.makedirs(os.path.join(save_root, "savedata_prospero", "DELTEST01"), exist_ok=True)
    with open(os.path.join(save_root, "savedata_prospero", "DELTEST01", "d.bin"), "wb") as f:
        f.write(b"DEL-NO-SNAP")
    os.makedirs(os.path.join(save_root, "savedata_prospero_meta", "GAME00002"), exist_ok=True)
    with open(os.path.join(save_root, "savedata_prospero_meta", "GAME00002", "m.bin"), "wb") as f:
        f.write(b"META")
    os.makedirs(os.path.join(snap_dir, title, "111"), exist_ok=True)
    with open(os.path.join(snap_dir, title, "111", "sd.bin"), "wb") as f:
        f.write(b"OLD-V1")
    os.makedirs(brew_dir, exist_ok=True)

    # app 根夹具：PKG 管理页必须显示 SMB 流式装好的游戏。
    # 真机 2026-10-22 报障（用户原话）：「我通过 SMB 安装好的 PKG 游戏，在 PKG
    # 管理页面不显示，但是我已经可以玩了」。根因：PKG 页只扫 .pkg 文件，而 SMB
    # 流式安装不在本机留镜像，游戏装在 app 目录里，六个文件根一个都扫不到。
    # 布局对齐真机 /user/app：一层 PPSA*/CUSA* 目录，且必须是目录。
    #   PPSA01234  app.db 有行      → 名字取 app.db（第一货源）
    #   CUSA09999  app.db 无行      → 回落 title_id（两处都查不到也不能挡住列表）
    #   NPXS30000  非 PPSA/CUSA 前缀 → 必须跳过
    #   notagame   同上
    #   PPSA05555  **同名普通文件**  → 必须跳过（S_ISDIR 判据）
    approot = os.path.join(ROOT, "approot")
    os.makedirs(os.path.join(approot, "PPSA01234", "sce_sys"), exist_ok=True)
    os.makedirs(os.path.join(approot, "CUSA09999"), exist_ok=True)
    os.makedirs(os.path.join(approot, "NPXS30000"), exist_ok=True)
    os.makedirs(os.path.join(approot, "notagame"), exist_ok=True)
    with open(os.path.join(approot, "PPSA05555"), "wb") as f:
        f.write(b"not-a-dir")

    # Device-root fixtures for the mount check (WFM_DEVICE_ROOT_PREFIX below).
    #   devroot/empty  -> an empty directory on the SAME filesystem as the prefix,
    #                     i.e. exactly what an unpopulated /mnt/usb0 looks like
    #   devroot/full   -> has content, so it counts as attached
    devroot = os.path.join(ROOT, "devroot")
    os.makedirs(os.path.join(devroot, "empty"), exist_ok=True)
    os.makedirs(os.path.join(devroot, "full", "games"), exist_ok=True)
    with open(os.path.join(devroot, "full", "games", "x.pkg"), "wb") as f:
        f.write(b"pkg")

    # 崩溃残留夹具：启动期 reconcile 必须清掉**本工具自己**的暂存前缀，而
    # /data 下别的东西一根汗毛都不能动。真机画像（2026-10-10）：上一次异常退出
    # 把几百 MB 的 _nex_arc 留在 /data/Mango/tmp，自启后的新进程写什么都失败，
    # 用户看到的是「插件功能全不能用」。这里把它做成端到端断言。
    nex_tmp = os.path.join(ROOT, "data", "Mango", "tmp")
    os.makedirs(os.path.join(nex_tmp, "_nex_arc", "CUSA00001"), exist_ok=True)
    with open(os.path.join(nex_tmp, "_nex_arc", "CUSA00001", "sdimg_x"), "wb") as f:
        f.write(b"STALE-ARCHIVE-STAGING")
    for stale in ("_nex_zip_4242.tmp", "_nex_up_4242.zip", "_nex_new_CUSA00001",
                  "_nex_enc_4242", "_nex_bak_4242.zip"):
        with open(os.path.join(nex_tmp, stale), "wb") as f:
            f.write(b"stale")
    with open(os.path.join(nex_tmp, "user-kept.txt"), "wb") as f:
        f.write(b"DO NOT TOUCH")

    env = dict(os.environ)
    env["WFM_SAVE_ROOT"] = save_root
    env["WFM_SNAPSHOT_DIR"] = snap_dir
    env["WFM_HOMEBREW_DIR"] = brew_dir
    env["WFM_DEVICE_ROOT_PREFIX"] = devroot
    # /data/* 重定向（save_mgr backup/encrypt/resign 的临时区与备份落点）。
    # 仅 savemgr.c 的 sm_map_path 消费；与 WFM_SAVE_ROOT 互不重叠。
    env["WFM_DATA_ROOT"] = os.path.join(ROOT, "data")
    # /system_data/* 同样是 savemgr.c 的 sm_map_path 消费：savedata.db 就在那
    # 下面。宿主上建不出真机的 /system_data，不重定向的话「注册/注销系统存档库」
    # 这条路径在宿主测试里根本跑不到（restore 会死在 -7）。
    env["WFM_SYSTEM_ROOT"] = os.path.join(ROOT, "system")
    os.makedirs(env["WFM_SYSTEM_ROOT"], exist_ok=True)
    # app.db 夹具：游戏名的第一货源（sm_db_game_title 读它，扫 app 根时每个条目
    # 都会问一遍）。表/列名照抄真机 schema（tbl_contentinfo.titleId/titleName）——
    # 生产代码按「有 titleId 列 + 有显示名列」动态发现表，表名若自己发明一个，
    # 发现逻辑等于没测。Python 内置 sqlite3、rollback journal（无 -wal 残留），
    # 与 C 侧只读打开（mode=ro&nolock=1）口径一致。
    appdb = os.path.join(env["WFM_SYSTEM_ROOT"], "priv", "mms", "app.db")
    os.makedirs(os.path.dirname(appdb), exist_ok=True)
    _cdb = sqlite3.connect(appdb)
    try:
        _cdb.execute("CREATE TABLE tbl_contentinfo "
                     "(titleId TEXT, titleName TEXT, contentId TEXT)")
        _cdb.execute("INSERT INTO tbl_contentinfo VALUES (?,?,?)",
                     ("PPSA01234", "测试游戏甲",
                      "PPSA01234_00-0000123400000000"))
        _cdb.commit()
    finally:
        _cdb.close()
    # app 根重定向：宿主上建不出 /user/app（非 root），只有这一个 app 根能映射；
    # 另两个（/mnt/ext1/user/app、/data/app）在宿主必然 opendir 失败 —— 这正好
    # 是「unreadable 上报」的现成断言素材，见 15c-bis。
    env["WFM_APP_ROOT"] = approot
    # Delete-phase forensics log: the console kills the payload without a
    # message, so the delete worker appends each phase to this file. Host tests
    # point it into the workdir and assert the phases actually land there.
    # ⚠️ The log is APPEND-mode and survives across runs — a stale log would
    # make the phase assertions pass against last run's bytes (a fake green),
    # so it must be removed here before the server starts.
    env["WFM_SAVE_LOG"] = os.path.join(WORKDIR, "save-delete.log")
    try:
        os.remove(env["WFM_SAVE_LOG"])
    except FileNotFoundError:
        pass

    # 2026 可能被 Windows/Hyper-V 动态保留范围整段吃掉（connect 说空闲、
    # bind EADDRINUSE，2026-10-10 实测 2024-2123）—— 服务端逐端口重试要
    # 每轮 sleep(3) 爬几分钟。这里预先 bind 试一下，绑不上就让服务端直接
    # 从保留段外的 2124 起步（WFM_PORT，仅宿主消费）。
    try:
        _probe = socket.socket()
        _probe.bind(("0.0.0.0", 2026))
        _probe.close()
    except OSError:
        env["WFM_PORT"] = "2124"

    proc = subprocess.Popen([BIN], cwd=WORKDIR, env=env,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    try:
        port = wait_port()
        if not port:
            print("FATAL: server never came up")
            out = proc.stdout.read(4000).decode("utf-8", "replace")
            print(out)
            return 1
        print("server on port %d\n" % port)
        c = Client(port)

        # ---- 1. status ----
        st, j = c.get_json("/api/status")
        check("GET /api/status ok+port", st == 200 and j and j.get("ok")
              and j.get("port") == port, "st=%s j=%s" % (st, j))
        check("GET /api/status has http flag", bool(j and j.get("http") is True))
        # The version the running binary was compiled with must equal the one
        # the Makefile claims. This is the assertion that makes a stale build
        # (flags changed but nothing relinked) impossible to miss.
        check("GET /api/status reports the Makefile's VERSION_TAG",
              j and j.get("version") == VERSION_TAG,
              "got %r want %r" % (j.get("version") if j else None, VERSION_TAG))

        # ---- 1b. boot reconcile cleared last run's leftovers ----
        # Everything below is device-shaped: it runs at startup, before the
        # listener, and is the only thing standing between "the payload crashed
        # once" and "the payload is unusable until you delete files by hand".
        leftover = sorted(os.listdir(nex_tmp)) if os.path.isdir(nex_tmp) else ["<missing>"]
        check("boot reconcile removed stale _nex_* staging",
              all(not n.startswith("_nex_") for n in leftover),
              "still there: %s" % leftover)
        check("boot reconcile left unrelated files alone",
              "user-kept.txt" in leftover, "dir now: %s" % leftover)
        check("boot reconcile removed the _nex_arc staging tree",
              not os.path.isdir(os.path.join(nex_tmp, "_nex_arc")))

        # ---- 1c. import_log carries the progress triple ----
        # 前端整个进度条就靠这四个字段；少了任何一个，长任务都会退回「请勿关页」
        # 那种让用户以为死机的界面。
        st, j = c.get_json("/api/save_mgr/import_log")
        check("GET /api/save_mgr/import_log -> phase/done/total/text + lines",
              st == 200 and j and isinstance(j.get("phase"), int)
              and isinstance(j.get("done"), int) and isinstance(j.get("total"), int)
              and isinstance(j.get("text"), str) and isinstance(j.get("lines"), list)
              and j.get("phase") == 0, "st=%s j=%s" % (st, j))

        # ---- 2. tasks (GET + JSON POST) ----
        st, j = c.get_json("/api/tasks")
        check("GET /api/tasks ok+tasks[]", st == 200 and j and j.get("ok")
              and isinstance(j.get("tasks"), list), "st=%s j=%s" % (st, j))
        st, j = c.api_json("/api/tasks", {})
        check("POST /api/tasks (json) ok", st == 200 and j and j.get("ok"))

        # ---- 3. list ----
        st, j = c.api_json("/api/fs/list", {"scheme": "local", "path": ROOT})
        check("POST /api/fs/list ok,path,entries", j and j.get("ok")
              and j.get("path") == ROOT and isinstance(j.get("entries"), list),
              "j=%s" % j)

        # ---- 4. space ----
        st, j = c.api_json("/api/space", {"path": ROOT})
        check("POST /api/space top-level free/total/path", j and j.get("ok")
              and isinstance(j.get("free"), int) and isinstance(j.get("total"), int)
              and j.get("path") == ROOT, "j=%s" % j)

        # ---- 5. text create/save/read (JSON, like the front-end) ----
        st, j = c.api_json("/api/text/create", {"path": ROOT, "name": "note.txt"})
        check("POST /api/text/create (json)", j and j.get("ok"), "st=%s j=%s" % (st, j))

        st, j = c.api_json("/api/fs/read", {"path": ROOT + "/note.txt"})
        check("POST /api/fs/read new file -> version+empty", j and j.get("ok")
              and "version" in j and j.get("data") == "", "j=%s" % j)
        version = (j or {}).get("version")

        body = "hello world\n"
        st, j = c.api_json("/api/text/save",
                           {"path": ROOT + "/note.txt", "content": body,
                            "version": version})
        check("POST /api/text/save (json, CAS)", j and j.get("ok"), "j=%s" % j)

        st, j = c.api_json("/api/fs/read", {"path": ROOT + "/note.txt"})
        got = ""
        try:
            got = base64.b64decode(j["data"]).decode()
        except Exception:
            pass
        check("POST /api/fs/read round-trips content", got == body, "got=%r" % got)

        # ---- 5b. text endpoints also accept form encoding (req_param fix) ----
        st, j = c.api_post("/api/text/create", {"path": ROOT, "name": "form.txt"})
        okform = bool(j and j.get("ok"))
        if okform:
            st, j = c.api_post("/api/text/save",
                               {"path": ROOT + "/form.txt", "content": "x"})
            okform = bool(j and j.get("ok"))
        check("text create/save also accepts form encoding", okform, "j=%s" % j)

        # ---- 6. rename ----
        st, j = c.api_json("/api/rename", {"path": ROOT + "/note.txt", "name": "real.txt"})
        check("POST /api/rename ok", j and j.get("ok"), "j=%s" % j)
        names = list_names(c, ROOT) or set()
        check("rename took effect on disk", "real.txt" in names and "note.txt" not in names,
              "names=%s" % names)

        # ---- 7. mkdir + errno report ----
        st, j = c.api_json("/api/mkdir", {"path": ROOT, "name": "sub"})
        check("POST /api/mkdir ok", j and j.get("ok"), "j=%s" % j)
        st, j = c.api_json("/api/mkdir", {"path": ROOT, "name": "sub"})
        err = (j or {}).get("error", "")
        check("mkdir EEXIST reports a real errno, not 'Success'", j
              and not j.get("ok") and err and err.lower() != "success",
              "j=%s" % j)

        # ---- 8. copy then move (task-driven) ----
        st, j = c.api_json("/api/copy", {"paths": ROOT + "/real.txt",
                                         "dst": ROOT + "/sub", "overwrite": "0"})
        check("POST /api/copy returns task_id", j and j.get("ok")
              and j.get("task_id") is not None, "j=%s" % j)
        c.wait_idle()
        check("copy produced sub/real.txt", "real.txt" in (list_names(c, ROOT + "/sub") or set()))

        st, j = c.api_json("/api/move", {"paths": ROOT + "/real.txt",
                                         "dst": ROOT + "/sub", "overwrite": "1"})
        check("POST /api/move returns task_id", j and j.get("ok")
              and j.get("task_id") is not None, "j=%s" % j)
        c.wait_idle()
        names = list_names(c, ROOT) or set()
        check("move removed source from ROOT", "real.txt" not in names, "names=%s" % names)

        # ---- 9. upload (raw streaming, like XHR) ----
        payload = b"0123456789ABCDEF"          # 16 bytes
        st, j = c.upload(ROOT, "up.bin", payload)
        check("POST /api/upload-file (raw, 16B) ok", j and j.get("ok"), "st=%s j=%s" % (st, j))
        ok_on_disk = False
        try:
            with open(os.path.join(ROOT, "up.bin"), "rb") as f:
                ok_on_disk = f.read() == payload
        except Exception:
            pass
        check("uploaded bytes match on disk", ok_on_disk)

        # ---- 9b. folder upload (X-WFM-Rel subtree) ----
        st, j = c.upload(ROOT, "kid.txt", b"abc", rel="folder/kid.txt")
        c.wait_idle()
        check("folder upload recreates subtree",
              "kid.txt" in (list_names(c, ROOT + "/folder") or set()),
              "j=%s" % j)

        # ---- 9c. staging upload into a NON-EXISTENT dir chain (真机 bug 回归) ----
        # 存档 ZIP 导入「一上传就失败」的根因：/api/upload-file 的目标目录
        # /data/Mango/incoming 首次使用时整条不存在，而 ensure_parent_dirs 只处理
        # rel 里的子路径（纯文件名时一个目录都不建）→ check_target_writable
        # 报 "current directory is not writable" → 500 → 前端误报网络故障。
        # 另外两个配套契约：EEXIST 409 必须带真实 error 文案（前端现在会显示
        # 它）；X-WFM-Overwrite:1 重传必须成功（ZIP/PKG 暂存同名覆盖）。
        deep = ROOT + "/Mango/incoming"         # 整条链在宿主夹具里都不存在
        st, j = c.upload(deep, "PPSA23226.zip", b"PK\x03\x04ZIPBODY")
        check("upload into a non-existent dir chain creates it (save-import "
              "first-attempt bug)", st == 200 and j and j.get("ok"),
              "st=%s j=%s" % (st, j))
        on_disk = False
        try:
            with open(os.path.join(deep, "PPSA23226.zip"), "rb") as f:
                on_disk = f.read() == b"PK\x03\x04ZIPBODY"
        except Exception:
            pass
        check("staged zip bytes match on disk", on_disk)
        st, j = c.upload(deep, "PPSA23226.zip", b"PK\x03\x04ZIPBODY2")
        check("re-upload without overwrite is a real 409 with a real message",
              st == 409 and j and not j.get("ok")
              and "exists" in ((j or {}).get("error") or "").lower(),
              "st=%s j=%s" % (st, j))
        st, j = c.upload(deep, "PPSA23226.zip", b"PK\x03\x04ZIPBODY3",
                         overwrite="1")
        check("re-upload with X-WFM-Overwrite: 1 succeeds (UI retry path)",
              st == 200 and j and j.get("ok"), "st=%s j=%s" % (st, j))
        try:
            with open(os.path.join(deep, "PPSA23226.zip"), "rb") as f:
                on_disk = f.read() == b"PK\x03\x04ZIPBODY3"
        except Exception:
            on_disk = False
        check("overwrite replaced the staged bytes", on_disk)

        # ---- 9d. live upload progress（服务端真实字节，PS5 WebKit onprogress 稀疏）
        # 前端进度条现在吃 /api/upload/progress 的服务端口径，契约必须稳：
        #   · 空闲时 uploads 是数组（可为空）；
        #   · 上传在飞时目标路径出现在 uploads 里且 received 单调不回退；
        #   · 上传结束后条目必须消失（不然前端会把旧进度当成新上传的）。
        st, j = c.api_json("/api/upload/progress", {})
        check("GET /api/upload/progress ok with uploads[]", st == 200 and j
              and j.get("ok") and isinstance(j.get("uploads"), list),
              "st=%s j=%s" % (st, j))
        _prog_target = deep + "/progress-probe.bin"
        _prog_total = 512 * 1024
        _conn = http.client.HTTPConnection("127.0.0.1", c.port, timeout=30)
        _conn.putrequest("POST", "/api/upload-file")
        _conn.putheader("X-WFM-Dir", encode_uri_component(deep))
        _conn.putheader("X-WFM-Name", encode_uri_component("progress-probe.bin"))
        _conn.putheader("X-WFM-Size", str(_prog_total))
        _conn.putheader("X-WFM-Overwrite", "1")
        _conn.putheader("Content-Length", str(_prog_total))
        _conn.endheaders()
        _conn.send(b"P" * (64 * 1024))       # 只发前 1/8，连接保持打开
        time.sleep(0.4)
        st, j = c.api_json("/api/upload/progress", {})
        _rows = [u for u in ((j or {}).get("uploads") or [])
                 if u.get("target") == _prog_target]
        check("in-flight upload shows up with server-side bytes",
              st == 200 and j.get("ok") and len(_rows) == 1
              and _rows[0].get("received") >= 64 * 1024
              and _rows[0].get("total") == _prog_total,
              "j=%s" % (j,))
        _conn.send(b"Q" * (_prog_total - 64 * 1024))
        _resp = _conn.getresponse()
        _body = _resp.read()
        _conn.close()
        check("in-flight upload finishes ok", _resp.status == 200
              and json.loads(_body.decode()).get("ok"), "st=%s" % _resp.status)
        st, j = c.api_json("/api/upload/progress", {})
        _left = [u for u in ((j or {}).get("uploads") or [])
                 if u.get("target") == _prog_target]
        check("finished upload disappears from progress ledger",
              st == 200 and j.get("ok") and not _left, "j=%s" % (j,))

        # ---- 10. chinese filename round-trip (json_escape codepoint fix) ----
        zh = "测试目录"
        st, j = c.api_json("/api/mkdir", {"path": ROOT, "name": zh})
        check("mkdir chinese name ok", j and j.get("ok"), "j=%s" % j)
        names = list_names(c, ROOT)
        check("chinese name round-trips through json_escape", names is not None
              and zh in names, "names=%s" % names)

        # ---- 11. .log read (user-reported crash case) ----
        logpath = os.path.join(ROOT, "big.log")
        logdata = b"line one\n\x00\x01\x02 binary\n" + b"x" * 5000
        with open(logpath, "wb") as f:
            f.write(logdata)
        st, j = c.api_json("/api/fs/read", {"path": ROOT + "/big.log"})
        check("read .log (with NUL bytes) ok", j and j.get("ok")
              and j.get("size") == len(logdata),
              "size=%s want=%s" % (j.get("size") if j else None, len(logdata)))

        # ---- 12. delete (task) ----
        st, j = c.api_json("/api/delete", {"paths": ROOT + "/sub/real.txt"})
        check("POST /api/delete returns task_id", j and j.get("ok")
              and j.get("task_id") is not None, "j=%s" % j)
        c.wait_idle()
        check("delete removed the file",
              "real.txt" not in (list_names(c, ROOT + "/sub") or set()))

        # ---- 12b. 删除是**条目计数**型任务：分母必须是条目数，不是字节 ----
        # 用户报的现象原话：「删除多个文件的时候会出进度弹窗 但是不显示删除文件
        # 和进度条都是 0」。根因在 TASK_DELETE 分支：它既没 task_set_total()，
        # remove_path() 里那次 task_update(..., add_done=0, ...) 的加数又恒为 0
        # ⇒ 分母与分子同时是 0，进度条永远 0%。
        #
        # 夹具故意做成「条目数与字节数不相等、且条目数能手算」的形状：
        #   5 个文件 + 4 个目录（deltree 自身 + one + two + two/deep）= 9 项
        # 字节和 = 1+2+3+4+3 = 13。
        # 坏版本 ⇒ total=0；若有人把分母改成字节 ⇒ total=13。两个都会立刻变红，
        # 所以这一条同时钉住了「有分母」和「分母的口径」。
        deltree = os.path.join(ROOT, "deltree")
        os.makedirs(os.path.join(deltree, "one"), exist_ok=True)
        os.makedirs(os.path.join(deltree, "two", "deep"), exist_ok=True)
        for n, sz in (("a.bin", 1), ("b.bin", 2), ("c.bin", 3)):
            with open(os.path.join(deltree, n), "wb") as f:
                f.write(b"x" * sz)
        with open(os.path.join(deltree, "one", "d.bin"), "wb") as f:
            f.write(b"x" * 4)
        with open(os.path.join(deltree, "two", "deep", "e.bin"), "wb") as f:
            f.write(b"x" * 3)
        DEL_ENTRIES = 9          # 5 files + 4 dirs
        DEL_BYTES = 13           # 1+2+3+4+3 —— 用来区分「条目数」和「字节数」

        st, j = c.api_json("/api/delete", {"paths": deltree})
        del_id = (j or {}).get("task_id")
        check("delete of a whole tree queues a task",
              j and j.get("ok") and del_id is not None, "j=%s" % j)
        del_frames = []
        if del_id is not None:
            # ★ 只能走 /api/install/poll：/api/tasks 会在下一次调用时把已完结的
            #   行摘掉，那样连 total 都读不回来（read_finished 不保号）。
            for _ in range(200):
                st, pj = c.api_post("/api/install/poll", {"id": str(del_id)})
                if not pj or not pj.get("ok"):
                    break
                del_frames.append(pj)
                if pj.get("state_name") in ("done", "failed", "cancelled"):
                    break
                time.sleep(0.02)
        del_last = del_frames[-1] if del_frames else {}
        check("delete task carries a non-zero item-count total",
              (del_last.get("total") or 0) == DEL_ENTRIES,
              "total=%s want=%s frames=%s" % (del_last.get("total"), DEL_ENTRIES,
                                              del_frames[:2]))
        check("delete progress climbs to total (bar no longer stuck at 0/0)",
              del_last.get("state_name") == "done"
              and (del_last.get("progress") or 0) == DEL_ENTRIES,
              "last=%s" % del_last)
        check("delete total is an item count, not a byte sum (%d != %d)"
              % (DEL_ENTRIES, DEL_BYTES),
              (del_last.get("total") or 0) != DEL_BYTES, "last=%s" % del_last)
        # 进度不能超过分母（remove_path 对每个条目恰好调用一次 ⇒ 天然对齐）
        check("delete never reports progress beyond total",
              all((f.get("progress") or 0) <= (f.get("total") or 0)
                  for f in del_frames),
              "frames=%s" % del_frames[:4])
        c.wait_idle()
        check("delete removed the whole tree", not os.path.exists(deltree))

        # ---- 12c. 取消必须认**数字** id（任务坞按钮发的就是 {id: <number>}） ----
        # 真因在 body_json_value：非字符串值（数字/布尔）以前被「跳到下一个逗号」，
        # 从不返回 ⇒ id=NULL→0→"active task not found"。删除弹窗发字符串 id
        # （"7"）所以能取消，任务坞发数字所以永远取消不了 —— 半边 UI 坏了却
        # 一直没被发现。夹具：一个足够大的文件让复制跑一会儿，再点取消。
        cc_src = os.path.join(ROOT, "cc-src")
        cc_dst = os.path.join(ROOT, "cc-dst")
        os.makedirs(cc_src, exist_ok=True)
        with open(os.path.join(cc_src, "blob.bin"), "wb") as f:
            f.write(b"x" * (150 * 1024 * 1024))
        st, j = c.api_json("/api/copy", {"paths": cc_src, "dst": cc_dst,
                                         "overwrite": "0"})
        cc_id = (j or {}).get("task_id")
        check("copy of a large file queues (cancel fixture)",
              j and j.get("ok") and cc_id is not None, "j=%s" % j)
        cc_canceled = False
        cc_state = None
        cc_row = None
        if cc_id is not None:
            # 任务窗口的「速度 / 已用时 / 剩余」三格全靠 /api/tasks 新吐出的三个字段
            # （task.c 早就在采样 speed/eta，以前只差输出）。跑着的任务一定能看到。
            for _ in range(30):
                st4, tj = c.get_json("/api/tasks")
                rows = (tj or {}).get("tasks", [])
                cc_row = next((r for r in rows if r.get("id") == cc_id), None)
                if cc_row and cc_row.get("state_name") == "running":
                    break
                time.sleep(0.1)
            check("running task row exposes speed / eta / created_at",
                  cc_row is not None and "speed" in cc_row and "eta" in cc_row
                  and "created_at" in cc_row, "row=%s" % (cc_row,))
            check("running task row's elapsed is derivable (created_at is epoch)",
                  cc_row is not None and (cc_row.get("created_at") or 0) > 0,
                  "created_at=%s" % ((cc_row or {}).get("created_at"),))
            for _ in range(40):
                # ★ 数字 id，不是字符串 —— 这正是被丢掉的那种值
                st2, j2 = c.api_json("/api/task/cancel", {"id": cc_id})
                if j2 and j2.get("ok"):
                    cc_canceled = True
                    break
                st3, pj = c.api_post("/api/install/poll", {"id": str(cc_id)})
                cc_state = (pj or {}).get("state_name")
                if cc_state in ("done", "failed", "cancelled"):
                    break
                time.sleep(0.15)
            # 取消受理后等 worker 真正停下，并在任务被摘走**之前**读到终态帧
            for _ in range(60):
                st3, pj = c.api_post("/api/install/poll", {"id": str(cc_id)})
                if pj and pj.get("state_name") in ("done", "failed", "cancelled"):
                    cc_state = pj.get("state_name")
                    break
                time.sleep(0.1)
            c.wait_idle()
        check("cancel with a NUMERIC id is accepted (dock sends {id: n})",
              cc_canceled, "id=%s state=%s" % (cc_id, cc_state))
        check("numerically cancelled task actually stops (state=cancelled)",
              cc_state == "cancelled", "state=%s" % cc_state)

        # ---- 13. chmod (task) ----
        st, j = c.api_json("/api/chmod", {"paths": ROOT + "/up.bin",
                                          "mode": "0755", "recursive": "0"})
        check("POST /api/chmod returns task_id", j and j.get("ok"), "j=%s" % j)
        c.wait_idle()

        # ---- 13b. download: the endpoint reads ?id= (a task), never ?path= ----
        # 真机反馈「选中一个文件下载到 PC 显示下载失败」的根因：前端直接拼
        # /api/download?path=…，而 src/download.c 只读 query 的 "id"，缺省为 0 ⇒
        # find_task_locked(0) 落空 ⇒ 404 JSON ⇒ 浏览器报「下载失败」。
        # 这一组把「prepare → 拿 task_id → 取流」这条唯一通路钉死，并且把旧调用
        # 形态留成回归哨兵 —— 它必须**仍然是 404**，否则说明有人又给 ?path= 开了口子。
        dl_bytes = b"PS5-Mango download fixture\n" + bytes(range(256)) * 8
        with open(os.path.join(ROOT, "dl.bin"), "wb") as f:
            f.write(dl_bytes)
        st, j = c.api_post("/api/download/prepare", {"paths": ROOT + "/dl.bin"})
        check("POST /api/download/prepare returns a numeric task_id",
              st == 200 and j and j.get("ok") and isinstance(j.get("task_id"), int),
              "st=%s j=%s" % (st, j))
        dl_id = (j or {}).get("task_id")
        st, data = c.get("/api/download?id=%s" % dl_id)
        check("GET /api/download?id=<task_id> streams the exact file bytes",
              st == 200 and data == dl_bytes,
              "st=%s len=%d want=%d" % (st, len(data), len(dl_bytes)))
        c.wait_idle()
        st, data = c.get("/api/download")
        stp, datap = c.get("/api/download?path=" + urllib.parse.quote(ROOT + "/dl.bin"))
        jno, jpath = c.j(st, data), c.j(stp, datap)
        check("GET /api/download with no id -> 404 (id defaults to 0, no such task)",
              st == 404 and jno and jno.get("error") == "active task not found",
              "st=%s j=%s" % (st, jno))
        check("GET /api/download?path=... -> 404 (regression sentinel: the old "
              "front-end call shape must stay refused)",
              stp == 404 and jpath and jpath.get("error") == "active task not found",
              "st=%s j=%s" % (stp, jpath))
        # 多路径 = 一个任务出一个 tar。服务端只允许一个活跃任务 ⇒ 前端必须一次
        # prepare 收全部路径，逐个建任务的话第二个起就是 409。
        with open(os.path.join(ROOT, "dl2.bin"), "wb") as f:
            f.write(b"second")
        st, j = c.api_post("/api/download/prepare",
                           {"paths": (ROOT + "/dl.bin") + "\n" + (ROOT + "/dl2.bin")})
        tid2 = (j or {}).get("task_id")
        check("one prepare accepts several newline-separated paths",
              st == 200 and isinstance(tid2, int), "st=%s j=%s" % (st, j))
        st, data = c.get("/api/download?id=%s" % tid2)
        check("multi-path download is a tar whose first entry names the first file",
              st == 200 and len(data) > 512 and b"dl.bin" in data[:100],
              "st=%s len=%d head=%r" % (st, len(data), data[:100]))
        c.wait_idle()

        # ---- 14. extract rejects a non-archive gracefully ----
        st, j = c.api_json("/api/extract", {"path": ROOT + "/up.bin"})
        check("extract non-archive -> clean error", j and not j.get("ok")
              and j.get("error"), "j=%s" % j)

        # ---- 14b. extract honours the front-end's dst_dir / subdir names ----
        # The dialog posts `dst_dir` (the 解压到 picker) + `subdir` (以压缩包名新建
        # 子目录), while this back-end used to read only `destination` / `separate`.
        # Probe: pointing dst_dir at a FILE must be rejected as
        # "destination is not a directory" — that verdict can only be reached if
        # the alias was really read.
        with open(os.path.join(ROOT, "afile.bin"), "wb") as f:
            f.write(b"x")
        with open(os.path.join(ROOT, "arch.rar"), "wb") as f:
            f.write(b"not a real rar")
        st, j = c.api_json("/api/extract", {"path": ROOT + "/arch.rar",
                                            "dst_dir": ROOT + "/afile.bin",
                                            "subdir": "1", "conflict": "overwrite"})
        check("extract reads dst_dir (file as destination -> 409)",
              st == 409 and j and j.get("error_code") == "destination_must_be_directory",
              "st=%s j=%s" % (st, j))

        # In-process engine: a bogus archive is ACCEPTED as a task and the worker
        # fails it with a format verdict (no more 503 helper-missing).
        st, j = c.api_json("/api/extract", {"path": ROOT + "/arch.rar",
                                            "dst_dir": ROOT + "/sub", "subdir": "1"})
        bogus_id = (j or {}).get("task_id")
        check("bogus archive accepted as a task (in-process engine)",
              j and j.get("ok") and bogus_id is not None, "st=%s j=%s" % (st, j))
        settled = None
        for _ in range(80):
            st, pj = c.api_post("/api/install/poll", {"id": str(bogus_id)})
            if pj and pj.get("ok") and pj.get("state_name") in (
                    "done", "failed", "cancelled"):
                settled = pj
                break
            time.sleep(0.25)
        check("bogus archive task fails with a format verdict",
              settled is not None and settled.get("state_name") == "failed",
              "settled=%s" % (settled,))

        # ---- 14c. multi-part RAR: members collapse to the first volume ----
        mp = os.path.join(ROOT, "mpgame.part1.rar")
        with open(mp, "wb") as f:
            f.write(b"rar-part1")
        with open(os.path.join(ROOT, "mpgame.part2.rar"), "wb") as f:
            f.write(b"rar-part2")
        st, j = c.api_json("/api/extract", {"path": ROOT + "/mpgame.part2.rar",
                                            "dst_dir": ROOT + "/sub"})
        bogus2 = (j or {}).get("task_id")
        check("extract .part2.rar alone -> collapses to part1 (task, not 400)",
              j and j.get("ok") and bogus2 is not None, "st=%s j=%s" % (st, j))
        with open(os.path.join(ROOT, "mpgame.r00"), "wb") as f:
            f.write(b"rar-legacy-volume")
        st, j = c.api_json("/api/extract", {"path": ROOT + "/mpgame.r00",
                                            "dst_dir": ROOT + "/sub"})
        check("extract legacy .r00 with .rar missing -> first-part error",
              st == 400 and j and j.get("error_code") == "archive_first_part_missing",
              "st=%s j=%s" % (st, j))

        # ---- 14c-bis. zero-padded volume names must keep their width ----
        # 用户原话：「不选第一个分卷就报错，选第一个分卷就可以」。
        # `mpz.part02.rar` 的第一卷是 `mpz.part01.rar`，不是 `mpz.part1.rar`。
        # 归一函数曾经把数字段直接写成 "1"，补零宽度被吃掉 ⇒ lstat 找不到第一卷
        # ⇒ 报 "multi-part archive is missing its first part"（而它就在旁边）。
        for nm in ("mpz.part01.rar", "mpz.part02.rar"):
            with open(os.path.join(ROOT, nm), "wb") as f:
                f.write(b"rar-" + nm.encode())
        os.makedirs(os.path.join(ROOT, "subz"), exist_ok=True)
        st, j = c.api_json("/api/extract", {"path": ROOT + "/mpz.part02.rar",
                                            "dst_dir": ROOT + "/subz"})
        check("extract .part02.rar -> collapses to .part01.rar (padded width kept)",
              st == 200 and j and j.get("ok") and j.get("task_id") is not None,
              "st=%s j=%s" % (st, j))
        # 反向：真的缺第一卷时仍要报错（证明上面那一条不是「一律放行」）
        os.makedirs(os.path.join(ROOT, "onlyz"), exist_ok=True)
        with open(os.path.join(ROOT, "onlyz", "gone.part03.rar"), "wb") as f:
            f.write(b"rar-x")
        st, j = c.api_json("/api/extract", {"path": ROOT + "/onlyz/gone.part03.rar",
                                            "dst_dir": ROOT + "/subz"})
        check("extract .part03.rar with no .part01 present -> first-part error",
              st == 400 and j and j.get("error_code") == "archive_first_part_missing",
              "st=%s j=%s" % (st, j))

        # ---- 14d. REAL in-process extraction: zip / rar / 7z / volumes ----
        # Fixtures come from PS5-Mango-legacy's engine suite (tests/fixtures/).
        # These are the "host-tested leaves" the helper design could never
        # exercise end-to-end: the unpack runs inside THIS linux binary.
        import shutil
        fx = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                          "tests", "fixtures")

        def fx_extract(names, dst_name, password=None):
            """Copy fixture(s) into the sandbox, enqueue, wait for the task."""
            if isinstance(names, str):
                names = [names]
            for n in names:
                shutil.copyfile(os.path.join(fx, n), os.path.join(ROOT, n))
            dst = os.path.join(ROOT, dst_name)
            os.makedirs(dst, exist_ok=True)
            body = {"path": os.path.join(ROOT, names[0]), "dst_dir": dst,
                    "conflict": "overwrite", "subdir": "0"}
            if password:
                body["password"] = password
            st, j = c.api_json("/api/extract", body)
            tid = (j or {}).get("task_id")
            if tid is None:
                return st, j, None
            for _ in range(240):
                st, pj = c.api_post("/api/install/poll", {"id": str(tid)})
                if pj and pj.get("ok") and pj.get("state_name") in (
                        "done", "failed", "cancelled"):
                    return st, j, pj
                time.sleep(0.25)
            return st, j, None

        # ZIP (minizip-ng + zlib): content-asserted
        st, j, pj = fx_extract("basic.zip", "xz-basic")
        ok = pj is not None and pj.get("state_name") == "done"
        root_txt = nested_txt = None
        if ok:
            root_txt = open(os.path.join(ROOT, "xz-basic", "root.txt"), "rb").read()
            nested_txt = open(os.path.join(ROOT, "xz-basic", "dir", "nested.txt"), "rb").read()
        check("real zip unpack: root.txt content matches",
              bool(ok and root_txt == b"root content"), "got=%r pj=%s" % (root_txt, pj))
        check("real zip unpack: nested dirs created",
              bool(ok and nested_txt == b"nested content"), "got=%r" % (nested_txt,))
        # 进度链：引擎上报 bytes_total，但适配层曾只转发 bytes_done ⇒ task->total 恒 0
        # ⇒ 前端 pctOf() 恒 0 ⇒「解压没有进度条」。这里钉住 total 真的 > 0。
        check("extract reports a non-zero byte total (progress denominator)",
              bool(ok and (pj.get("total") or 0) > 0),
              "progress=%s total=%s" % (pj.get("progress") if pj else None,
                                        pj.get("total") if pj else None))
        check("extract progress never exceeds the total",
              bool(ok and (pj.get("progress") or 0) <= (pj.get("total") or 0)),
              "progress=%s total=%s" % (pj.get("progress") if pj else None,
                                        pj.get("total") if pj else None))

        # RAR (unrar7, C++, RARDLL mode)
        st, j, pj = fx_extract("basic-v6.rar", "xz-rar")
        ok = pj is not None and pj.get("state_name") == "done"
        got = open(os.path.join(ROOT, "xz-rar", "root.txt"), "rb").read() if ok else None
        # fixture's root.txt carries a banner line before the payload text
        check("real rar unpack (unrar7 in-process)",
              bool(ok and got is not None and
                   b"rar v1.9 fixture root content" in got),
              "got=%r pj=%s" % (got, pj))

        # 7z (LZMA SDK, incl. -maes AesOpt TUs)
        st, j, pj = fx_extract("lzma.7z", "xz-7z")
        ok = pj is not None and pj.get("state_name") == "done"
        got = os.path.isfile(os.path.join(ROOT, "xz-7z", "_src", "readme.txt")) if ok else False
        check("real 7z unpack (lzma)", bool(ok and got), "pj=%s" % (pj,))

        # Encrypted: without a password -> archive_password_required; with the
        # right one -> done. Covers PKCRYPT (ZipCrypto) and RAR5 AES-256.
        st, j, pj = fx_extract("enc-zipcrypto.zip", "xz-enc-zc")
        check("zipcrypto zip without password -> password_required failure",
              pj is not None and pj.get("state_name") == "failed"
              and pj.get("error_code") == "archive_password_required",
              "pj=%s" % (pj,))
        st, j, pj = fx_extract("enc-zipcrypto.zip", "xz-enc-zc2", password="secret123")
        check("zipcrypto zip with password -> done",
              pj is not None and pj.get("state_name") == "done", "pj=%s" % (pj,))
        st, j, pj = fx_extract("enc-v6.rar", "xz-enc-rar")
        check("encrypted rar without password -> password_required failure",
              pj is not None and pj.get("state_name") == "failed"
              and pj.get("error_code") == "archive_password_required",
              "pj=%s" % (pj,))
        st, j, pj = fx_extract("enc-v6.rar", "xz-enc-rar2", password="secret123")
        check("encrypted rar with password -> done",
              pj is not None and pj.get("state_name") == "done", "pj=%s" % (pj,))

        # Volume sets: part1 alone works; selecting ANY later member also works
        # because the entry layer collapses members to the first volume; a
        # missing first volume fails loudly instead of hanging.
        st, j, pj = fx_extract(["vol.part1.rar", "vol.part2.rar", "vol.part3.rar"],
                               "xz-vol1")
        check("rar volume set from part1 -> done",
              pj is not None and pj.get("state_name") == "done", "pj=%s" % (pj,))
        st, j, pj = fx_extract(["vol.part1.rar", "vol.part2.rar", "vol.part3.rar"],
                               "xz-vol2")
        # enqueue from part2 only: same outcome, the collapse already ran
        body = {"path": os.path.join(ROOT, "vol.part2.rar"),
                "dst_dir": os.path.join(ROOT, "xz-vol3"),
                "conflict": "overwrite", "subdir": "0"}
        os.makedirs(os.path.join(ROOT, "xz-vol3"), exist_ok=True)
        st, j = c.api_json("/api/extract", body)
        tid = (j or {}).get("task_id")
        pj2 = None
        if tid is not None:
            for _ in range(240):
                st, pj2 = c.api_post("/api/install/poll", {"id": str(tid)})
                if pj2 and pj2.get("ok") and pj2.get("state_name") in (
                        "done", "failed", "cancelled"):
                    break
                time.sleep(0.25)
        check("rar volume set enqueued from part2 -> still done (collapse)",
              pj2 is not None and pj2.get("state_name") == "done", "pj2=%s" % (pj2,))
        st, j, pj = fx_extract("broken.zip.001", "xz-broken")
        check("incomplete zip volume set -> missing-volume failure",
              pj is not None and pj.get("state_name") == "failed"
              and pj.get("error_code") in ("archive_missing_volume",
                                           "archive_extract_failed"),
              "pj=%s" % (pj,))

        # ---- 15. MANGO-only stubs must be graceful ----
        st, j = c.api_post("/api/save/list", {"title_id": "CUSA00000"})
        check("POST /api/save/list ok (empty)", j and j.get("ok"), "j=%s" % j)

        # ---- 15a. ZIP 导入必须走盘上路径（4MB body 上限的真机教训） ----
        # /api/save_mgr/import_zip 的 ZIP 由 /api/upload-file 流式落盘后再读，
        # 不受缓冲 body 上限约束。路由本身要存在、缺 path 要 400 —— 挂了路由
        # 或改了参数名，前端两步导入就静默退回 4MB 时代。
        st, j = c.api_post("/api/save_mgr/import_zip", {})
        check("POST /api/save_mgr/import_zip without path -> 400 path required",
              (not (j and j.get("ok"))) and
              (j or {}).get("error") == "path required", "j=%s" % j)
        st, j = c.api_post("/api/save_mgr/import_zip?path=/no/such.zip", {})
        check("POST /api/save_mgr/import_zip with missing file -> zip not found",
              (not (j and j.get("ok"))) and
              (j or {}).get("error") == "zip not found", "j=%s" % j)

        # ---- 15a-2. 加密镜像导入同样必须走盘上路径 ----
        # 2026-10-10 真机反馈：60MB 镜像一步式 POST 永远卡在「正在上传并挂载
        # 镜像」—— body 超过 4MB 上限后被缓冲到天荒地老。前端已改两步走
        # （/api/upload-file 落盘 + ?path= 挂载）。这里钉住 path 模式契约：
        # 空 body + 合法 path 不再报 empty body；path 越界（/etc 下）要 400。
        # 宿主上 savemgr_mount_image 是 stub（恒 -1），所以合法 path 预期走到
        # 挂载并失败为 500 "mount failed" —— 证明参数解析和文件检查都通过。
        img_logical = "/data/Mango/incoming/_ht_img.img"
        img_path = os.path.join(ROOT, "data", "Mango", "incoming", "_ht_img.img")
        os.makedirs(os.path.dirname(img_path), exist_ok=True)
        with open(img_path, "wb") as f:
            f.write(b"FAKE-IMAGE")
        st, j = c.api_post("/api/save_mgr/import_encrypted?path=" + img_logical, {})
        check("POST /api/save_mgr/import_encrypted path mode -> reaches mount (500 mount failed)",
              st == 500 and (j or {}).get("error") == "mount failed", "st=%s j=%s" % (st, j))
        st, j = c.api_post("/api/save_mgr/import_encrypted?path=/etc/passwd", {})
        check("POST /api/save_mgr/import_encrypted path outside /data/Mango -> 400 not allowed",
              (not (j and j.get("ok"))) and
              (j or {}).get("error") == "path not allowed", "j=%s" % j)
        os.unlink(img_path)

        # ---- 15a-3. 批量归档导入（一个 ZIP 里多个 sdimg_*）----
        # 2026-10-10 真机场景：用户从主机整目录打包出来的存档是**一个 ZIP 里三个
        # sdimg_* 加密镜像**（PPSA23226/sdimg_UserSettingSaveGame 等）。新端点要：
        # 认 ZIP 魔数（不看后缀）、摊包、把 sdimg_* 全列出来、从包内 <tid>/ 目录名
        # 推出标题号。宿主上 savemgr_mount_image 是 stub（恒 -1）⇒ 读不到 param.sfo，
        # 所以这条"从包内目录名推标题号"在宿主上就是**唯一**的推导路径 —— 而它同时
        # 也是真机上「镜像内 PARAMS 被清零」时的兜底，正是要钉住的那一段。
        # 顺序有讲究：finish 的「没有暂存」断言必须排在成功扫描**之前**，否则
        # 会用上一条留下的大清单，永远测不到 409。
        st, j = c.api_post("/api/save_mgr/import_archive_finish?uid=0000000000000001", {})
        check("POST /api/save_mgr/import_archive_finish without a staged scan -> 409",
              st == 409 and (j or {}).get("error") == "no archive staged", "st=%s j=%s" % (st, j))
        st, j = c.api_post("/api/save_mgr/import_archive?path=/etc/passwd", {})
        check("POST /api/save_mgr/import_archive path outside /data/Mango -> 400 not allowed",
              (not (j and j.get("ok"))) and
              (j or {}).get("error") == "path not allowed", "j=%s" % j)

        arc_dir = os.path.join(ROOT, "data", "Mango", "incoming")
        os.makedirs(arc_dir, exist_ok=True)
        notzip = os.path.join(arc_dir, "_ht_notzip.bin")
        with open(notzip, "wb") as f:
            f.write(b"NOT-A-ZIP-AT-ALL")
        st, j = c.api_post(
            "/api/save_mgr/import_archive?path=/data/Mango/incoming/_ht_notzip.bin", {})
        check("POST /api/save_mgr/import_archive on a non-zip -> 400 not a zip",
              (not (j and j.get("ok"))) and (j or {}).get("error") == "not a zip",
              "j=%s" % j)
        os.unlink(notzip)

        arc_path = os.path.join(arc_dir, "_ht_arc.zip")
        with zipfile.ZipFile(arc_path, "w") as z:
            z.writestr("PPSA23226/sdimg_UserSettingSaveGame", b"X" * 64)
            z.writestr("PPSA23226/sdimg_ArchiveSaveFile1", b"Y" * 32)
            z.writestr("PPSA23226/readme.txt", b"ignored")
        st, j = c.api_post("/api/save_mgr/import_archive?path=/data/Mango/incoming/_ht_arc.zip",
                           {})
        items = (j or {}).get("items", [])
        check("POST /api/save_mgr/import_archive -> 只收 sdimg_*，标题号从包内 <tid>/ 推出",
              st == 200 and (j or {}).get("ok") and (j or {}).get("count") == 2
              and sorted(i.get("name") for i in items) ==
                  ["sdimg_ArchiveSaveFile1", "sdimg_UserSettingSaveGame"]
              and set(i.get("title_id") for i in items) == {"PPSA23226"}
              and sorted(i.get("dir") for i in items) ==
                  ["ArchiveSaveFile1", "UserSettingSaveGame"]
              and all(i.get("probed") is False for i in items),
              "st=%s j=%s" % (st, j))
        # 扫描**不**清源 ZIP：用户还没点确认，取消之后暂存里的解压结果会被删，
        # 要重扫就只能从这个源包重新解包。清早了 = 让用户重传几百 MB。
        arc_stage = os.path.join(ROOT, "data", "Mango", "tmp", "_nex_arc")
        check("import_archive 扫描后**保留**源 ZIP（取消/失败还要靠它重扫）+ 暂存已摊开",
              os.path.exists(arc_path) and os.path.isdir(arc_stage),
              "zip=%s stage=%s" % (os.path.exists(arc_path), os.path.isdir(arc_stage)))

        # 确认后逐张替换：宿主上每张都会挂在 mount 上失败 —— 要求"逐个汇报、不中断、
        # 不崩"，而不是整批 500。真机上失败的几张不能连累成功的那几张。
        st, j = c.api_post("/api/save_mgr/import_archive_finish?uid=0000000000000001", {})
        results = (j or {}).get("results", [])
        check("POST /api/save_mgr/import_archive_finish -> 逐张汇报（宿主上 2 张都 mount failed）",
              st == 200 and (j or {}).get("ok") and (j or {}).get("count") == 2
              and len(results) == 2
              and (j or {}).get("done") == 0 and (j or {}).get("failed") == 2
              and all(r.get("ok") is False and r.get("error") == "mount failed"
                      for r in results),
              "st=%s j=%s" % (st, j))
        # 有失败项 ⇒ 源 ZIP **保留**（暂存里那份解压结果已经删了，重试的唯一输入
        # 就是它）。判据和 PKG「装完自动清理、失败保留」同一条口径。
        # ⚠️ 反过来那条"全部成功 ⇒ 删源包"在宿主上到不了：没有 PS5 内核，每张都
        #    mount failed。那条分支由静态守卫 .build/chk_import_progress.py 记账。
        check("import_archive_finish 有失败项 ⇒ 源 ZIP 保留、暂存清空",
              os.path.exists(arc_path) and not os.path.isdir(arc_stage),
              "zip=%s stage=%s" % (os.path.exists(arc_path), os.path.isdir(arc_stage)))
        st, j = c.api_post("/api/save_mgr/import_archive_cancel", {})
        check("POST /api/save_mgr/import_archive_cancel -> ok + 暂存清空",
              j and j.get("ok"), "j=%s" % j)
        # 取消之后用户多半是去改个标题号再扫一遍 —— 源 ZIP 留着，不必重传。
        check("import_archive_cancel 也保留源 ZIP（取消后重扫不用重传）",
              os.path.exists(arc_path), "zip=%s" % os.path.exists(arc_path))

        # ★ 存活断言必须紧贴这条调用，不能指望后面某条请求"顺带"发现进程没了。
        # send_buffer() 是**接管所有权**的（MHD_RESPMEM_MUST_FREE）：传进去的
        # 必须是把命交出去的堆内存。json_ok() 曾经直接把**栈上的 buf** 递过去，
        # 于是 MHD 后来对一个栈地址 free()；glibc 在这里几乎不设防 —— 它把指针
        # 前面那个字当块头读，看着像就**把栈地址挂进 free 链表**然后正常返回，
        # 堆元数据当场烂掉。cancel 自己回 200 一切正常，崩溃落在**紧随其后**的
        # 请求上。所以这条断言分两半：cancel 对了 + 下一个请求还活着。
        # 静态面由 .build/chk_send_buffer.py 守（它能逐点判定栈/字面量/双重释放）。
        def alive(tag):
            st2, _d2 = c.get("/api/status")
            check("进程在这之后仍然响应（%s）" % tag, st2 == 200, "st=%s" % st2)

        alive("import_archive_cancel/json_ok")

        os.unlink(arc_path)

        # 反证：上一条断言能过，是因为解包器放宽了"类型位为 0"的判据 ——
        # 必须证明**真的软链条目依然被拒**，否则这次放宽就是拿安全换便利。
        # （POSIX symlink 条目一定带 S_IFLNK，放宽后仍然拦得住。）
        link_logical = "/data/Mango/incoming/_ht_link.zip"
        link_path = os.path.join(arc_dir, "_ht_link.zip")
        zi = zipfile.ZipInfo("PPSA23226/sdimg_Evil")
        zi.create_system = 3                      # Unix
        zi.external_attr = (0o120777 << 16)       # S_IFLNK
        with zipfile.ZipFile(link_path, "w") as z:
            z.writestr(zi, "/etc/passwd")
        st, j = c.api_post("/api/save_mgr/import_archive?path=" + link_logical, {})
        err = (j or {}).get("error", "")
        check("POST /api/save_mgr/import_archive 仍然拒绝软链条目（放宽判据的反证）",
              st == 400 and "-106" in err and ("link" in err or "special" in err),
              "st=%s err=%s" % (st, err))
        os.unlink(link_path)

        # ---- 15a-4. 其余 send_buffer 调用点（一整类从未被覆盖的端点） ----
        # 上面那条存活断言钉的是 json_ok()。这里把剩下几个"产生响应"的端点也
        # 走一遍：它们同样把结果交给 send_buffer，曾经一个回字面量、一个把栈
        # 缓冲递过去、还有一个调用方自己又 free 了一次（双重释放）。共同点是
        # **从来没有任何测试碰过它们** —— 没覆盖就没有守卫。每条后面都跟一次
        # 存活检查，因为失败模式是"下一个请求才死"。
        # /api/save_mgr/list：宿主分支回的是字面量，曾经直接把它交给 free()。
        st, j = c.api_post("/api/save_mgr/list", {})
        check("POST /api/save_mgr/list -> 200 且是合法 JSON",
              st == 200 and j is not None and "saves" in j, "st=%s j=%s" % (st, j))
        alive("save_mgr/list")

        # 未挂载时的冲突/错误路径（都不碰真存档，宿主机可测）。
        st, j = c.api_post("/api/save_mgr/files", {})
        check("POST /api/save_mgr/files 未挂载 -> 409 no save mounted",
              st == 409 and (j or {}).get("error") == "no save mounted",
              "st=%s j=%s" % (st, j))
        alive("save_mgr/files")
        st, j = c.api_post("/api/save_mgr/read", {"path": "param.sfo"})
        check("POST /api/save_mgr/read 未挂载 -> 409 no save mounted",
              st == 409 and (j or {}).get("error") == "no save mounted",
              "st=%s j=%s" % (st, j))
        alive("save_mgr/read")
        st, j = c.api_post("/api/save_mgr/unmount", {})
        check("POST /api/save_mgr/unmount 未挂载 -> 500 not mounted（json_ok 的另一半）",
              st == 500 and (j or {}).get("error") == "not mounted",
              "st=%s j=%s" % (st, j))
        alive("save_mgr/unmount")
        st, j = c.api_post("/api/save_mgr/mount", {"title_id": "X", "dir_name": "Y"})
        check("POST /api/save_mgr/mount 宿主上必然失败 -> 500 mount failed",
              st == 500 and (j or {}).get("error") == "mount failed",
              "st=%s j=%s" % (st, j))
        alive("save_mgr/mount")

        # ---- 15b. 存档树递归的深度上限（真机崩溃的兜底契约） ----
        # 40 层嵌套（> MANGO_SAVE_MAX_DEPTH=32）必须**干净失败**并让任务落到终态，
        # 而不是把进程带走。宿主栈大（8MB）复现不了 SIGSEGV，但能钉住
        # 「超深 ⇒ 干净失败 + 进程还活着」这个可观测契约。
        # ⚠️ 夹具**必须在这里现建现删**：放在 main() 的夹具区会给 savedata_prospero
        # 多出一个真标题，/api/save/scan 的既有断言（按标题集合比对）会全红。
        deep = os.path.join(save_root, "savedata_prospero", "DEEP00003")
        cur = deep
        for _i in range(40):
            cur = os.path.join(cur, "d%02d" % _i)
        os.makedirs(cur, exist_ok=True)
        with open(os.path.join(cur, "leaf.bin"), "wb") as f:
            f.write(b"DEEP-LEAF")
        try:
            st, j = c.api_post("/api/save/backup", {"title_id": "DEEP00003"})
            deep_id = (j or {}).get("task_id")
            check("POST /api/save/backup (40-level deep) routed", j and j.get("ok"),
                  "st=%s j=%s" % (st, j))
            deep_state = None
            polled_field_check = False
            if deep_id is not None:
                for _ in range(120):
                    st, pj = c.api_post("/api/install/poll", {"id": str(deep_id)})
                    if pj and pj.get("ok") and not polled_field_check:
                        polled_field_check = True
                        # 任务坞的速度/已用/剩余三格全指望这三个字段——漏了就是
                        # 恒 "—"（真机上传 PKG 只见字节不见速度的根因）。
                        check("install/poll carries speed/eta/created_at (dock needs them)",
                              all(k in pj for k in ("speed", "eta", "created_at")),
                              "keys=%s" % sorted(pj.keys()))
                    if pj and pj.get("ok") and pj.get("state_name") in (
                            "done", "failed", "cancelled"):
                        deep_state = pj
                        break
                    time.sleep(0.1)
            check("deep tree settles instead of hanging", deep_state is not None,
                  "id=%s" % (deep_id,))
            check("deep tree (40 > 32) -> clean failure, not a crash",
                  deep_state is not None and deep_state.get("state_name") == "failed",
                  "state=%s" % (deep_state or {}).get("state_name"))
            # 进程必须还活着：再打一个必然成功的路由。进程被带走了这里会连接失败。
            st, j = c.api_post("/api/save/list", {"title_id": "CUSA00000"})
            check("process still alive after deep-tree backup", j and j.get("ok"),
                  "st=%s j=%s" % (st, j))
        finally:
            shutil.rmtree(deep, ignore_errors=True)
        # 正常存档不受影响（深度上限不能误伤浅树）
        st, j = c.api_post("/api/save/backup", {"title_id": "GAME00002"})
        ok_id = (j or {}).get("task_id")
        ok_state = None
        if ok_id is not None:
            for _ in range(120):
                st, pj = c.api_post("/api/install/poll", {"id": str(ok_id)})
                if pj and pj.get("ok") and pj.get("state_name") in (
                        "done", "failed", "cancelled"):
                    ok_state = pj
                    break
                time.sleep(0.1)
        check("normal save still backs up (guard not over-eager)",
              ok_state is not None and ok_state.get("state_name") == "done",
              "state=%s" % (ok_state or {}).get("state_name"))
        st, j = c.api_post("/api/pkg/scan", {"root": ROOT})
        check("POST /api/pkg/scan ok (empty)", j and j.get("ok"), "j=%s" % j)
        # 根目录打不开必须上报（unreadable），不能静默当成"没有 PKG"——
        # 真机上 /data/pkg 大小写拼错就是这种形态，之前表现为「扫描不到」。
        st, j = c.api_post("/api/pkg/scan", {"root": ROOT + "/definitely-missing"})
        check("pkg/scan reports an unreadable root instead of a silent empty set",
              j and j.get("ok")
              and (j.get("unreadable") or []) == [ROOT + "/definitely-missing"],
              "j=%s" % j)
        st, j = c.api_post("/api/app/register", {})
        # PS5-only; on the host it must be a clean, routed error (never 404/405).
        check("POST /api/app/register routed (PS5-only on host)",
              st == 501 and j and not j.get("ok"), "st=%s j=%s" % (st, j))
        # ---- fetch: a REAL NAS pull (it was a 501 stub until 2026-10-01) ----
        st, j = c.api_post("/api/fetch", {"src": "127.0.0.1/share/x.pkg",
                                          "dst": ROOT, "scheme": "ftp"})
        check("POST /api/fetch with a backend we do not have -> 501",
              st == 501 and j and not j.get("ok")
              and j.get("error_code") == "fetch_unsupported", "st=%s j=%s" % (st, j))

        st, j = c.api_post("/api/fetch", {"src": "127.0.0.1/share/game.pkg",
                                          "dst": ROOT, "scheme": "smb"})
        fetch_id = (j or {}).get("task_id")
        check("POST /api/fetch queues a real pull task", j and j.get("ok")
              and fetch_id is not None, "st=%s j=%s" % (st, j))
        if fetch_id is not None:
            # Nothing listens on 127.0.0.1:445 here, so the worker must fail FAST
            # and settle. A queued task that never terminates would wedge the
            # single-active-task queue and block every later operation.
            # /api/install/poll is used (not /api/tasks) because it looks a task up
            # by id directly — /api/tasks removes finished rows on the next call.
            settled = None
            for _ in range(80):
                st, pj = c.api_post("/api/install/poll", {"id": str(fetch_id)})
                if pj and pj.get("ok") and pj.get("state_name") in (
                        "done", "failed", "cancelled"):
                    settled = pj
                    break
                time.sleep(0.25)
            check("fetch task settles (fails) instead of hanging",
                  settled is not None and settled.get("state_name") == "failed",
                  "settled=%s" % settled)
        c.wait_idle()

        # ---- 15a-bis. 预取流水线：真跑一次（v1.0.18）------------------------
        # NAS→PS5 只有 10MB/s 的修法是「读线程/写盘重叠」，但流水线只在 ≥32MB 时
        # 启用，小文件仍走旧串行路 —— 于是这条新代码是仓库里**唯一一条宿主测试
        # 默认覆盖不到**的路径（真机以外的全部用例都是小文件）。local:// scheme
        # 让宿主也能做一次真拉取：源是本地文件，但走的是与 /api/nas/copy 完全
        # 同一条 nas_pull_file，因此流水线、续传、取消语义一并被测到。
        def _pull(src_path, dst_path, label, tries=480):
            st, j = c.api_post("/api/fetch", {"src": src_path, "dst": dst_path,
                                              "scheme": "local"})
            tid = (j or {}).get("task_id")
            check("fetch(local) queues %s" % label,
                  st == 200 and bool(j and j.get("ok")) and tid is not None,
                  "st=%s j=%s" % (st, j))
            if tid is None:
                return None
            for _ in range(tries):
                st, pj = c.api_post("/api/install/poll", {"id": str(tid)})
                if pj and pj.get("ok") and pj.get("state_name") in (
                        "done", "failed", "cancelled"):
                    return pj
                time.sleep(0.25)
            return {"state_name": "hung"}

        def _sha(path):
            h = hashlib.sha256()
            with open(path, "rb") as f:
                for blk in iter(lambda: f.read(1 << 20), b""):
                    h.update(blk)
            return h.hexdigest()

        nas_src_dir = os.path.join(ROOT, "nassrc")
        nas_dst_dir = os.path.join(ROOT, "nasdst")
        os.makedirs(nas_src_dir, exist_ok=True)
        os.makedirs(nas_dst_dir, exist_ok=True)
        small_src = os.path.join(nas_src_dir, "small.bin")
        big_src = os.path.join(nas_src_dir, "big.bin")
        with open(small_src, "wb") as f:
            f.write(b"MANGO-SMALL" * 90000)          # ~1 MB，走旧串行路
        BIG = 34 * 1024 * 1024                      # > 32MB 阈值，走流水线
        with open(big_src, "wb") as f:
            f.write(b"MANGO-PIPE!" * (BIG // 10))
        small_sha = _sha(small_src)
        big_sha = _sha(big_src)

        r = _pull(small_src, os.path.join(nas_dst_dir, "small.bin"), "small")
        check("small pull (serial path) completes",
              bool(r) and r.get("state_name") == "done", "r=%s" % r)
        if r and r.get("state_name") == "done":
            check("small pull is byte-identical",
                  _sha(os.path.join(nas_dst_dir, "small.bin")) == small_sha,
                  "sha mismatch")
        c.wait_idle()
        r = _pull(big_src, os.path.join(nas_dst_dir, "big.bin"), "big")
        check("big pull (>=32MB, prefetch pipeline) completes",
              bool(r) and r.get("state_name") == "done", "r=%s" % r)
        if r and r.get("state_name") == "done":
            check("big pull is byte-identical (pipeline loses no bytes)",
                  _sha(os.path.join(nas_dst_dir, "big.bin")) == big_sha,
                  "sha mismatch")

        # 静态把关：阈值必须真的低于上面那个用例。
        # 否则哪天把阈值抬到 4GB，「大文件走流水线」的用例就悄悄退化成串行路，
        # 运行时断言全绿而流水线本身一行没被测到 —— 正是本项目反复踩的假绿。
        with open(os.path.join(WORKDIR, "src", "mango_port.c"),
                  encoding="utf-8") as f:
            nx_src_bis = f.read()
        m = re.search(r"#define\s+NAS_PULL_PIPELINE_MIN\s+\((\d+)ull\s*\*\s*"
                      r"(\d+)\s*\*\s*(\d+)\)", nx_src_bis)
        thr = int(m.group(1)) * int(m.group(2)) * int(m.group(3)) if m else -1
        check("the >=32MB test file really takes the pipeline path",
              m is not None and 0 < thr <= BIG,
              "threshold=%s BIG=%s" % (thr, BIG))
        c.wait_idle()

        # ---- 15a-ter. /api/nas/delete -------------------------------------
        # 真机删除「点了没反应」的根因是：删除走纯 POSIX unlink/rmdir，而 NAS
        # 路径在本机是不存在的相对路径（ENOENT），任务却汇报成功。远端删除必须
        # 经 /api/nas/delete 走 SMB 的 rmdir/unlink。
        # 这里没有 NAS（fixture 要 root 起 smbd），所以只验**失败路径**：
        # 参数校验、真原因回传、以及不会把 payload 吊死。成功路径留给真机。
        st, j = c.api_post("/api/nas/delete", {"paths": ""})
        check("POST /api/nas/delete empty paths -> 400",
              st == 400 and j and not j.get("ok"), "st=%s j=%s" % (st, j))
        st, j = c.api_post("/api/nas/delete", {"paths": "   \n  "})
        check("POST /api/nas/delete blank-only paths -> 400",
              st == 400 and j and not j.get("ok"), "st=%s j=%s" % (st, j))
        st, j = c.api_post("/api/nas/delete", {"scheme": "bogus",
                                               "paths": "h/s/x"})
        check("POST /api/nas/delete unknown scheme -> not implemented",
              st == 501 and j and not j.get("ok"), "st=%s j=%s" % (st, j))
        # 连不上的地址：必须入队 → 失败（带真原因），不能成功也不能挂住。
        st, j = c.api_post("/api/nas/delete",
                           {"scheme": "smb",
                            "paths": "127.0.0.1:1/noshare/nosuchfile"})
        nd_tid = (j or {}).get("task_id")
        check("POST /api/nas/delete unreachable -> queued",
              st == 200 and bool(j and j.get("ok")) and nd_tid is not None,
              "st=%s j=%s" % (st, j))
        if nd_tid is not None:
            nd_state = "hung"
            nd_err = ""
            nd_pj = None
            for _ in range(240):
                st, pj = c.api_post("/api/install/poll", {"id": str(nd_tid)})
                if pj and pj.get("ok") and pj.get("state_name") in (
                        "done", "failed", "cancelled"):
                    nd_state = pj.get("state_name")
                    # 失败原因落在 note 上（error 是这个端点的另一回事），
                    # 读错字段就会拿到空串 —— 明明有原因却判成「未知」。
                    nd_err = pj.get("note") or pj.get("error") or ""
                    nd_pj = pj
                    break
                time.sleep(0.25)
            check("nas delete of an unreachable share ends in failed "
                  "(never silently 'done')",
                  nd_state == "failed", "state=%s err=%r" % (nd_state, nd_err))
            check("nas delete failure carries a real reason (not 'unknown')",
                  bool(nd_err) and nd_err != "删除中断",
                  "err=%r pj=%s" % (nd_err, nd_pj))
        c.wait_idle()
        shutil.rmtree(nas_src_dir, ignore_errors=True)
        shutil.rmtree(nas_dst_dir, ignore_errors=True)
        st, j = c.api_post("/api/install/poll", {"id": "99999"})
        check("POST /api/install/poll unknown id -> not found json",
              st == 404 and j and not j.get("ok"), "st=%s j=%s" % (st, j))
        st, j = c.api_json("/api/task/cancel", {"id": 99999})
        check("POST /api/task/cancel unknown id -> error json", j and not j.get("ok"),
              "st=%s j=%s" % (st, j))

        # ---- 15b. status returns a real address ----
        st, j = c.get_json("/api/status")
        check("status reports a non-empty addr", j and j.get("addr"),
              "addr=%r" % (j.get("addr") if j else None))

        # ---- 15c. PKG library scan / enqueue / poll ----
        pkgdir = os.path.join(ROOT, "pkgs")
        os.makedirs(os.path.join(pkgdir, "deep"), exist_ok=True)
        with open(os.path.join(pkgdir, "game.pkg"), "wb") as f:
            f.write(b"not-a-real-pkg")           # meta parse fails -> stem fallback
        with open(os.path.join(pkgdir, "deep", "image.ffpkg"), "wb") as f:
            f.write(b"FPKG-IMAGE-BYTES")

        st, j = c.api_post("/api/pkg/scan", {"root": pkgdir})
        pkgs = (j or {}).get("packages", [])
        by_path = {p["path"]: p for p in pkgs}
        gp = by_path.get(os.path.join(pkgdir, "game.pkg"))
        ip = by_path.get(os.path.join(pkgdir, "deep", "image.ffpkg"))
        check("pkg/scan finds .pkg recursively", j and j.get("ok") and gp is not None,
              "j=%s" % j)
        check("pkg/scan labels .pkg by stem fallback", gp is not None
              and gp.get("title") == "game" and gp.get("is_fpkg") is False,
              "gp=%s" % gp)
        check("pkg/scan marks .ffpkg as fPKG image", ip is not None
              and ip.get("is_fpkg") is True and ip.get("title") == "image",
              "ip=%s" % ip)

        # ---- 15c-bis. app 根扫描：SMB 装好的游戏要出现在 PKG 页 ----
        # 真机 2026-10-22 报障（用户原话）：「我通过 SMB 安装好的 PKG 游戏，在
        # PKG 管理页面不显示，但是我已经可以玩了」。根因：PKG 页只扫 .pkg 文件，
        # SMB 流式安装不在本机留镜像，游戏装在 app 目录，六个文件根一个都扫不到。
        # 修法：3 个 app 根（/user/app、/mnt/ext1/user/app、/data/app）并入扫描，
        # 扫的是「装好的游戏目录」而不是文件。字段集与 .pkg 条目一致（前端 libCard
        # 只认一套），差别全在语义：size=0、is_fpkg=false、app_dir=true。
        st, j = c.api_post("/api/pkg/scan", {"root": "/user/app"})
        apps = (j or {}).get("packages", []) if j else []
        by_tid = {}
        for p in apps:
            by_tid[p.get("title_id")] = p
        pa = by_tid.get("PPSA01234")
        ca = by_tid.get("CUSA09999")
        check("app root scan: ok + found the installed game dirs",
              st == 200 and j and j.get("ok")
              and pa is not None and ca is not None,
              "st=%s j=%s" % (st, j))
        check("app root entry flagged app_dir/installed, never is_fpkg",
              pa and pa.get("app_dir") is True and pa.get("installed") is True
              and pa.get("is_fpkg") is False and pa.get("size") == 0,
              "pa=%s" % pa)
        check("app root entry takes its title from app.db",
              pa and pa.get("title") == "测试游戏甲", "pa=%s" % pa)
        check("app root entry with no app.db row falls back to title_id",
              ca and ca.get("title") == "CUSA09999", "ca=%s" % ca)
        check("app root entry reports the CONSOLE path, not the host fixture path",
              pa and pa.get("path") == "/user/app/PPSA01234",
              "pa=%s" % pa)
        check("app root scan skips non-title prefixes (NPXS30000/notagame)",
              "NPXS30000" not in by_tid and "notagame" not in by_tid,
              "tids=%s" % sorted(by_tid))
        check("app root scan skips a same-named FILE (S_ISDIR gate)",
              "PPSA05555" not in by_tid, "tids=%s" % sorted(by_tid))
        # 宿主只映射 /user/app：另两个 app 根必然 opendir 失败，必须进 unreadable
        # 而不是假装扫过了（真机上 M.2 没插、USB 没挂也是这个形状，前端靠它提示
        # 用户，而不是显示一个空的「没有游戏」）。
        st, j2 = c.api_post("/api/pkg/scan", {"root": "/mnt/ext1/user/app"})
        unr2 = (j2 or {}).get("unreadable") or []
        check("unmapped app root reports unreadable (never a silent empty list)",
              st == 200 and j2 and j2.get("ok")
              and "/mnt/ext1/user/app" in unr2
              and not (j2 or {}).get("packages"),
              "st=%s j2=%s" % (st, j2))
        st, j3 = c.api_post("/api/pkg/scan", {"root": "/data/app"})
        unr3 = (j3 or {}).get("unreadable") or []
        check("/data/app also reports unreadable on host",
              st == 200 and j3 and j3.get("ok") and "/data/app" in unr3
              and not (j3 or {}).get("packages"),
              "st=%s j3=%s" % (st, j3))

        st, j = c.api_post("/api/pkg/enqueue", {"path": ip["path"] if ip else "",
                                                "title": "image", "version": ""})
        tid = (j or {}).get("task_id")
        check("pkg/enqueue (fPKG) returns task_id", j and j.get("ok") and tid is not None,
              "j=%s" % j)
        # poll until terminal, then confirm the image landed in homebrew
        polled = None
        for _ in range(60):
            st, pj = c.api_post("/api/install/poll", {"id": str(tid)})
            if not pj or not pj.get("ok"):
                break
            polled = pj
            if pj.get("state_name") in ("done", "failed", "cancelled"):
                break
            time.sleep(0.2)
        check("install/poll reports state_name/progress/total",
              polled and polled.get("state_name") and "progress" in polled
              and "total" in polled, "polled=%s" % polled)
        check("install/poll reached done", polled and polled.get("state_name") == "done",
              "polled=%s" % polled)
        check("fPKG image copied into homebrew",
              os.path.exists(os.path.join(brew_dir, "image.ffpkg")))
        c.wait_idle()

        # ---- 15d. /api/pkg/install-url (PC 网络直装入队) ----
        st, j = c.api_post("/api/pkg/install-url", {"url": "ftp://pc/game.pkg"})
        check("pkg/install-url rejects non-http scheme (400)",
              st == 400 and j and j.get("error_code") == "pkg_url_invalid",
              "st=%s j=%s" % (st, j))
        st, j = c.api_post("/api/pkg/install-url", {"url": "http://"})
        check("pkg/install-url rejects empty url (400)",
              st == 400 and j and j.get("error_code") == "pkg_url_invalid",
              "st=%s j=%s" % (st, j))
        st, j = c.api_post("/api/pkg/install-url",
                           {"url": "http://" + "9" * 520 + "/x.pkg"})
        check("pkg/install-url rejects oversized url (400)",
              st == 400 and j and j.get("error_code") == "pkg_url_invalid",
              "st=%s j=%s" % (st, j))
        st, j = c.api_json("/api/pkg/install-url",
                           {"url": "http://192.168.1.10:9898/pkg/game.pkg"})
        # 宿主（__linux__）没有安装层 ⇒ 501；真机才会入队返回 task_id
        check("pkg/install-url valid url -> 501 on host (pkg_install_unsupported)",
              st == 501 and j and j.get("error_code") == "pkg_install_unsupported",
              "st=%s j=%s" % (st, j))
        st, _ = c.raw("GET", "/api/pkg/install-url")
        check("pkg/install-url GET -> 405", st == 405, "st=%s" % st)

        # ---- 15e. /api/pkg/uninstall（已安装卡片上的「卸载」）----
        # 只收 TITLE_ID，绝不收路径：交给系统安装器（sceAppInstUtilAppUnInstall）
        # 的东西必须是一个标题号，前端也不该有能力让内核去删任意路径。
        st, j = c.api_post("/api/pkg/uninstall", {"title_id": "PPSA23226"})
        check("pkg/uninstall valid title -> 501 unsupported on host (never a fake 200)",
              st == 501 and j and j.get("error_code") == "pkg_uninstall_unsupported",
              "st=%s j=%s" % (st, j))
        st, j = c.api_post("/api/pkg/uninstall", {})
        check("pkg/uninstall without title_id -> 400",
              st == 400 and not (j and j.get("ok")), "st=%s j=%s" % (st, j))
        st, j = c.api_post("/api/pkg/uninstall", {"title_id": "ppsa23226"})
        check("pkg/uninstall lower-case title -> 400（标题号是 4 大写字母 + 5 位数字）",
              st == 400 and not (j and j.get("ok")), "st=%s j=%s" % (st, j))
        st, j = c.api_post("/api/pkg/uninstall",
                           {"title_id": "/data/Mango/incoming/_ht_arc.zip"})
        check("pkg/uninstall a PATH instead of a title id -> 400（绝不让路径递交内核）",
              st == 400 and not (j and j.get("ok")), "st=%s j=%s" % (st, j))
        st, _ = c.raw("GET", "/api/pkg/uninstall")
        check("pkg/uninstall GET -> 405", st == 405, "st=%s" % st)

        # ---- 15f. /api/pkg/enqueue?stream=1（本地流式投递，opt-in）----
        # 宿主没有安装层 ⇒ 两条投递路都回 501。这组断言钉住的是「新开关被接受、
        # 且既有的校验与口径一点没变」：流式发不出去必须回落 path delivery，
        # 绝不能变成 400 把用户的安装请求挡在门外。
        if gp:
            st, j = c.api_post("/api/pkg/enqueue",
                               {"path": gp["path"], "stream": "1"})
            check("pkg/enqueue stream=1 -> 501 on host（开关被接受，口径不变）",
                  st == 501 and j and j.get("error_code") == "pkg_install_unsupported",
                  "st=%s j=%s" % (st, j))
            st, j = c.api_post("/api/pkg/enqueue",
                               {"path": gp["path"], "stream": "0"})
            check("pkg/enqueue stream=0 仍是默认 path delivery（501 on host）",
                  st == 501 and j and j.get("error_code") == "pkg_install_unsupported",
                  "st=%s j=%s" % (st, j))
            st, j = c.api_post("/api/pkg/enqueue",
                               {"path": gp["path"] + ".missing", "stream": "1"})
            check("pkg/enqueue stream=1 对不存在的文件仍 404（开关不绕过校验）",
                  st == 404, "st=%s j=%s" % (st, j))

        # ---- 15g. NAS 直装（scheme 入参 → 拼 URL → 强制流式）----
        # 宿主没有安装层 ⇒ 拼好的 URL 进队列后仍然 501；断言钉住的是
        # 「带 scheme 的请求不再被 stat() 当本地路径拒掉」，坏地址在真机上
        # 才由 pkg_stream_prepare_nas 的首字节探测变成任务失败。
        if gp:
            st, j = c.api_post("/api/pkg/enqueue",
                               {"path": "192.0.2.1/share/game.pkg",
                                "scheme": "smb", "user": "u", "pass": "p"})
            check("pkg/enqueue 带 scheme 的 NAS 路径不再 404（走流式队列，501 on host）",
                  st == 501 and j and j.get("error_code") == "pkg_install_unsupported",
                  "st=%s j=%s" % (st, j))
            st, j = c.api_post("/api/pkg/enqueue",
                               {"path": "/share/game.pkg", "scheme": "smb"})
            check("pkg/enqueue scheme+无主机 -> 400（拼不出合法 NAS 地址）",
                  st == 400 and j and j.get("error_code") == "pkg_url_invalid",
                  "st=%s j=%s" % (st, j))
            st, j = c.api_post("/api/pkg/info",
                               {"path": "192.0.2.1/share/game.pkg",
                                "scheme": "nosuch"})
            check("pkg/info 未知 scheme 不再裸 open()（报错而非 200）",
                  st in (400, 404, 501), "st=%s j=%s" % (st, j))

        # ---- 15d. save domain: list / backup / restore ----
        st, j = c.api_post("/api/save/list", {"title_id": title})
        snaps = (j or {}).get("snapshots", [])
        check("save/list returns existing snapshots", j and j.get("ok") and "111" in snaps,
              "j=%s" % j)

        st, j = c.api_post("/api/save/backup", {"title_id": title})
        bk_id = (j or {}).get("task_id")
        check("save/backup returns task_id", j and j.get("ok") and bk_id is not None,
              "j=%s" % j)
        c.wait_idle()
        st, j = c.api_post("/api/save/list", {"title_id": title})
        snaps2 = (j or {}).get("snapshots", [])
        new_ids = [s for s in snaps2 if s not in snaps]
        check("save/backup created a new snapshot", len(new_ids) == 1,
              "before=%s after=%s" % (snaps, snaps2))
        if new_ids:
            snap_file = os.path.join(snap_dir, title, new_ids[0], "sd.bin")
            ok_snap = False
            try:
                with open(snap_file, "rb") as f:
                    ok_snap = f.read() == b"LIVE-V2"
            except Exception:
                pass
            check("snapshot content equals the live save", ok_snap)

        # restore the OLD snapshot over the live dir, then verify the live bytes went back
        st, j = c.api_post("/api/save/restore", {"title_id": title, "snapshot": "111"})
        rs_id = (j or {}).get("task_id")
        check("save/restore returns task_id", j and j.get("ok") and rs_id is not None,
              "j=%s" % j)
        c.wait_idle()
        live_file = os.path.join(save_root, "savedata_prospero", title, "sd.bin")
        ok_restore = False
        try:
            with open(live_file, "rb") as f:
                ok_restore = f.read() == b"OLD-V1"
        except Exception:
            pass
        check("restore wrote the snapshot back over the live save", ok_restore)

        # backup of an unknown title is a clean 404, not a hang
        st, j = c.api_post("/api/save/backup", {"title_id": "CUSA99999"})
        check("save/backup unknown title -> not found", st == 404 and j
              and not j.get("ok"), "st=%s j=%s" % (st, j))

        # save/list must answer BOTH questions: "is there a save here" and
        # "is there a snapshot". Only reporting snapshots made a console that
        # had never been backed up look like it had no save at all — the user
        # could not tell "no save" from "not backed up yet", and a manually
        # typed title id looked identical to a typo.
        st, j = c.api_post("/api/save/list", {"title_id": title})
        check("save/list reports save_found=true for a title with live data",
              j and j.get("ok") and j.get("save_found") is True
              and "savedata_prospero" in (j.get("save_path") or ""),
              "j=%s" % j)
        check("save/list reports a non-zero save_size",
              bool(j and (j.get("save_size") or 0) > 0), "size=%s" % (j or {}).get("save_size"))
        st, j = c.api_post("/api/save/list", {"title_id": "CUSA99999"})
        check("save/list reports save_found=false for an absent title",
              j and j.get("ok") and j.get("save_found") is False, "j=%s" % j)

        # ---- 15d-bis. /api/save/scan: what saves live on this console ----
        # 这是存档页原本缺的那个端点：它列的是**存档**，不是快照。
        st, j = c.get_json("/api/save/scan")
        saves = (j or {}).get("saves", [])
        # GAME00002 同时出现在 data 与 meta 两个叶下 ⇒ 去掉重复的 title id
        ids = sorted(set(s.get("title_id") for s in saves))
        check("POST /api/save/scan lists every title with save data",
              st == 200 and j and j.get("ok") and ids == ["CUSA00001", "GAME00002"],
              "st=%s ids=%s j=%s" % (st, ids, j))
        kinds = sorted(set(s.get("kind") for s in saves))
        check("save/scan covers both leaves (data + meta)",
              kinds == ["savedata_prospero", "savedata_prospero_meta"],
              "kinds=%s" % kinds)
        one = [s for s in saves if s.get("title_id") == title]
        check("save/scan entry carries path + size + mtime",
              bool(one and "savedata_prospero" in one[0].get("path", "")
                   and (one[0].get("size") or 0) > 0 and (one[0].get("mtime") or 0) > 0),
              "first=%s" % (one[0] if one else None))
        # 同一台机器上「扫得到」和「查得到」必须一致 —— 两处曾经用不同的层级顺序，
        # 结果一个说有、一个说没有（而真机只有 leaf 在前那一种排法）。
        scanned = set(ids)
        found_by_list = set()
        for cand in ("CUSA00001", "GAME00002"):
            st, j2 = c.api_post("/api/save/list", {"title_id": cand})
            if j2 and j2.get("save_found"):
                found_by_list.add(cand)
        check("save/scan and save/list agree on which saves exist",
              scanned == found_by_list,
              "scan=%s list=%s" % (sorted(scanned), sorted(found_by_list)))

        # ---- 15d-ter. save/scan 的诊断字段 ----
        # 用户报⑤：「存档管理还是显示不出我 PS5 中的游戏存档 … 为什么在存档页面
        # 不显示，扫描不到他的存档」。根因是**查询路径漏了提权**：/user/home/<账号>
        # 属存档域，没有存档 AuthID（0x4800000000000010）时 opendir 直接被拒 ⇒
        # 扫描恒返回空，而 backup/restore 因为先调了 save_escalate() 却是好的
        # （同一个存档「备份能备份、查询说没有」）。宿主上 save_escalate() 走
        # #ifdef __linux__ 的 return 0，所以 escalated 恒 true —— 这条钉的是
        # 「字段存在且口径正确」，真机上的提权本身只能上机验。
        #
        # 更要紧的是 roots：页面拿它把「提权失败 / 没有账号目录 / 有账号但没存档」
        # 三件**完全不同的事**分开说。原来它们是同一句「没找到任何存档目录」，
        # 用户除了「扫不到」什么信息都得不到。
        st, j = c.get_json("/api/save/scan")
        check("save/scan reports an escalation state",
              j and j.get("ok") and "escalated" in j, "j=%s" % j)
        check("save/scan escalated=true on the host (nothing to escalate to)",
              j and j.get("escalated") is True, "j=%s" % j)
        roots = (j or {}).get("roots", [])
        check("save/scan returns per-root diagnostics",
              isinstance(roots, list) and len(roots) > 0, "roots=%s" % roots)
        leaf_names = set()
        for r in roots:
            leaf_names.add((r.get("root") or "").rstrip("/").rsplit("/", 1)[-1])
        check("save/scan diagnostics cover both the PS5 and the PS4 leaf names",
              {"savedata_prospero", "savedata_prospero_meta",
               "savedata", "savedata_meta"} <= leaf_names,
              "leaf_names=%s" % sorted(leaf_names))
        check("save/scan diagnostics show which root actually yielded titles",
              any((r.get("opened") and (r.get("titles") or 0) > 0) for r in roots),
              "roots=%s" % roots)
        # 夹具里 WFM_SAVE_ROOT 扮演「一个账号的 home」，所以叶在它下面一级；
        # 若有人把两级写反（home 与 leaf 互换），这里会立刻指出来。
        check("save/scan diagnostics point at the fixture's leaf, not the home root",
              any((r.get("root") or "").endswith("savedata_prospero") and r.get("opened")
                  for r in roots), "roots=%s" % roots)

        # ---- 15d-quater. 存档扫描的形状过滤：不是 TITLE_ID 的条目不算「游戏」 ----
        # 用户截图圈掉的三行：sce_backupN（0 B · savedata）、user（·meta）——
        # 存档叶里本来就住着系统备份槽/账号树，用户把列表读成「我的游戏」，
        # 这些杂项就是噪音。server 端按「4 大写字母 + 5 数字」过滤。
        # ★ 夹具先放杂项进去：过滤器被摘掉时 ids 断言会直接变红。
        os.makedirs(os.path.join(save_root, "savedata", "sce_backup4"), exist_ok=True)
        os.makedirs(os.path.join(save_root, "savedata_meta", "user"), exist_ok=True)
        with open(os.path.join(save_root, "savedata", "sce_backup4", "x.bin"), "wb") as f:
            f.write(b"")
        st, j = c.get_json("/api/save/scan")
        ids_f = sorted(set(s.get("title_id") for s in (j or {}).get("saves", [])))
        check("save/scan filters non-title entries (sce_backupN / user are noise)",
              st == 200 and ids_f == ["CUSA00001", "GAME00002"],
              "ids=%s" % ids_f)

        # ---- 15d-quinquies. 存档删除（快照删除 + 本机存档删除） ----
        # 用户点名「这个存档页面没有增删改存档的功能只有个备份这不行」。
        # ★ 契约（2026-10-04 用户拍板）：删除本机存档**不强制先备份** ——
        #   「可以直接删除也可以备份」，两条路都通。内联 copy_tree 已移除
        #   （真机六连崩根因：日志停在 SNAPROOT-READY、SNAPSHOT-OK 永不出现 ⇒
        #   copy_tree 复制进 /data/savesnap 时进程被内核带走）。安全网 = 独立
        #   「备份」按钮 + 前端 confirm 提示，选择权在用户。
        # 先验证无快照也能删：DELTEST01 没有→直接删成功。
        st, j = c.api_post("/api/save/delete", {"title_id": "DELTEST01"})
        check("save/delete works WITHOUT a pre-existing snapshot (no hard gate)",
              j and j.get("ok") and (j.get("task_id") or 0) > 0,
              "st=%s j=%s" % (st, j))
        c.wait_idle()
        check("save/delete wiped DELTEST01 bytes",
              not os.path.exists(os.path.join(save_root, "savedata_prospero",
                                              "DELTEST01", "d.bin")))
        live_file = os.path.join(save_root, "savedata_prospero", title, "sd.bin")
        st, j = c.api_post("/api/save/snapdelete",
                           {"title_id": title, "snapshot": "../evil"})
        check("snapdelete rejects non-digit snapshot names (no traversal)",
              st == 400 and j and not j.get("ok"), "st=%s j=%s" % (st, j))
        st, j = c.api_post("/api/save/snapdelete",
                           {"title_id": title, "snapshot": "111"})
        check("save/snapdelete queues a task", j and j.get("ok")
              and (j.get("task_id") or 0) > 0, "j=%s" % j)
        c.wait_idle()
        st, j = c.api_post("/api/save/list", {"title_id": title})
        check("deleted snapshot is gone from the list",
              j and j.get("ok") and "111" not in (j.get("snapshots") or []),
              "j=%s" % j)
        check("live save is untouched by a snapshot delete",
              os.path.exists(live_file), "live=%s" % os.path.exists(live_file))

        # ---- 批量快照删除（用户要求「快照要能多选删除」） ----
        # 再造两份快照，用 snapshots 逗号列表一次删两份。注意别误删原始备份
        # （后面的 rescue 测试要用它）—— 先记下已有列表，只删新建的两份。
        st, j = c.api_post("/api/save/list", {"title_id": title})
        snaps_old = set((j or {}).get("snapshots") or [])
        st, jb1 = c.api_post("/api/save/backup", {"title_id": title})
        c.wait_idle()
        st, jb2 = c.api_post("/api/save/backup", {"title_id": title})
        c.wait_idle()
        st, j = c.api_post("/api/save/list", {"title_id": title})
        snaps_mid = (j or {}).get("snapshots") or []
        new_two = [s for s in snaps_mid if s not in snaps_old]
        check("two more snapshots created for batch delete",
              len(new_two) == 2, "old=%s mid=%s new=%s" % (snaps_old, snaps_mid, new_two))
        if len(new_two) >= 2:
            batch = ",".join(new_two[:2])
            st, j = c.api_post("/api/save/snapdelete",
                               {"title_id": title, "snapshots": batch})
            check("snapdelete batch (snapshots=csv) queues a task",
                  j and j.get("ok") and (j.get("task_id") or 0) > 0,
                  "st=%s j=%s" % (st, j))
            c.wait_idle()
            st, j = c.api_post("/api/save/list", {"title_id": title})
            snaps_after = (j or {}).get("snapshots") or []
            check("snapdelete batch removed both new snapshots",
                  all(s not in snaps_after for s in new_two[:2]),
                  "new=%s after=%s" % (new_two, snaps_after))
            check("snapdelete batch left the original backup intact",
                  snaps_old <= set(snaps_after),
                  "old=%s after=%s" % (snaps_old, snaps_after))

        # ★ 提权基线：见下面「契约①」——要证明 worker 自己提权，得先知道
        #   这次删除之前计数是什么。
        _st0, _j0 = c.get_json("/api/save-domain")
        dom_before = (_j0 or {}).get("enters") or 0
        st, j = c.api_post("/api/save/delete", {"title_id": title})
        check("save/delete queues a task", j and j.get("ok")
              and (j.get("task_id") or 0) > 0, "j=%s" % j)
        c.wait_idle()
        # ⚠️ 必须紧跟 wait_idle，中间不能再夹任何会提权的请求：本用例后面还要
        #    调 /api/save/scan，而 scan 自己也要提权 —— 晚采一步就 +1，正好把
        #    「worker 漏提权」的 delta=1 补成 2，断言假绿（2026-10-16 反证实测）。
        _st1, _j1 = c.get_json("/api/save-domain")
        dom_after = (_j1 or {}).get("enters") or 0
        # ★ 新契约（真机两轮崩溃后的修复）：TITLE_ID 叶目录由内核 save daemon
        #   管理，删除 worker **只清空内容、保留目录本身**（rmdir 它是进程消失
        #   的头号嫌疑）。断言拆成三半：字节没了 / 目录还在 / 目录是空的。
        #   keep_top 被改坏（重新 rmdir）⇒ 第一条红；没清空 ⇒ 后两条红。
        live_dir = os.path.join(save_root, "savedata_prospero", title)
        check("save/delete wipes the live save's BYTES", not os.path.exists(
            os.path.join(live_dir, "sd.bin")))
        check("save/delete KEEPS the title dir (never rmdir the kernel-managed leaf)",
              os.path.isdir(live_dir))
        # Directory may be GONE when the keep-top guarantee is broken — never
        # let that turn into an unhandled FileNotFoundError that kills the
        # whole suite (it would mask every later assertion).
        entries = (os.listdir(live_dir) if os.path.isdir(live_dir)
                   else ["<DIR GONE>"])
        check("save/delete leaves the kept dir EMPTY", entries == [],
              "entries=%s" % entries)
        st, j = c.get_json("/api/save/scan")
        ids_after = sorted(set(s.get("title_id") for s in (j or {}).get("saves", [])))
        check("save/scan stops listing the emptied title (probe agrees with scan)",
              title not in ids_after, "ids=%s" % ids_after)
        # 阶段日志：真机上进程被杀没有 stderr，这份 append-only 日志是唯一的
        # 取证手段。宿主上断言「每个阶段都真的落盘」——日志函数坏了（打不开
        # 就静默返回）会让这条红，而不是真机崩了才发现什么都查不到。
        dlog = os.path.join(WORKDIR, "save-delete.log")
        # Log function dead (or /data unwritable) => NO file at all. Read that
        # as "" so every phase assertion goes red here instead of crashing the
        # suite with FileNotFoundError.
        log_text = ""
        try:
            with open(dlog, "r", errors="replace") as f:
                log_text = f.read()
        except FileNotFoundError:
            pass
        for phase in ("ESCALATE rc=0", "RM-BEGIN", "delete DONE"):
            check("delete log records phase <%s>" % phase, phase in log_text,
                  "log=%r" % log_text[-400:])

        # ---- 15d-sexies. 源码级防回归 ----
        # 宿主上提权的内核调用不存在 ⇒ 漏调没有任何行为痕迹。「漏提权」同类
        # bug 已三犯：①备份正常/查询说没有、②扫描恒空、③删除真机崩（worker
        # 没提权，copy_tree 一碰 /user/home 进程就被内核带走）。
        # 所以这里钉三契约：①worker 自己提权 —— **走计数器，不再读源码文本**
        # （见下）；②删除走 filemgr_delete_run；③worker 体内不含 copy_tree。
        # ②③ 仍是源码级：它们守的是「不许再长出第二套删除实现」，那是架构
        # 约束而不是行为，正则守架构约束是合适的。①曾经也是源码级，2026-10-16
        # 搬进了 save_domain_enter() 的计数 —— 因为它守的是**行为**。
        # hosttest.py lives at the repo root; ROOT is a temp workdir.
        repo_root = os.path.dirname(os.path.abspath(__file__))
        with open(os.path.join(repo_root, "src", "mango_port.c"),
                  encoding="utf-8", errors="replace") as f:
            nx_src = f.read()
        w = nx_src.find("save_delete_worker(void *arg)")
        del_body = nx_src[w:w + 6000] if w >= 0 else ""
        # ---- 契约①：worker 自己提权 ----
        # ⚠️ 2026-10-16 之前这条是「在 del_body 里找 save_escalate(); 这个子串」。
        #    它只能那样写，是因为宿主上提权是 `#ifdef __linux__ return 0`，漏调
        #    没有任何行为痕迹 —— 但代价是不变量住在**源码格式**里：函数改个名、
        #    调用被包一层、body 拆开，守卫就假绿而 bug 回来。
        #    现在 save_domain_enter() 在任何平台都计数（含宿主），并由
        #    /api/save-domain 暴露。一次删除必须花掉 **两次**：api_save_delete
        #    在 MHD 线程一次（find_save_dir 要用），save_delete_worker 在新
        #    pthread 上一次。只 +1 = worker 漏提权（正是三犯的形态）；+0 = 两边都漏。
        #    日志断言（下面 ESCALATE rc=0）钉的是「worker 走到了提权且 rc 落盘」，
        #    这条钉的是「次数对得上」——互补，不重复。
        check("a save delete enters the save domain TWICE (MHD path + worker "
              "thread); asserted on the counter, not on source text",
              dom_after - dom_before >= 2,
              "before=%s after=%s delta=%s" % (dom_before, dom_after,
                                               dom_after - dom_before))
        # ---- 源码级防回归 2：删除必须走 filemgr_delete_run ----
        # 存档删除真机反复崩，自研 save_rm_task_r 排查三轮未根除；最终把删除
        # 改成与文件管理「删除」按钮同一条 count_path_bytes + remove_path 管线
        # （filemgr_delete_run，filemgr.c 导出）。钉死两条：worker 里必须真的
        # 调它；自研递归 save_rm_task_r 必须从源码里消失（谁再抄回来就是
        # 绕开被验证过的删除实现）。
        check("save_delete_worker deletes via filemgr_delete_run (one proven "
              "delete pipeline for both file page and save page)",
              w >= 0 and "filemgr_delete_run(" in del_body,
              "body=%r" % del_body[:400])
        check("self-made save_rm_task_r recursion is GONE (do not reintroduce "
              "a second delete implementation)",
              "save_rm_task_r(" not in nx_src)
        # ---- 源码级防回归 3：删除路径不得再含内联 copy_tree 调用 ----
        # 真机六连崩的真凶是「删前强制内联快照」这一步 copy_tree(target, snap)：
        # 日志每次停在 SNAPROOT-READY、SNAPSHOT-OK/SNAPSHOT-FAILED 永不出现 ⇒
        # 进程在 copy_tree 内被内核直接带走（fault，非错误返回），删除那一步根本
        # 没执行到。前五轮改的代码一行没被真机跑到。copy_tree 已从 worker 彻底
        # 移除。钉死的是具体调用 copy_tree(target —— 注释里提到 copy_tree() 不算。
        check("save_delete_worker has NO inline copy_tree(target, snap) call "
              "(six-time crash root cause; safety net is now optional backup)",
              w >= 0 and "copy_tree(target" not in del_body,
              "body=%r" % del_body[:400])
        # ---- 源码级防回归 3b：硬门槛已移除（用户拍板不强制备份） ----
        # 2026-10-04 用户明确要求「不要强制我备份再删除」。save_delete_worker
        # 体内的 no_snapshot + save_dir_has_content 硬门槛已移除。谁把硬门槛
        # 抄回来就是违反用户意愿。
        check("save_delete_worker has NO hard gate (user opted for optional backup)",
              w >= 0 and "no_snapshot" not in del_body
              and "save_dir_has_content(" not in del_body,
              "body=%r" % del_body[:400])
        # ---- 源码级防回归 4：copy_tree_r 必须带取证埋点 ----
        # 备份/恢复仍走 copy_tree；如果备份按钮也崩（前五轮假设它正常是没验证的），
        # 没有埋点就又一次什么都查不到。FILE-BEFORE 在 copy_file_atomic 之前打，
        # fault 时这行就是案发记录。
        ctr = nx_src.find("copy_tree_r(const char *src, const char *dst, unsigned depth)")
        cpy_body = nx_src[ctr:ctr + 4000] if ctr >= 0 else ""
        check("copy_tree_r has forensics markers (FILE-BEFORE + DIR + CHILD-FAIL)",
              ctr >= 0 and "cpy FILE-BEFORE" in cpy_body
              and "cpy DIR" in cpy_body and "cpy CHILD-FAIL" in cpy_body)
        # ---- 源码级防回归 5：NAS 拉取任务的 current 文本 ----
        # 用户报「复制全程一直显示正在统计远端目录」：nas_copy_worker 把 current
        # 设成该文案后，nas_pull_file 每块 task_update(…, task->current, …) 把旧
        # 文本原样写回 —— 标题永远停在扫描阶段。钉死三契约：①扫描阶段每个目录
        # 推进 current（与 filemgr 删除管线的 count_path_bytes_sync 同口径）；
        # ②拉取阶段 current 换成正在拉的文件路径；③两处都必须经 nas_url_label
        # 脱敏 —— task->srcs 里的 URL 带 user:pass，而 current 会经 /api/tasks
        # 的 label/note 原样回显到页面。
        scan_w = nx_src.find("nas_scan_tree(nas_copy_ctx_t *c, const char *url")
        scan_end = nx_src.find("nas_pull_tree(nas_copy_ctx_t *c")
        scan_body = nx_src[scan_w:scan_end] if scan_w >= 0 and scan_end > scan_w else ""
        check("nas_scan_tree advances current while scanning (the stuck "
              "'正在统计远端目录' label was the reported bug)",
              scan_w >= 0 and "task_update(c->task, TASK_RUNNING, lbl, 0, NULL)" in scan_body
              and "nas_url_label(url, lbl, sizeof(lbl))" in scan_body,
              "body=%r" % scan_body[:300])
        pull_w = nx_src.find("nas_pull_file(file_task_t *task, transfer_src_t **slot")
        pull_end = nx_src.find("fetch_worker(void *arg)")
        pull_body = nx_src[pull_w:pull_end] if pull_w >= 0 and pull_end > pull_w else ""
        check("nas_pull_file updates current to the file being pulled "
              "(never re-passing the stale scan label)",
              pull_w >= 0 and "task_update(task, TASK_RUNNING, lbl, 0, NULL)" in pull_body
              and "task_update(task, TASK_RUNNING, task->current" not in pull_body,
              "body=%r" % pull_body[:300])
        lbl_w = nx_src.find("nas_url_label(const char *url")
        lbl_body = nx_src[lbl_w:lbl_w + 1200] if lbl_w >= 0 else ""
        check("nas_url_label parses via transfer_url_parse and NEVER echoes the "
              "raw URL (a malformed one may still carry user:pass)",
              lbl_w >= 0 and "transfer_url_parse(url, &nas)" in lbl_body
              and 'snprintf(out, out_size, "%s", url)' not in lbl_body
              and 'snprintf(out, out_size, "?")' in lbl_body,
              "body=%r" % lbl_body[:400])
        st, j = c.api_post("/api/save/list", {"title_id": title})
        snaps3 = (j or {}).get("snapshots") or []
        check("save/delete reports save_found=false afterwards",
              j and j.get("ok") and j.get("save_found") is False, "j=%s" % j)
        check("save/delete kept the pre-existing snapshot (rescue path intact)",
              len(snaps3) >= 1, "snapshots=%s" % snaps3)
        rescued = False
        for sname in snaps3:
            try:
                with open(os.path.join(snap_dir, title, sname, "sd.bin"), "rb") as f:
                    if f.read() == b"LIVE-V2":
                        rescued = True
                        break
            except Exception:
                pass
        check("the surviving snapshot holds the backed-up bytes", rescued)

        # ---- 15e. pkg info via form POST (front-end style) must not be 405 ----
        st, j = c.api_post("/api/pkg/info", {"path": ROOT + "/pkgs/game.pkg"})
        check("POST /api/pkg/info (form) is routed, not 405", st != 405,
              "st=%s" % st)

        # ---- 15f. request ledger (/api/diag) ----
        # 2026-10-20：ledger 是滚动窗口，而 /api/status 只在启动探测时出现过；
        # wait_port 修好（不再被 2082 上的外部服务骗过）后时序变化，跑到这里
        # 时那条早已被挤出窗口。断言前现打一条，考的是 ledger 的行为本身。
        c.get_json("/api/status")
        st, j = c.get_json("/api/diag")
        reqs = (j or {}).get("requests", [])
        check("GET /api/diag returns a request ledger", j and j.get("ok")
              and isinstance(reqs, list) and len(reqs) > 0, "j=%s" % j)
        check("ledger records completed requests with done=true",
              any(r.get("url") == "/api/status" and r.get("done") is True
                  for r in reqs),
              "sample=%s" % reqs[:3])

        # ---- 15g. NAS browsing: scheme routing on /api/fs/list ----
        # The reported bug: clicking a NAS bookmark sent {scheme:"smb",
        # path:"nas.lan/games"} and the back-end ran opendir() on that literal
        # string -> ENOENT -> "No such file or directory". The guard is that a
        # non-local scheme is routed to the NAS backend and the failure names
        # something the user can act on.
        st, j = c.api_json("/api/fs/list", {"scheme": "smb",
                                            "path": "nonexistent.invalid/games"})
        err = (j or {}).get("error", "")
        check("NAS list is routed to the NAS backend (not local ENOENT)",
              j and not j.get("ok") and "No such file or directory" not in err,
              "st=%s j=%s" % (st, j))
        check("NAS list failure carries nas_list_failed",
              j and j.get("error_code") == "nas_list_failed", "j=%s" % j)

        # Unreachable-but-fast: nothing listens on 127.0.0.1:445.
        st, j = c.api_json("/api/fs/list", {"scheme": "smb", "path": "127.0.0.1/share"})
        check("unreachable SMB -> 502 nas_list_failed",
              st == 502 and j and not j.get("ok")
              and j.get("error_code") == "nas_list_failed", "st=%s j=%s" % (st, j))

        st, j = c.api_json("/api/fs/list", {"scheme": "nfs", "path": "127.0.0.1/export"})
        check("unreachable NFS -> 502 nas_list_failed",
              st == 502 and j and not j.get("ok")
              and j.get("error_code") == "nas_list_failed", "st=%s j=%s" % (st, j))

        # A malformed address (non-numeric port) must say so, not "not found".
        st, j = c.api_json("/api/fs/list", {"scheme": "smb", "path": "127.0.0.1:abc/share"})
        check("malformed NAS address -> nas_bad_address",
              j and not j.get("ok") and j.get("error_code") == "nas_bad_address",
              "st=%s j=%s" % (st, j))

        # Share-less SMB address: rejected at the door with an actionable
        # message. Without this guard libsmb2 connects to IPC$ instead and many
        # NAS/Samba boxes answer the root listing by dropping the TCP
        # connection, which surfaces as CONNECTION_REFUSED and sends the user
        # off to check the network while the real problem is the address.
        st, j = c.api_json("/api/fs/list", {"scheme": "smb", "path": "127.0.0.1"})
        check("SMB address without share name -> nas_bad_address naming the share",
              st == 400 and j and not j.get("ok")
              and j.get("error_code") == "nas_bad_address"
              and "共享名" in (j or {}).get("error", ""), "st=%s j=%s" % (st, j))
        st, j = c.api_json("/api/fs/list", {"scheme": "smb", "path": "\\192.168.1.3\\"})
        check("SMB share-less UNC form also rejected as nas_bad_address",
              st == 400 and j and not j.get("ok")
              and j.get("error_code") == "nas_bad_address", "st=%s j=%s" % (st, j))

        # Credentials / port are spliced into the URL, never rejected as bad input.
        st, j = c.api_json("/api/fs/list", {"scheme": "smb", "path": "127.0.0.1/share",
                                            "user": "guest", "pass": "x", "port": "1500"})
        check("NAS list with credentials+port still reaches the backend",
              j and not j.get("ok") and j.get("error_code") == "nas_list_failed",
              "st=%s j=%s" % (st, j))

        # ---- 15g-bis. 反斜杠是路径分隔符，不是主机名的一部分 ----
        # 用户实测（2026-10-01）：地址栏填 Windows 形状的
        # "192.168.1.3\PS5_Games" ⇒
        #   NAS 连接失败：Invalid address:192.168.1.3\PS5_Games
        #                 Can not resolve into IPv4/v6.
        # 即整串（含反斜杠）被当成主机名交给了 getaddrinfo。
        #
        # 判据是**两种失败的形状不同**：解析阶段失败 ⇒ nas_bad_address；
        # 到了后端才失败 ⇒ nas_list_failed。修复前是后者**且消息里带
        # "Invalid address" / "Can not resolve"**；修复后仍然是后者，
        # 但主机名已经干净（127.0.0.1 秒拒，不走 DNS，所以断言离线且快）。
        # 故意不打 192.168.1.3：真去连一个不存在的内网地址会挂到超时。
        for label, naspath in (
            ("host\\share", "127.0.0.1\\PS5_Games"),
            ("UNC \\\\host\\share", "\\\\127.0.0.1\\PS5_Games"),
            ("forward-slash control", "127.0.0.1/PS5_Games"),
        ):
            st, j = c.api_json("/api/fs/list", {"scheme": "smb", "path": naspath})
            msg = (j or {}).get("error", "") or ""
            check("NAS %s -> backslash is a separator, host not the whole string" % label,
                  j and not j.get("ok")
                  and j.get("error_code") == "nas_list_failed"
                  and "Can not resolve" not in msg and "Invalid address" not in msg,
                  "path=%r st=%s j=%s" % (naspath, st, j))

        # 这条同时钉住「共享名之后那个文件夹可以不写」这个答复（用户问④）。
        st, j = c.api_json("/api/fs/list", {"scheme": "smb", "path": "127.0.0.1"})
        check("share-less rejection explains the folder after the share is optional",
              j and not j.get("ok")
              and "可以不写" in (j.get("error") or ""), "st=%s j=%s" % (st, j))

        # ---- 15g-ter. NAS 源不能走 /api/copy（远程源要走 transfer 层） ----
        # 用户实测③：「把 NAS 中的文件复制到 PS5 中时会提示复制失败：source not
        # found」。/api/copy 的第一个动作就是 lstat(src)，而 "192.168.1.3/PS5_Games/x"
        # 在本机根本不存在 ⇒ 必然 source not found。修法是两半：
        #   ① 这一形状的源给出**可操作**的文案（指明该用哪个按钮）；
        #   ② 新增 /api/nas/copy 真正走 transfer 层（断言见下）。
        st, j = c.api_json("/api/copy", {"paths": "192.168.1.3/PS5_Games/x.bin",
                                         "dst": ROOT, "overwrite": "0"})
        check("copy with a NAS-shaped source names the right action",
              j and not j.get("ok") and "复制到本机" in (j.get("error") or ""),
              "st=%s j=%s" % (st, j))
        # 反证另一半：本地路径（以 '/' 开头）漏掉时**不能**也去劝人点「复制到本机」，
        # 那样只会把人引到错方向。源故意放在 sub/ 下，免得撞上
        # 「source and destination are the same」那道更早的闸。
        st, j = c.api_json("/api/copy", {"paths": ROOT + "/sub/nosuch-xyz.bin",
                                         "dst": ROOT, "overwrite": "0"})
        check("a genuinely missing LOCAL source still says plain 'source not found'",
              j and not j.get("ok")
              and (j.get("error") or "") == "source not found", "st=%s j=%s" % (st, j))

        # ---- 15g-quater. 反方向（本地 → NAS）必须**同步**拒绝 ----
        # transfer 层**只有读接口**（没有写接口），所以「传到 NAS」此刻做不到。
        # 不挡的话 /api/copy 会照 "192.168.1.3/PS5_Games" 这个相对形状一路走下去：
        # 目标路径被当成相对路径接受，任务**入队成功**（前端于是显示"已开始"），
        # 然后才在空间检查那一步失败 —— 用户拿到的是一个「看起来跑起来了、
        # 结果莫名其妙失败」的任务，而不是一句「这个方向做不到」。
        #
        # ⚠️ 这里原本还有一条「进程当前目录下没多出垃圾树」的断言，**已删**：
        #    反证时把闸门摘掉，它照样绿 —— 因为目标父目录根本不存在，
        #    失败发生在写盘之前。一条永远不会变红的断言没有任何价值
        #    （本项目反复踩的就是这种「看起来在测、其实什么都没测」）。
        #    dst 用一个**目录**源也不行，试过了：空间检查挡在写之前。
        st, j = c.api_json("/api/copy", {"paths": ROOT + "/folder",
                                         "dst": "192.168.1.3/PS5_Games",
                                         "overwrite": "0"})
        check("copy to a NAS-shaped destination is refused synchronously",
              st == 400 and j and not j.get("ok")
              and j.get("task_id") is None
              and "本地路径" in (j.get("error") or ""),
              "st=%s j=%s" % (st, j))

        # ---- 15g-quinquies. /api/nas/copy 的那几道门 ----
        st, j = c.api_json("/api/nas/copy", {"scheme": "smb",
                                             "paths": "127.0.0.1/games/readme.txt",
                                             "dst": "192.168.1.3/PS5_Games"})
        check("nas/copy refuses a remote destination (bad_target)",
              st == 400 and j and not j.get("ok")
              and j.get("error_code") == "bad_target", "st=%s j=%s" % (st, j))
        st, j = c.api_json("/api/nas/copy", {"scheme": "smb",
                                             "paths": "127.0.0.1/games/readme.txt",
                                             "dst": ROOT + "/no-such-dir-xyz"})
        check("nas/copy refuses a destination that does not exist (bad_target)",
              st == 400 and j and j.get("error_code") == "bad_target",
              "st=%s j=%s" % (st, j))
        st, j = c.api_json("/api/nas/copy", {"scheme": "ftp",
                                             "paths": "127.0.0.1/games/readme.txt",
                                             "dst": ROOT})
        check("nas/copy with an uncompiled backend -> 501 nas_unsupported",
              st == 501 and j and j.get("error_code") == "nas_unsupported",
              "st=%s j=%s" % (st, j))
        st, j = c.api_json("/api/nas/copy", {"scheme": "smb", "paths": "",
                                             "dst": ROOT})
        check("nas/copy with an empty paths -> 400 (form of '源路径与目标目录都要填')",
              st == 400 and j and not j.get("ok"), "st=%s j=%s" % (st, j))
        # 只有空白/空行 ⇒ 切完一行不剩。这条同时证明两件事：行是按 '\n' 切的，
        # 且「全是空行」不会被当成一个合法的源混过去（真机上是粘贴带进来的换行）。
        st, j = c.api_json("/api/nas/copy", {"scheme": "smb",
                                             "paths": "   \n\t\n  ", "dst": ROOT})
        check("nas/copy with only blank lines -> '没有可复制的源路径'",
              st == 400 and j and not j.get("ok")
              and "没有可复制的源路径" in (j.get("error") or ""),
              "st=%s j=%s" % (st, j))

        # ---- 15h. NAS end-to-end against a real SMB2 server ----
        # Nothing above proves a share can actually be listed; it only proves the
        # failure is NAS-flavoured. This block points the client at a real smbd
        # (started by .build/nas-fixture.sh) and asserts the entries come back.
        if nas_port_open(NAS_PORT):
            st, j = c.api_json("/api/fs/list",
                               {"scheme": "smb", "path": "127.0.0.1/games",
                                "port": str(NAS_PORT)})
            ents = (j or {}).get("entries", []) if j else []
            names = {e["name"] for e in ents}
            check("NAS smb: lists a real share", j and j.get("ok")
                  and names == {"readme.txt", "subdir"}, "st=%s j=%s" % (st, j))
            isdir = {e["name"]: e.get("is_dir") for e in ents}
            check("NAS smb: subdir marked is_dir", isdir.get("subdir") is True,
                  "isdir=%s" % isdir)
            sizes = {e["name"]: e.get("size") for e in ents}
            check("NAS smb: file size carried through",
                  sizes.get("readme.txt") == len(b"hello from nas\n"),
                  "sizes=%s" % sizes)

            st, j = c.api_json("/api/fs/list",
                               {"scheme": "smb", "path": "127.0.0.1/games/subdir",
                                "port": str(NAS_PORT)})
            names = {e["name"] for e in ((j or {}).get("entries", []) if j else [])}
            check("NAS smb: descends into a subdirectory", j and j.get("ok")
                  and names == {"inner.txt"}, "st=%s j=%s" % (st, j))

            st, j = c.api_json("/api/fs/list",
                               {"scheme": "smb", "path": "127.0.0.1/nosuchshare",
                                "port": str(NAS_PORT)})
            check("NAS smb: unknown share -> clean backend error, not a crash",
                  j and not j.get("ok") and j.get("error_code") == "nas_list_failed",
                  "st=%s j=%s" % (st, j))

            # ---- NAS → PS5 复制（用户报③那条路的真实验证） ----
            # ⚠️ 只断言任务报 done 是不够的：一个「什么都没拉」的实现也能 done。
            #   判据必须是**磁盘上的字节**。
            # 这段同时把切行/跳过空白/去重三条一起走一遍：paths 里故意混进一个
            # 空行和一条重复，且第二条是一个**目录**（考子树重建）。
            local_dst = os.path.join(ROOT, "nas-pulled")
            os.makedirs(local_dst, exist_ok=True)
            nas_paths = ("127.0.0.1/games/readme.txt\n"
                         "\n"
                         "  127.0.0.1/games/readme.txt  \n"
                         "127.0.0.1/games/subdir")
            st, j = c.api_json("/api/nas/copy",
                               {"scheme": "smb", "paths": nas_paths,
                                "dst": local_dst, "port": str(NAS_PORT)})
            nas_id = (j or {}).get("task_id")
            check("nas/copy queues a pull task", j and j.get("ok")
                  and nas_id is not None, "st=%s j=%s" % (st, j))
            nas_last = {}
            nas_labels = []
            run_labels = []
            if nas_id is not None:
                for _ in range(150):
                    st, pj = c.api_post("/api/install/poll", {"id": str(nas_id)})
                    if not pj or not pj.get("ok"):
                        break
                    nas_last = pj
                    nas_labels.append(pj.get("label") or "")
                    if pj.get("state_name") == "running":
                        run_labels.append(pj.get("label") or "")
                    if pj.get("state_name") in ("done", "failed", "cancelled"):
                        break
                    time.sleep(0.05)
            check("nas/copy finished cleanly", nas_last.get("state_name") == "done",
                  "last=%s" % nas_last)
            # 标签脱敏（真机 bug：复制全程标题停在「正在统计远端目录」；修复后
            # current = 正在扫描/拉取的脱敏路径）。硬断言：任何状态下任务标签都
            # 不得携带主机名/scheme —— task->srcs 里的 URL 带 user:pass，漏脱敏
            # 就是把 NAS 密码贴到页面上。
            check("nas/copy labels never leak host/scheme/credentials",
                  all("127.0.0.1" not in l and "://" not in l for l in nas_labels),
                  "labels=%r" % nas_labels)
            # 行为证据（抓到 running 才断言：本 fixture 的文件只有十几字节，
            # 极端情况下首个 poll 就是 done，抓不到运行中样本不硬凑）。
            if run_labels:
                check("nas/copy RUNNING label is the sanitized remote path "
                      "(/share/path, advancing past the scan label)",
                      all(l.startswith("/games") for l in run_labels),
                      "run_labels=%r" % run_labels)
            got_readme = got_inner = None
            try:
                with open(os.path.join(local_dst, "readme.txt"), "rb") as f:
                    got_readme = f.read()
            except OSError:
                pass
            try:
                with open(os.path.join(local_dst, "subdir", "inner.txt"), "rb") as f:
                    got_inner = f.read()
            except OSError:
                pass
            check("nas/copy landed the file bytes (blank + duplicate lines skipped)",
                  got_readme == b"hello from nas\n", "got=%r" % (got_readme,))
            check("nas/copy recreated the remote subtree locally",
                  got_inner == b"inner\n", "got=%r" % (got_inner,))
            check("nas/copy total = the two pulled files' bytes (file source counted)",
                  (nas_last.get("total") or 0)
                  == len(b"hello from nas\n") + len(b"inner\n"),
                  "last=%s" % nas_last)
            c.wait_idle()
        else:
            SKIP.append("NAS end-to-end (nothing listening on 127.0.0.1:%d; "
                        "run .build/nas-fixture.sh as root first)" % NAS_PORT)

        # Local scheme unchanged (regression guard for the owendswang path).
        st, j = c.api_json("/api/fs/list", {"scheme": "local", "path": "/nonexistent-xyz"})
        check("local scheme missing path -> 404 (unchanged)",
              st == 404 and j and not j.get("ok"), "st=%s j=%s" % (st, j))

        # ---- 15i. device roots: an unpopulated mount point is not "attached" ----
        # PS5 pre-creates /mnt/usb0, /mnt/usb1 and /mnt/ext1 as empty directories,
        # so listing them SUCCEEDS with nothing plugged in — and the front-end's
        # probeRoots shows a shortcut for a drive that is not there. That is the
        # reported bug. WFM_DEVICE_ROOT_PREFIX points the check at devroot/ so both
        # branches are testable without a console.
        st, j = c.api_json("/api/fs/list", {"scheme": "local",
                                            "path": os.path.join(devroot, "empty")})
        check("unmounted device root -> 404 device_not_mounted",
              st == 404 and j and not j.get("ok")
              and j.get("error_code") == "device_not_mounted", "st=%s j=%s" % (st, j))

        st, j = c.api_json("/api/fs/list", {"scheme": "local",
                                            "path": os.path.join(devroot, "full")})
        check("populated device root still lists",
              j and j.get("ok")
              and {e["name"] for e in j.get("entries", [])} == {"games"},
              "st=%s j=%s" % (st, j))

        # Anything deeper is an ordinary directory INSIDE a device: never gated,
        # otherwise browsing an attached drive would break.
        st, j = c.api_json("/api/fs/list", {"scheme": "local",
                                            "path": os.path.join(devroot, "full", "games")})
        check("paths deeper than a device root are never gated",
              j and j.get("ok"), "st=%s j=%s" % (st, j))

        st, j = c.api_json("/api/fs/list", {"scheme": "local", "path": devroot})
        check("the prefix directory itself is never gated",
              j and j.get("ok"), "st=%s j=%s" % (st, j))

        # ---- 15j. save_mgr 归档端点：zipw 写入器 + backup + 错误契约 ----
        # zipw 往返：用 gcc 把 src/zip_write.c 编成独立小工具（纯 lib，无 HTTP
        # 依赖），收集一棵「嵌套目录+普通文件+点文件+符号链接」的树并写出 STORE
        # ZIP；Python zipfile 与服务器自身的 zipx_extract 引擎两头解出逐字节比对，
        # 并钉住「点文件/符号链接不进归档」。
        zc = os.path.join(WORKDIR, ".build", "zipw_harness.c")
        zb = os.path.join(WORKDIR, ".build", "zipw_harness")
        os.makedirs(os.path.dirname(zc), exist_ok=True)
        with open(zc, "w", encoding="utf-8") as f:
            f.write(r'''/* zipw_harness.c — hosttest driver for src/zip_write.c. */
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include "zip_write.h"

static int sink_write(void *ctx, const void *buf, size_t len) {
  int fd = *(int *)ctx;
  size_t off = 0;
  while (off < len) {
    ssize_t w = write(fd, (const char *)buf + off, len - off);
    if (w <= 0) return -1;
    off += (size_t)w;
  }
  return 0;
}

int main(int argc, char **argv) {
  if (argc != 3) return 2;
  zipw_file_t *files = NULL;
  int n = 0;
  if (zipw_walk_dir(argv[1], &files, &n) < 0) return 3;
  int fd = open(argv[2], O_CREAT | O_WRONLY | O_TRUNC, 0644);
  if (fd < 0) return 4;
  zipw_sink_t sink = {sink_write, &fd, 0};
  char *iobuf = malloc(256 * 1024);
  int rc = iobuf ? zipw_write_files(&sink, files, n, iobuf, 256 * 1024) : -1;
  free(iobuf);
  close(fd);
  printf("%d\n", n);
  for (int i = 0; i < n; i++) { free(files[i].path); free(files[i].name); }
  free(files);
  return rc < 0 ? 5 : 0;
}
''')
        cc = shutil.which("cc") or "cc"
        cr = subprocess.run([cc, "-O2", "-Wall", "-Isrc", "-o", zb, zc,
                             os.path.join("src", "zip_write.c")],
                            cwd=WORKDIR, capture_output=True, text=True)
        check("zipw harness compiles (cc)", cr.returncode == 0,
              "stderr=%s" % (cr.stderr or ""))

        tree = os.path.join(ROOT, "zipw_tree")
        os.makedirs(os.path.join(tree, "sub", "deep"), exist_ok=True)
        payloads = {
            "a.txt": b"hello-a" * 1000,
            "sub/b.bin": bytes(range(256)) * 200,
            "sub/deep/c.dat": os.urandom(70000),
        }
        for rel, data in payloads.items():
            with open(os.path.join(tree, *rel.split("/")), "wb") as f:
                f.write(data)
        with open(os.path.join(tree, ".hidden"), "wb") as f:
            f.write(b"no-dot-files")
        os.makedirs(os.path.join(tree, "sub", ".hdir"), exist_ok=True)
        with open(os.path.join(tree, "sub", ".hdir", "x"), "wb") as f:
            f.write(b"x")
        made_symlink = False
        try:
            os.symlink(os.path.join(tree, "a.txt"),
                       os.path.join(tree, "link.txt"))
            made_symlink = True
        except OSError:
            pass
        outzip = os.path.join(ROOT, "zipw_out.zip")
        if os.path.exists(outzip):
            os.remove(outzip)
        hr = subprocess.run([zb, tree, outzip], capture_output=True, text=True)
        got_n = hr.stdout.strip() if hr.returncode == 0 else ""
        check("zipw_walk_dir + zipw_write_files run clean",
              hr.returncode == 0 and got_n.isdigit(),
              "rc=%s out=%s err=%s" % (hr.returncode, hr.stdout, hr.stderr))
        check("zipw skips dot files and symlinks (3 regular files only)",
              got_n == "3", "n=%r symlink_made=%s" % (got_n, made_symlink))
        if got_n == "3" and os.path.isfile(outzip):
            with zipfile.ZipFile(outzip) as zf:
                znames = sorted(zf.namelist())
                check("zip entry names are the nested rel paths",
                      znames == sorted(payloads.keys()), "names=%s" % znames)
                for rel, expect in payloads.items():
                    try:
                        got = zf.read(rel)  # zipfile 读时校验 CRC（data descriptor）
                    except Exception:
                        got = None
                    check("zip entry %r byte-identical" % rel,
                          got == expect,
                          "got_len=%r want_len=%r" % (None if got is None else len(got), len(expect)))
            # 服务器自身的 zipx_extract 引擎再解一遍：走真实解压链路。
            xdst = os.path.join(ROOT, "zipw_x")
            os.makedirs(xdst, exist_ok=True)
            st, xj = c.api_json("/api/extract",
                                {"path": outzip, "dst_dir": xdst,
                                 "conflict": "overwrite", "subdir": "0"})
            xtid = (xj or {}).get("task_id")
            xdone = None
            if xtid is not None:
                for _ in range(240):
                    st, xp = c.api_post("/api/install/poll", {"id": str(xtid)})
                    if xp and xp.get("ok") and xp.get("state_name") in (
                            "done", "failed", "cancelled"):
                        xdone = xp
                        break
                    time.sleep(0.25)
            check("zipw zip unpacks via server zipx_extract engine",
                  xdone is not None and xdone.get("state_name") == "done",
                  "xj=%s xdone=%s" % (xj, xdone))
            xok = True
            for rel, expect in payloads.items():
                p = os.path.join(xdst, *rel.split("/"))
                try:
                    with open(p, "rb") as f:
                        xgot = f.read()
                except OSError:
                    xgot = None
                if xgot != expect:
                    xok = False
            check("zipx_extract output byte-identical to source", xok)
        shutil.rmtree(tree, ignore_errors=True)

        # backup：造假 PS5 镜像（savedata_prospero）+ PS4 镜像（savedata 叶 + .bin），
        # 打本地备份，断言 200 + path 存在 + zip 可解且条目与源字节一致。
        fake_uid = "10000001"
        btid = "CUSA0BACK"
        bdir = "savedata0"
        img5 = os.path.join(save_root, fake_uid, "savedata_prospero",
                            btid, "sdimg_" + bdir)
        os.makedirs(os.path.dirname(img5), exist_ok=True)
        img5_bytes = os.urandom(5000)
        with open(img5, "wb") as f:
            f.write(img5_bytes)
        # 元数据 icon：一份 PS5 存档在盘上是「镜像 + savedata_prospero_meta 里的
        # icon」两份东西。删除会删它，备份就必须打包它，否则 备份→删除→还原
        # 之后系统存档列表的缩略图永远空着。
        ico5 = os.path.join(save_root, fake_uid, "savedata_prospero_meta",
                            "user", btid, bdir + "_icon0.png")
        os.makedirs(os.path.dirname(ico5), exist_ok=True)
        with open(ico5, "wb") as f:
            f.write(b"PNG-ICON")
        st, j = c.api_post("/api/save_mgr/backup",
                           {"uid": fake_uid, "title_id": btid,
                            "dir_name": bdir, "target": "local"})
        bpath = (j or {}).get("path")
        check("POST /api/save_mgr/backup local -> ok + path",
              st == 200 and j and j.get("ok") and bpath, "st=%s j=%s" % (st, j))
        check("backup zip exists at returned path",
              bool(bpath and os.path.isfile(bpath)), "path=%r" % (bpath,))
        if bpath and os.path.isfile(bpath):
            entry = "%s/sdimg_%s" % (btid, bdir)
            with zipfile.ZipFile(bpath) as zf:
                check("backup zip contains <tid>/sdimg_<dir>",
                      entry in zf.namelist(), "names=%s" % zf.namelist())
                got = zf.read(entry)
                check("backup image byte-identical", got == img5_bytes,
                      "len=%d want=%d" % (len(got), len(img5_bytes)))
                check("backup zip carries manifest.json "
                      "(restore never mounts, so it cannot read param.sfo)",
                      "manifest.json" in zf.namelist(),
                      "names=%s" % zf.namelist())
                check("backup zip carries the meta icon",
                      ("meta/%s/%s_icon0.png" % (btid, bdir)) in zf.namelist(),
                      "names=%s" % zf.namelist())
            side = bpath[:-4] + ".json" if bpath else None
            check("backup writes a .json sidecar beside the zip "
                  "(so listing does not have to open any zip)",
                  bool(side and os.path.isfile(side)), "sidecar=%r" % (side,))
        # PS4：savedata 叶 + <dir>.bin 一起打包（换一个 tid，避免 savedata_prospero
        # 优先命中把 is_ps4 判成 0 而漏掉 .bin）
        btid4 = "CUSA0BAK4"
        img4 = os.path.join(save_root, fake_uid, "savedata", btid4,
                            "sdimg_" + bdir)
        bin4 = os.path.join(save_root, fake_uid, "savedata", btid4,
                            bdir + ".bin")
        os.makedirs(os.path.dirname(img4), exist_ok=True)
        with open(img4, "wb") as f:
            f.write(b"PS4-IMG")
        with open(bin4, "wb") as f:
            f.write(b"PS4-KEY-BIN")
        st, j = c.api_post("/api/save_mgr/backup",
                           {"uid": fake_uid, "title_id": btid4,
                            "dir_name": bdir, "target": "local"})
        bpath = (j or {}).get("path")
        ps4ok = bool(bpath and os.path.isfile(bpath))
        if ps4ok:
            with zipfile.ZipFile(bpath) as zf:
                n4 = zf.namelist()
                ps4ok = ("%s/sdimg_%s" % (btid4, bdir)) in n4 and \
                        ("%s/%s.bin" % (btid4, bdir)) in n4 and \
                        zf.read("%s/%s.bin" % (btid4, bdir)) == b"PS4-KEY-BIN"
        check("backup PS4 flavor packs sdimg_<dir> + <dir>.bin", ps4ok,
              "st=%s j=%s" % (st, j))
        # 镜像不存在 -> 404
        st, j = c.api_post("/api/save_mgr/backup",
                           {"uid": fake_uid, "title_id": "NOEXIST0",
                            "dir_name": "x", "target": "local"})
        check("backup missing image -> 404 image not found",
              st == 404 and j and j.get("error") == "image not found",
              "st=%s j=%s" % (st, j))

        # ---- 15k. 游戏粒度：整体备份 / 封面端点 / 整体删除（2026-10-13）----
        # 「一个游戏 = 一个整体」是用户拍板的粒度：一个游戏 3 个槽位要备份 3 次、
        # 删 3 次是不可接受的。⚠️ 这三条路径此前 hosttest **零覆盖** —— 新端点
        # 最便宜的假绿就是"没人打过它"（/api/download 的契约错配正是这么一路
        # 绿到真机才炸的），所以端点一落地就把断言补上。
        gtid = "PPSA0WHOLE"
        gdirs = ["slot_a", "slot_b"]
        gbytes = {}
        gbase = os.path.join(save_root, fake_uid, "savedata_prospero", gtid)
        gico = os.path.join(save_root, fake_uid, "savedata_prospero_meta",
                            "user", gtid)
        for _d in gdirs:
            _p = os.path.join(gbase, "sdimg_" + _d)
            os.makedirs(os.path.dirname(_p), exist_ok=True)
            gbytes[_d] = os.urandom(3000)
            with open(_p, "wb") as f:
                f.write(gbytes[_d])
            _i = os.path.join(gico, _d + "_icon0.png")
            os.makedirs(os.path.dirname(_i), exist_ok=True)
            with open(_i, "wb") as f:
                f.write(b"PNG-" + _d.encode())

        # 整体备份：不给 dir_name ⇒ 打包这个 title_id 下的**所有**槽位。
        st, j = c.api_post("/api/save_mgr/backup",
                           {"uid": fake_uid, "title_id": gtid,
                            "target": "local"})
        gpath = (j or {}).get("path")
        check("POST /api/save_mgr/backup 不给 dir_name -> 整体备份，slots=2",
              st == 200 and (j or {}).get("slots") == 2 and bool(gpath),
              "st=%s j=%s" % (st, j))
        if gpath and os.path.isfile(gpath):
            with zipfile.ZipFile(gpath) as zf:
                gnames = zf.namelist()
                check("整体备份把每个槽位的镜像都打进包里",
                      all(("%s/sdimg_%s" % (gtid, d)) in gnames for d in gdirs),
                      "names=%s" % (gnames,))
                check("整体备份每个镜像字节一致",
                      all(zf.read("%s/sdimg_%s" % (gtid, d)) == gbytes[d]
                          for d in gdirs
                          if ("%s/sdimg_%s" % (gtid, d)) in gnames))
                check("整体备份每个槽位各带一份自己的 icon",
                      all(("meta/%s/%s_icon0.png" % (gtid, d)) in gnames
                          for d in gdirs),
                      "names=%s" % (gnames,))
                gmj = zf.read("manifest.json").decode("utf-8", "replace")
                check("整体备份 manifest 的 slots[] 逐个记下槽位",
                      all(('"dir":"%s"' % d) in gmj for d in gdirs),
                      "manifest=%s" % (gmj[:300],))
                # 顶层仍保留单槽位那四个字段（取 slots[0]）：老版本还原逻辑只认
                # 它们，v1.0.24/25 打的包必须还能走同一条还原路径。
                check("整体备份 manifest 仍带顶层 dir_name（旧版还原只读它）",
                      '"dir_name":"slot_a"' in gmj, "manifest=%s" % (gmj[:300],))

        # ★ 无封面对照组：这个 tid 只建镜像、**一张封面都不放**。
        #   2026-10-16 之前「没有封面 -> 404」用的是 gtid + dir=nosuch —— 那时
        #   只看当前槽自己的副本，所以随便编个不存在的槽就能触发 404。加了跨槽
        #   借用之后那个请求会被兄弟槽的封面救活（返回 200），于是这条用例
        #   测的其实是**旧口径**。这里按它原本的意图重建场景：整个游戏都没有
        #   封面时，才应该 404。
        ntid = "PPSA0NOICO"
        nd = "nosuch_slot"
        os.makedirs(os.path.join(save_root, fake_uid, "savedata_prospero",
                                 ntid), exist_ok=True)
        os.makedirs(os.path.join(save_root, fake_uid,
                                 "savedata_prospero_meta", "user", ntid),
                    exist_ok=True)
        with open(os.path.join(save_root, fake_uid, "savedata_prospero",
                               ntid, "sdimg_" + nd), "wb") as f:
            f.write(os.urandom(1024))

        # /api/save_mgr/icon：浏览页要显示封面，而 /user 是系统区、文件管理页
        # 不给进，所以单独开一条只读取 png 的端点。⚠️ 不能走 send_buffer()
        # （strlen 会被 png 里的 0 字节截断），这里验的是"字节原样回来"。
        st, draw = c.get("/api/save_mgr/icon?uid=%s&title_id=%s&dir=%s"
                         % (fake_uid, gtid, gdirs[0]))
        check("GET /api/save_mgr/icon -> 200 且字节原样（png 不被 strlen 截断）",
              st == 200 and draw == (b"PNG-" + gdirs[0].encode()),
              "st=%s got=%r" % (st, draw[:16]))
        alive("save_mgr/icon")
        # ★ 跨槽借用（2026-10-16 加）：一个游戏有多个存档槽，兄弟槽有封面时
        #   不该让它白躺着 —— 真机 PPSA23226 的 ArchiveSaveFile1 槽抠不出 icon
        #   （日志 capture rc=-1 复现多次），而 UserSettingSaveGame 槽带着图。
        #   这里用"不存在的槽名"来查，返回兄弟槽的图就证明借用生效。
        st, draw = c.get("/api/save_mgr/icon?uid=%s&title_id=%s&dir=nosuch"
                         % (fake_uid, gtid))
        check("GET /api/save_mgr/icon 兄弟槽有封面 -> 跨槽借用 200",
              st == 200 and draw in (b"PNG-" + d.encode() for d in gdirs),
              "st=%s got=%r" % (st, draw[:16]))
        # 没有封面时才 404 —— 用一个**整个游戏都没图**的 tid（不能再用 gtid +
        # 编造槽名：那个请求现在会被上面的借用救活，测不到 404）。
        st, draw = c.get("/api/save_mgr/icon?uid=%s&title_id=%s&dir=%s"
                         % (fake_uid, ntid, nd))
        check("GET /api/save_mgr/icon 整个游戏都没有封面 -> 404 no icon",
              st == 404 and (c.j(st, draw) or {}).get("error") == "no icon",
              "st=%s j=%s" % (st, c.j(st, draw)))
        # ⚠️ 缺 uid 不算错（sm_uid_hex 回落到前台用户）。
        # 缺 dir 时默认槽位级封面请求 400；带 game=1 则进入游戏级封面模式，
        # 按 title_id 从 appmeta / homebrew 查找，不需要具体槽位。
        st, draw = c.get("/api/save_mgr/icon?uid=%s&title_id=%s"
                         % (fake_uid, gtid))
        check("GET /api/save_mgr/icon 缺 dir -> 400 required",
              st == 400 and (c.j(st, draw) or {}).get("error")
              == "uid/title_id[/dir] required",
              "st=%s j=%s" % (st, c.j(st, draw)))
        st, draw = c.get("/api/save_mgr/icon?uid=%s&title_id=%s&game=1"
                         % (fake_uid, gtid))
        check("GET /api/save_mgr/icon?game=1 游戏级封面 -> 200/404",
              st in (200, 404), "st=%s j=%s" % (st, c.j(st, draw)))
        st, draw = c.get("/api/save_mgr/icon?uid=%s&title_id=%s&dir=%%2e%%2e%%2fetc"
                         % (fake_uid, gtid))
        check("GET /api/save_mgr/icon dir 带路径分隔 -> 400（不能借它读 /user 之外的东西）",
              st == 400, "st=%s j=%s" % (st, c.j(st, draw)))

        # 整体删除：不给 dir_name ⇒ 删掉这个游戏的所有槽位（镜像 + icon + db 行）。
        st, j = c.api_post("/api/save_mgr/delete",
                           {"uid": fake_uid, "title_id": gtid})
        check("POST /api/save_mgr/delete 不给 dir_name -> 整体删除 deleted=2",
              st == 200 and (j or {}).get("deleted") == 2
              and (j or {}).get("failed") == 0, "st=%s j=%s" % (st, j))
        check("整体删除删掉了每个槽位的镜像",
              not any(os.path.exists(os.path.join(gbase, "sdimg_" + d))
                      for d in gdirs))
        check("整体删除删掉了每个槽位的 meta icon",
              not any(os.path.exists(os.path.join(gico, d + "_icon0.png"))
                      for d in gdirs))
        st, j = c.api_post("/api/save_mgr/delete",
                           {"uid": fake_uid, "title_id": "NOEXIST0"})
        check("POST /api/save_mgr/delete 标题不存在 -> 404 save not found",
              st == 404 and (j or {}).get("error") == "save not found",
              "st=%s j=%s" % (st, j))

        # ---- 备份 → 列备份 → 还原（2026-10-12）----
        # 还原走的是「同机同账号直拷回原路径」这条**零挂载**通道：镜像是加密的
        # PFS，同一台机器同一个账号复制回去就能用，不必解密也不必改签。换机器/
        # 换账号必须走导入重签，所以服务端对账号不符直接回 409 —— 硬还原的结果
        # 就是游戏里一句「存档损坏」。
        st, j = c.api_post("/api/save_mgr/backups", {"target": "local"})
        blist = (j or {}).get("backups") or []
        hit = [b for b in blist
               if b.get("title_id") == btid and b.get("dir_name") == bdir]
        check("POST /api/save_mgr/backups lists the backup (read from the sidecar)",
              st == 200 and len(hit) == 1 and hit[0].get("uid") == fake_uid,
              "st=%s j=%s" % (st, j))
        rpath = hit[0].get("path") if hit else None
        check("backup list entry points at an existing zip",
              bool(rpath and os.path.isfile(rpath)), "path=%r" % (rpath,))

        # 先把镜像换掉，才能证明确实是**备份里的字节**被放回去了
        with open(img5, "wb") as f:
            f.write(b"CLOBBERED")
        os.remove(ico5)
        st, j = c.api_post("/api/save_mgr/restore",
                           {"path": rpath, "uid": fake_uid})
        check("POST /api/save_mgr/restore same user -> ok",
              st == 200 and j and j.get("ok"), "st=%s j=%s" % (st, j))
        check("restore put the image back byte-identical",
              os.path.isfile(img5) and open(img5, "rb").read() == img5_bytes,
              "bytes=%r" % (open(img5, "rb").read()[:16] if os.path.isfile(img5) else None,))
        check("restore restored the meta icon",
              os.path.isfile(ico5) and open(ico5, "rb").read() == b"PNG-ICON",
              "icon=%r" % (open(ico5, "rb").read() if os.path.isfile(ico5) else None,))

        st, j = c.api_post("/api/save_mgr/restore",
                           {"path": rpath, "uid": "99999999"})
        check("POST /api/save_mgr/restore another user -> 409 (would read as corrupted)",
              st == 409, "st=%s j=%s" % (st, j))
        st, j = c.api_post("/api/save_mgr/restore",
                           {"path": "/data/Mango/backups/nope.zip", "uid": fake_uid})
        check("POST /api/save_mgr/restore missing zip -> 404",
              st == 404, "st=%s j=%s" % (st, j))
        st, j = c.api_post("/api/save_mgr/restore", {"uid": fake_uid})
        check("POST /api/save_mgr/restore missing path -> 400",
              st == 400, "st=%s j=%s" % (st, j))

        # 错误契约：未挂载 download 409；无状态 encrypt/resign_download 409；
        # dump_usb 在 host 上必须明确 device-only；encrypt_upload 的参数/文件/
        # create_pfs 三档错误。
        st, j = c.api_post("/api/save_mgr/download", {})
        check("POST /api/save_mgr/download no params -> 400 required",
              st == 400 and j and j.get("error") == "title_id/dir_name required",
              "st=%s j=%s" % (st, j))
        st, j = c.api_post("/api/save_mgr/download",
                           {"title_id": "X", "dir_name": "y"})
        check("POST /api/save_mgr/download with params -> still 409 (host mount fails)",
              st == 409 and j and j.get("error") == "mount failed",
              "st=%s j=%s" % (st, j))
        st, j = c.get_json("/api/save_mgr/encrypt_download?name=x.img")
        check("GET encrypt_download with no state -> 409",
              st == 409 and j and j.get("error") == "no encrypted image ready",
              "st=%s j=%s" % (st, j))
        st, j = c.get_json("/api/save_mgr/resign_download?name=x.img")
        check("GET resign_download with no state -> 409",
              st == 409 and j and j.get("error") == "no resign image ready",
              "st=%s j=%s" % (st, j))
        st, j = c.api_post("/api/save_mgr/dump_usb",
                           {"uid": fake_uid, "title_id": btid,
                            "dir_name": bdir, "usb": "usb0"})
        check("POST /api/save_mgr/dump_usb on host -> 500 device-only",
              st == 500 and j and j.get("error") == "device-only",
              "st=%s j=%s" % (st, j))
        st, j = c.api_post("/api/save_mgr/encrypt_upload", {"title_id": btid})
        check("POST encrypt_upload missing path -> 400",
              st == 400 and j and j.get("error") == "title_id/path required",
              "st=%s j=%s" % (st, j))
        st, j = c.api_post("/api/save_mgr/encrypt_upload",
                           {"title_id": btid, "path": "/no/such.zip"})
        check("POST encrypt_upload missing zip -> 400 zip not found",
              st == 400 and j and j.get("error") == "zip not found",
              "st=%s j=%s" % (st, j))
        st, j = c.api_post("/api/save_mgr/encrypt_upload",
                           {"title_id": btid, "dir_name": "savedata0",
                            "path": outzip})
        check("POST encrypt_upload on host -> 500 create pfs failed",
              st == 500 and j and (j.get("error") or "").startswith("create pfs failed"),
              "st=%s j=%s" % (st, j))

        # ---- 16. still alive after all of it ----
        st, j = c.get_json("/api/status")
        check("server still alive after full sweep", st == 200 and j and j.get("ok"))

        # ---- 17. shutdown last ----
        st, j = c.api_json("/api/shutdown", {})
        check("POST /api/shutdown ok", j and j.get("ok"), "st=%s j=%s" % (st, j))
        gone = False
        for _ in range(30):
            try:
                c.get_json("/api/status")
                time.sleep(0.2)
            except Exception:
                gone = True
                break
        check("server exited after shutdown", gone or proc.poll() is not None)

    finally:
        try:
            proc.send_signal(signal.SIGTERM)
        except Exception:
            pass
        try:
            proc.wait(timeout=5)
        except Exception:
            proc.kill()

    print("\n%d passed, %d failed" % (PASS, FAIL))
    if SKIP:
        print("SKIPPED (fixture missing — this is NOT a pass):")
        for s in SKIP:
            print("  - " + s)
    if FAILED:
        print("failed: " + ", ".join(FAILED))
    return 1 if FAIL else 0


if __name__ == "__main__":
    sys.exit(main())

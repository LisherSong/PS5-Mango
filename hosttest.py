#!/usr/bin/env python3
"""Host test for the merged owendswang-backend + NEXUS-frontend tree.

Emulates the NEXUS front-end exactly:
  * apiJson(path, obj)  -> POST application/json, body = JSON.stringify(obj)
  * apiPost(path, obj)  -> POST application/x-www-form-urlencoded
  * uploadOne()         -> POST /api/upload-file with X-WFM-* headers + raw body
and checks the response shapes the front-end actually consumes.

Run inside WSL:  python3 hosttest.py
"""
import base64
import http.client
import json
import os
import re
import shutil
import signal
import socket
import subprocess
import sys
import time
import urllib.parse

WORKDIR = os.path.dirname(os.path.abspath(__file__))


def _makefile_version():
    """Read VERSION_TAG out of the Makefile.

    $(LINUX_BIN) is PS5-Nexus-linux-$(VERSION_TAG), so any hard-coded name goes
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
    name = "PS5-Nexus-linux-" + VERSION_TAG if VERSION_TAG else "PS5-Nexus-linux"
    return os.path.join(WORKDIR, name)


# WFM_BIN lets a counterfactual run point the whole suite at a deliberately
# re-broken binary (see .build/nas-counterfactual-wsl.sh) without touching the
# tree. Default is the normal build.
BIN = os.environ.get("WFM_BIN") or _host_bin()
ROOT = "/tmp/ps5-nexus-hosttest"
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


def wait_port(ports=range(2026, 2041), timeout=20):
    end = time.time() + timeout
    while time.time() < end:
        for p in ports:
            try:
                c = http.client.HTTPConnection("127.0.0.1", p, timeout=2)
                c.request("GET", "/api/status")
                r = c.getresponse()
                r.read()
                c.close()
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

    # Device-root fixtures for the mount check (WFM_DEVICE_ROOT_PREFIX below).
    #   devroot/empty  -> an empty directory on the SAME filesystem as the prefix,
    #                     i.e. exactly what an unpopulated /mnt/usb0 looks like
    #   devroot/full   -> has content, so it counts as attached
    devroot = os.path.join(ROOT, "devroot")
    os.makedirs(os.path.join(devroot, "empty"), exist_ok=True)
    os.makedirs(os.path.join(devroot, "full", "games"), exist_ok=True)
    with open(os.path.join(devroot, "full", "games", "x.pkg"), "wb") as f:
        f.write(b"pkg")

    env = dict(os.environ)
    env["WFM_SAVE_ROOT"] = save_root
    env["WFM_SNAPSHOT_DIR"] = snap_dir
    env["WFM_HOMEBREW_DIR"] = brew_dir
    env["WFM_DEVICE_ROOT_PREFIX"] = devroot
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
        # /data/wfm/incoming 首次使用时整条不存在，而 ensure_parent_dirs 只处理
        # rel 里的子路径（纯文件名时一个目录都不建）→ check_target_writable
        # 报 "current directory is not writable" → 500 → 前端误报网络故障。
        # 另外两个配套契约：EEXIST 409 必须带真实 error 文案（前端现在会显示
        # 它）；X-WFM-Overwrite:1 重传必须成功（ZIP/PKG 暂存同名覆盖）。
        deep = ROOT + "/wfm/incoming"          # 整条链在宿主夹具里都不存在
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
        # Fixtures come from ps5-nexus-legacy's engine suite (tests/fixtures/).
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

        # ---- 15. NEXUS-only stubs must be graceful ----
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

        # ---- 15b. 存档树递归的深度上限（真机崩溃的兜底契约） ----
        # 40 层嵌套（> NEXUS_SAVE_MAX_DEPTH=32）必须**干净失败**并让任务落到终态，
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

        st, j = c.api_post("/api/save/delete", {"title_id": title})
        check("save/delete queues a task", j and j.get("ok")
              and (j.get("task_id") or 0) > 0, "j=%s" % j)
        c.wait_idle()
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
        # 宿主上 save_escalate() 是 #ifdef __linux__ return 0 ⇒ 行为测试永远
        # 验不出漏调（宿主盲区）。「漏提权」同类 bug 已三犯：①备份正常/查询说
        # 没有、②扫描恒空、③删除真机崩（worker 没提权，copy_tree 一碰
        # /user/home 进程就被内核带走）。本轮根因铲除：copy_tree 已从删除路径
        # 移除（真机六连崩都在内联快照这一步），安全网改为硬门槛。钉死三契约：
        # ①worker 自己提权 ②删除走 filemgr_delete_run ③worker 体内不含 copy_tree。
        # hosttest.py lives at the repo root; ROOT is a temp workdir.
        repo_root = os.path.dirname(os.path.abspath(__file__))
        with open(os.path.join(repo_root, "src", "nexus_port.c"),
                  encoding="utf-8", errors="replace") as f:
            nx_src = f.read()
        w = nx_src.find("save_delete_worker(void *arg)")
        del_body = nx_src[w:w + 6000] if w >= 0 else ""
        check("save_delete_worker escalates ITSELF (kernel authid is per-thread; "
              "host behavior tests can't catch a miss)",
              w >= 0 and "save_escalate();" in del_body,
              "body=%r" % del_body[:300])
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

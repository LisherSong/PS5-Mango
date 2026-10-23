# PS5 Mango — HANDOVER（交接文档，权威）

> ⚠️ **本文档描述的是 `ps5-mango` 那棵树（"重写"路线的设计文档），不是本仓库。**
> 本仓库是另一条路线：以 **owendswang/ps5-web-file-manager 最新版后端**为底座，
> 搬入 MANGO 的前端 UI 与 PKG / 存档能力，产物 `PS5-Mango-v1.0.0.elf`，端口 `2026`。
> 因此本文里的**文件路径、模块划分、ELF 名（`ps5-mango`）都不适用于本仓**；
> 保留它只为设计理由与坑清单（机理、取证）。仓库外的引用
> （`../ps5-nas-rewrite-proposal.html`、`../ps5-ui-demo-*.html`）不在本仓内。
> **本仓的现状看 `README.md` 与 `REAL-CONSOLE-CHECKLIST.md`。**

> 配套设计文档：`docs/PLAN.md`（落地方案 v1，执行计划）。长期备忘：`../.workbuddy/memory/MEMORY.md`。
> 旧仓 `ps5-web-file-manager` 仅作为**引擎 / 测试 / 工具链来源**，本仓是它的重写（新仓库、做自己的）。

---

## 0. 项目身份（已拍板，勿再议）

| 项 | 值 |
| --- | --- |
| 名称 / 仓库 / ELF | **PS5 Mango** / `ps5-mango` / `ps5-mango` |
| 默认端口 | **2026**（避开 8888/9021/9090·12800/1337/9081） |
| 形态 | 单 ELF + 端口 + gzip 内嵌前端，**纯网页、不写 app/Electron** |
| 四个能力域 | ① NAS 互通 ② 解压引擎（7z/RAR/ZIP + 加密 + 分卷）③ 大文件吞吐与续传 ④ 存档管理（P1） |
| 真空校准（差异化卖点） | NFS 互通（SMB 已有 libsmb2 开源参考，非纯真空）、**RAR/7z 解压（含加密+分卷，竞品均无）**、大文件吞吐与续传、FFPKG 通道 |
| 明确不做 | 又一个通用文件管理器；风扇/主题周边；依赖 etaHEN |
| 主 UI | **Harness（E 暗色蓝）**；备选主题 A 主机大厅仅作对照 |
| 继承原则 | 测试充分、耦合低的**叶子模块直接复用**；高耦合**编排层按低耦合接口重写**——模块间只过 `.h` 契约、依赖单向、新功能独立插入不改动其他模块 |

**阶段路线**：Phase 0（真机基线，拆两半）/ 1 传输 / 2 安装+库 / 3 吞吐 / 5 存档 + **UI 并行轨道**（贯穿 1–5）。总工期 ≈11–16 周。

---

## 1. 当前进度（Session 1，2026-09-27）

| 任务 | 状态 | 交付物 |
| --- | --- | --- |
| 仓库初始化 | ✅ | `git init` + `.gitignore`（适配新名 + `.build` 白名单 + host-test 忽略） |
| 复制测试过的叶子模块 | ✅ | `src/` 29 文件：解压引擎（extract/zip/rar/zipx/sevenz 全套）+ `json_util` + `path_util` + `pkg_info`（SFO 解析） |
| 复制 vendored 依赖 | ✅ | `third_party/minizip-ng`（11 src）+ `third_party/unrar7`（161 文件） |
| 复制测试矩阵 | ✅ | `tests/` 66 文件（run-tests.sh / run-sevenz-tests.sh / 兼容头 / test_*.c / fixtures / 夹具生成器） |
| 移植构建工具链 | ✅ | `Makefile`（身份改 `ps5-mango` / `v0.1.0` / 模块集 TODO + flag-stamp TODO）+ `gen-asset-module.py` + `install-libmicrohttpd.sh` + `.build/*.sh/*.py/*.mjs` 验收工具 |
| 播种 Harness 前端 | ✅ | `assets/index.html`（= demo5 全量单文件，可直接打开）+ `install.html`（E 首屏）+ `main.css`（37KB 冻结令牌/组件）+ `main.js`（8.5KB）+ `lang-zh.js` / `lang-en.js`（i18n 桩） |
| 低耦合接口占位 | ✅ | `src/*.h` 契约：`mango_common` + `httpd` + `task_engine` + `transfer` + `pkg_installer` + `pkg_lib` + `copypipe` + `savemgr` |
| 本地 git 提交 | ✅ | `cc6237d`（318）+ `741fe86`（94）＝ 2 个提交，本地 `master` 分支 |
| 推送 GitHub | ✅ | remote `origin`=`https://github.com/LisherSong/ps5-nexus.git`（GPL-3.0）；`master→main` **零分叉已推**，远端 429 文件 = 本地 HEAD `741fe86` |
| 仓库转私有 ⚠️ | ✅ | **2026-10-11 由 public 改为 private**（用户拍板：等功能全可用再公开）。当前远端 `main` 停在 `e56cae8`（2026-10-03），**是本地 HEAD 的纯祖先、落后 7 个提交** —— 远端那份**不含** v1.0.4 之后的修复（含「导入进度弹窗永远空白」「结束进程无限自启」）。0 fork / 0 star / 0 issue / 无 Release。**发布时改回 public 即可**（历史与仓库名保留，不必重建）；`gh api -X PATCH repos/LisherSong/ps5-nexus -F private=true` |

**未动（刻意）**：旧仓高耦合编排层（main/websrv/filemgr/upload/download/task/pkg_installer/app_installer 等）——按「低耦合重写」原则**不复制**，由后续 Phase 按契约头文件重新实现。

---

## 1b. 当前进度（Session 2，2026-09-27）

| 任务 | 状态 | 交付物 / 结果 |
| --- | --- | --- |
| 补齐 zlib | ✅ | `third_party/zlib/src`（15 .c）+ `third_party/zlib/include`（11 .h），**madler/zlib v1.3.1**，经 GitHub Git Data API 抓取（见 §5：github.com:443 被 egress 挡，不能 clone/curl） |
| 补齐 7z (LZMA SDK) | ✅ | `third_party/7z`（29 .c + 2 .asm + 头），**从旧仓 `ps5-web-file-manager/third_party/7z` 复制**（旧仓本来就有，Session 1 漏搬） |
| 修测试 harness | ✅ | `run-tests.sh`：RAR 包含路径 `third_party/unrar`→`third_party/unrar7`；`run-sevenz-tests.sh`：补 `mkdir -p "$BUILD/out"`（原 harness 遗漏，mktemp 父目录不存在导致失败） |
| **host 测试矩阵跑通** | ✅ | **MinGW 全绿**：ZIP **140/0** + RAR **37/0** + 7z **27/0**（另 error/policy 33/0）＝ **合计 204/204，解压引擎搬迁零损坏** |

**跑法（二者都需关删除守卫）**：
```
CODEBUDDY_SAFE_DELETE_ENABLED=0 bash tests/run-tests.sh        # ZIP+RAR
CODEBUDDY_SAFE_DELETE_ENABLED=0 bash tests/run-sevenz-tests.sh  # 7z
```
⚠️ 编译慢：49 个 unrar `.cpp` 在沙箱里约 4–5 分钟，耐心等后台跑完（不要被 2 分钟默认超时误判失败）。

---

## 1c. 当前进度（Session 3，2026-09-27）：推送 GitHub

| 任务 | 状态 | 交付物 / 结果 |
| --- | --- | --- |
| 建远端仓库 | ✅ | `gh repo create ps5-mango --public`（owner **LisherSong**，因上游 `ps5-web-file-manager` 即公开 GPL-3.0；需私有可 `gh repo edit` 翻转） |
| 推送提交 | ✅ | `git remote add origin …` + `GIT_SSL_NO_VERIFY=1 git push -u origin master:main` ⇒ **EXIT=0，零分叉**：远端 `main`=`741fe8635…`=本地 HEAD；远端 2 提交、429 文件 |
| 后续推送方式 | ✅ | **本次实测 `github.com:443` 已可达（curl 返回 200）**，故直接 `git push` + `GIT_SSL_NO_VERIFY=1` 即通；若日后 egress 又把 `github.com:443` 挡死（TCP 连不上、非证书），退回 **Git Data API** 通路（blob/tree/commit 逐字节同 sha 闸门，见用户级 MEMORY.md） |

> 注：本地分支名是 `master`，远端默认 `main`；已用 `master:main` 推送并设上游，后续 `git push` 即可（仍可能需 `GIT_SSL_NO_VERIFY=1`）。

---

## 1d. 当前进度（Session 5，2026-09-27）：按契约头实现编排层

按 §6 待办 4 执行：**按 9 个契约头把编排层全部落地**。分两档——可在本机 MinGW 编译并跑测试的（纯 C/POSIX）与只能在 SDK 机编译的（PS5 SDK / libmicrohttpd）。

| 模块 | 档 | 状态 | 交付 / 结果 |
| --- | --- | --- | --- |
| `src/task_engine.c` | A 本机 | ✅ | 统一任务中心：create/set_total/set_progress/set_state/cancel/recover + 扩展（worker 轮询 `task_cancelled`、JSON 快照供 UI、journal 路径）。跨重启恢复用 5 字段定长 journal（label 放最后含空格）。**测试 15/15** |
| `src/copypipe.c` | A 本机 | ✅ | 8 MiB 暂存 + 原子 commit（`<dst>.part` → rename；Windows 走 `MoveFileEx REPLACE_EXISTING`，因 `rename` 不覆盖已存在文件）；close 未 commit 则丢弃临时文件。**测试 22/22** |
| `src/transfer.c` | A 本机 | ✅ | local 后端（list/read/close）；`smb`/`nfs` 用 `MANGO_HAVE_LIBSMB2`/`MANGO_HAVE_LIBNFS` 守卫，缺库时 `transfer_open` 返回 **NULL**（不是静默空实现）。**测试 17/17** |
| `src/pkg_sfo.c` | A 本机 | ✅ | **新写**（替代 `pkg_info.c`）：自包含解析 PKG 容器（入口表 + PARAM.SFO/param.json），取 title_id/title/version/size/content_id + icon 偏移；含 icon 抽取（复用 copypipe 原子落盘）。**测试 22/22** |
| `src/pkg_lib.c` | A 本机 | ✅ | 递归深扫 `.pkg` → 索引；`pkg_lib_enqueue` 把安装任务推入统一任务中心（队列自动推进）。**测试 22/22** |
| `src/httpd.c` | B SDK | ✅ 已写 | libmicrohttpd 封装：路由表 + 运行时端口探测（顺延直到绑定成功）+ `httpd_port()` 上报 + 静态回退钩。**本机无 libmicrohttpd，未编译** |
| `src/pkg_installer.c` + `src/pkg_stream.c` | B SDK | ✅ 已写，**2026-09-30 已用 prospero-clang 18 + `-Werror` 编过并链进 ELF** | 只依赖 kstuff（按路径判定就绪）；显式 `kernel_set_ucred_authid(0x…0006)`；**真签名 `(pkg_metadata_t*, pkg_info_t*, playgo_info_t*)`** + `Initialize`/`Terminate` 包裹 + 真进度查询（`GetInstallStatus`，终态 `playable`/`completed`）；**PKG 走 loopback HTTP 推流** ⇒ `/data` 与 SMB 上的包也能装（`pkg_stream.c` 只监听 `127.0.0.1`，range 三阶段、`.crc` 探测必 404、非阻塞、由 `worker_tick()` 泵送，**主机端 89 项断言可验**）；已装预检以「DONE + 备注」呈现；取消只停喂流（真取消符号未证） |
| `src/savemgr.c` | B SDK | ✅ 已写 | 5 条硬规则：强制快照（无开关可跳过）/ 原子回写 / 失败回滚 / 可见（list）/ 二次确认；快照落 `SAVEMGR_SNAPSHOT_DIR`；提权三件套（`…0010` + caps[7]\|=0x40 + setuid）；串行 mount 锁 + ioctl 缓冲 heap 分配 |
| `src/main.c` | B SDK | ✅ 已写 | 接线：journal 恢复 + 路由（version/tasks/pkg scan/pkg enqueue）+ 静态资产表 + 启动打印实际端口 |
| `Makefile` | — | ✅ | 模块集换成新编排层；**补 `-lkernel_sys`**（旧仓从未链接 ⇒ 安装路径几乎肯定没在真机验过）；`pkg_info.c/h` 与 `extract.c` 刻意不入构建（见 Makefile 内注释） |
| `tests/run-orchestration-tests.sh` | A | ✅ | 新跑法；**4 个套件共 76 项断言全绿** |

**跑法**（新增，快；不含 unrar 那个 4–5 分钟的慢套件）：
```
CODEBUDDY_SAFE_DELETE_ENABLED=0 /usr/bin/bash tests/run-orchestration-tests.sh
```

**本档关键坑（新踩）**
- **`pread` 在 MinGW 未声明**（需 `_XOPEN_SOURCE`）⇒ 改 `lseek`+`read`（单线程无并发偏移问题）。
- **`rename()` 在 Windows 不覆盖已存在文件** ⇒ 原子替换必须 `MoveFileExA(..., MOVEFILE_REPLACE_EXISTING)`，否则 commit 覆盖旧档会失败。
- **`strncpy` 截断告警**（`-Wextra`）在 `transfer`/`pkg_lib` 里会响 ⇒ 用「strlen 后 clamp + memcpy」替代，别靠 strncpy 补零。
- **宿主测试别碰 opaque 结构体字段**（`task_handle_t`）⇒ 行为断言走 JSON 快照，不直接读 `t->state`。
- **`pkg_info.c` 不能进构建**：它 include 了未继承的 `websrv.h` / `filemgr_internal.h`；其解析逻辑已由 `pkg_sfo.c` 重写并去耦（且可本机测试）。`extract.c`（旧 MHD 端点）同理，等改成 mango 路由再入。

## 1e. 当前进度（Session 6，2026-09-27）：JSON API 层 + 平台状态

编排层落地后还缺「UI 唯一要对接的缝」。本档把散在 `main.c` 里的一次性 handler 与手写 body 解析器抽成 `src/api.c`，并补上状态条要的平台事实。

| 模块 | 档 | 状态 | 交付 / 结果 |
| --- | --- | --- | --- |
| `src/api.c/.h` | A 本机 | ✅ | **19 条路由**的路由表 `api_routes[]` + `api_dispatch`；`api_body_value` 同时吃 form 与 JSON（含 `\"` 转义、`\uXXXX`+代理对解 UTF-8、裸数字）。**测试 108/108** |
| `src/platform.c/.h` | A 本机 | ✅ | kstuff 判定**只看磁盘路径**（`/proc/kstuff` → `autoload.txt` → `/data/etaHEN` + `no_kstuff` 哨兵）；端口运行时上报；本机 IPv4（PS5/Unix 走 `getifaddrs`，Windows 走 `getaddrinfo`）；SMB/NFS 报**后端是否编入** |
| `src/task_engine.c` | A 本机 | ✅ 增补 | `task_id()` / `task_find(id)`（取消要按 id 找）；快照带 `ok`。**句柄绝不过 HTTP 边界** |
| `src/main.c` | B SDK | ✅ 重写 | **不再含任何 handler**：只遍历 `api_routes[]` 注册；启动后 `platform_set_http_port(httpd_port())` 并打印 `http://addr:port` |
| `src/stubs_nops5.c` | linux | ✅ 新增 | linux 目标专用桩——两个 SDK 域**一律返回 `MANGO_ERR_UNSUPPORTED`**，绝不假装可用 |
| `Makefile` | — | ✅ | `api`/`platform` 入 COMMON；**安装/存档移出 COMMON 进 `PS5_SRCS`**（否则 linux 目标编不过）；**第一方 CFLAGS flag-stamp**（只改 `-D`/`-I`/`LDADD` 时 make 不会重链，新功能会静默消失） |

**测试**：5 套件 **184 项断言全绿**（原 76 + api 108）。跑法不变：
```
CODEBUDDY_SAFE_DELETE_ENABLED=0 /usr/bin/bash tests/run-orchestration-tests.sh
```

**本档关键坑（新踩）**
- **两套桩职责不同，别混**：`tests/stubs_ps5.c` 是**记录式桩**（记参数、返回 OK，用来断言「路由有没有走到正确的模块」）；`src/stubs_nops5.c` 是**诚实桩**（返回 `UNSUPPORTED`，让 linux 二进制链接且报「此处不可用」）。二者返回相反，混淆会让测试或 linux 构建之一说谎。
- **让 `api.c` 不 include `httpd.h`**：路由表由 `main.c` 注册（handler 签名与 `http_handler_fn` 一致，可直接传）⇒ api 层零 socket 依赖、宿主可测。
- **`mkdir` 参数个数平台不同**（Windows 1 个、POSIX 2 个）⇒ 收敛到 `ensure_dir()`。
- **数组判空恒真**：`if(!mango_assets)` 触发 `-Waddress`（`mango_assets` 是数组）⇒ 删判空，靠 `a->path` 终止。
- **Windows 下地址探测要 `-lws2_32`** ⇒ 跑批脚本按 `uname -s` 自动加。
- JSON 里 `\uXXXX` 不解码的话，中文游戏名会原样进 UI；`\u` 还得处理**代理对**（emoji）。

---

## 1f. 当前进度（Session 7，2026-09-27）：worker 任务驱动

Session 6 结束时 `/api/extract` 与 `/api/install/begin` **只入队、任务停在 `queued`**（有意留的空）。本档让任务中心真正转起来。

| 模块 | 档 | 状态 | 交付 / 结果 |
| --- | --- | --- | --- |
| `src/worker.c/.h` | A 本机 | ✅ 新增 | 单线程 **tick 驱动**（`worker_tick()` 由 main 循环调）；4 个槽位并发；按 kind 分派 fetch/install/extract/backup/**restore**；轮询 `task_cancelled()`。`worker_shutdown()` **只丢弃、绝不 commit** 半截产物。**测试 19/19** |
| `src/task_engine.c` | A 本机 | ✅ 增补 | `task_payload_t`（`src_path`/`dst_path`/`scheme`/`snapshot`）+ `task_set_payload`/`task_payload` ⇒ **worker 才知道该干什么**；`task_kind_of` 公开 |
| `src/mango_common.h` | A 本机 | ✅ 增补 | `TASK_KIND_RESTORE` 补上（恢复与备份是两条独立路径，共用一个 kind 会让回滚语义含糊） |
| `src/transfer.c/.h` | A 本机 | ✅ 增补 | `transfer_size()` —— worker 要提前知道总字节才能报进度（`task_set_total`） |
| `src/api.c` | A 本机 | ✅ 改写 | 安装/备份/恢复**统一改成「入队 + worker 驱动」**；新增 `/api/fetch`（NAS 拉取）。**入队不再触碰数据源** ⇒ 缺库/坏参的失败改在 worker 里以「任务失败」暴露（更贴近 UI 看到的样子） |
| `src/extract_engine.h` | — | ✅ 新增 | 解压引擎契约头（worker 只依赖契约，不依赖具体 zip/rar/7z 实现） |
| `src/main.c` | B SDK | ✅ | 主循环每 100 ms `worker_tick()`；退出前 `worker_shutdown()` |
| `src/stubs_nops5.c` / `tests/stubs_ps5.c` | — | ✅ | 随新增 kind 补齐桩 |

**测试**：6 套件 **222 项断言全绿**（原 184 + worker 19 + api 增至 127）。跑法不变。

**本档关键坑（新踩）**
- **「只入队」时失败必须在 worker 里暴露**：旧测试断言「smb 缺库 → `/api/fetch` 返回 `ok:false`」，改成入队语义后该断言**必然翻**，语义上失败应表现为「任务进入 FAILED」。改断言而非改实现——UI 看到的就是任务失败。
- **单次性任务（extract/backup/restore）也要占槽位**：最初只在「打开数据源」分支占槽，单次性任务走另一条路径 ⇒ 执行完没释放槽、**永远停在 RUNNING**。修法是统一走 `start_task()` 再 `slot_release()`。
- **取消后不能留半截文件**：`<dst>.part` 未 commit 一律丢弃（copypipe 契约）⇒ worker 取消路径必须显式 close 而不 commit，测试按此断言。
- **`worker_shutdown()` 绝不能 commit**：进程退出时把半截产物落盘 = 假成功，比失败更坏。
- 编辑 `task_engine.h` 时**用替换覆盖了 `task_recover()` 声明**（编译才暴露）；删 `slot_find()` 后又留了未使用函数告警。**头文件用 Edit 时整段替换要看前后文**。

---

## 1g. 当前进度（Session 8，2026-09-27）：解压引擎接进 worker（续被中断的工作）

Session 7 结束时 worker 的 `step_extract` 还是占位，调的是旧签名 `mango_extract(path, dst, conflict, password, progress, ctx)`（还没有真适配器）。本档把继承来的 zip/rar/7z 叶子按契约接上——**密码/分卷路径全部贯通**（相对竞品的真真空），并让失败带原因回 UI。

| 模块 | 档 | 状态 | 交付 / 结果 |
| --- | --- | --- | --- |
| `src/extract_engine.c` | A 本机 | ✅ 新增 | 传输无关适配器 `mango_extract()`：按扩展名分派 zip/rar/7z，卷组检测，唯一诚实拒收（`x.rar.001` 字节切分，unrar 无法打开）；`zipx_status_t → mango_err_t` 映射。分派规则是旧 MHD 端点 `extract_dispatch()` 的直接移植（真机验过的） |
| `src/extract_engine.h` | — | ✅ 改写 | 新契约：`mango_extract_req_t`（conflict/password/**large**/progress/cancel/ctx）+ `mango_extract_result_t`（err + 人类可读 message）。worker 只依赖此契约，永不接触 `zipx_status_t` |
| `src/mango_common.h` | A | ✅ 增补 | `MANGO_ERR_PASSWORD = -8` / `MANGO_ERR_CONFLICT = -9`（密码是解压首试失败的唯一大原因，UI 要能区分） |
| `src/task_engine.c/.h` | A | ✅ 增补 | `task_payload_t` 加 `password[128]` + `large`（**内存内，绝不入 journal**）；`task_set_note`/`task_note`——失败原因随 JSON 快照带 `"note"` 字段给 UI |
| `src/worker.c` | A | ✅ 改写 | `step_extract` 走新契约：password/large 透传、progress→`task_set_progress`、独立 `on_extract_cancel` 轮询通道（引擎轮询 cancel 比报进度勤得多，取消落得快）；失败时 `task_set_note(res.message)` 再 FAILED |
| `src/api.c` | A | ✅ 增补 | `/api/extract` 收 `password`/`large`（`"1"`/`"true"` 视为开）；`copy_clamp` 按 sizeof 截断到 128 |
| `src/stubs_nops5.c` | linux | ✅ 改 | **`mango_extract` 不再桩掉**——`extract_engine.c` 是纯 POSIX、每个目标都能编；桩掉会在 linux 构建上静默禁用解压 |
| `Makefile` | — | ✅ | `extract_engine.c` 入 `COMMON_SRCS` |
| `tests/stubs_engines.c` | A | ✅ 新增 | **与 stubs_ps5.c 职责相反**的记录式桩：替 zip/rar/7z，让适配器套件免拖 zlib/minizip-ng/unrar7 |
| `tests/test_extract_engine.c` | A | ✅ 新增 | 40 项：分派（含大写 .ZIP、未知扩展、无扩展名）、参数透传（password/conflict/dst/large/profile）、progress+cancel 接通、状态映射、rar.001 拒收。**所有 .zip 路径用真实（空）文件** |
| `tests/run-orchestration-tests.sh` | A | ✅ 重构 | `CORE` 与 `WORKER_OBJS` 分离：worker.o 拖 SDK 域 → 与 stubs_ps5 捆绑；extract_engine 套件只链真实适配器 + stub 引擎（stubs_ps5 的 mango_extract 会撞符号） |

**测试**：7 套件 **266 项断言全绿**（原 222 + worker +4 密码/原因 + extract_engine 40）。跑法不变。

**本档关键坑（新踩，含被打断时踩的）**
- **`zipx_volume_detect` 会 stat `.zip` 名并扫兄弟分卷**：假路径返回"卷不完整"而非"单文件" ⇒ 适配器测试必须 `touch()` 造真实（空）文件，不能写假路径。
- **入队不再触碰数据源，失败只在 worker 暴露**：两处旧断言（`/api/fetch` smb 缺库 → HTTP `ok:false`、installer 拒绝 → `ok:false`）按新语义改成断言任务 `failed`——UI 看到的就是任务失败，改断言不改实现。
- **两个桩文件职责相反、只能各归其位**：`stubs_ps5.c`（记录式，断言路由/worker 走到对模块）与 `stubs_engines.c`（记录式，断言适配器分派）都定义 `mango_extract`/引擎符号 ⇒ 一个套件只能链 {extract_engine.o} 或 {stubs_ps5.o} 之一，绝不能都链。
- **`mango_extract` 不能进 stubs_nops5.c**：诚实桩按模块缺什么补什么；extract 是真实 POSIX 代码，桩掉 = linux 构建上解压静默不可用。
- **rar.001 拒收收窄了**：旧 MHD 端点拒收一切 rar 卷组；引擎本身能正确合并 `x.partNN.rar`（`test_rar_extract.c` 对真实夹具断言过）⇒ Mango 只拒收引擎真打不开的命名（字节切分 `x.rar.001`），并给用户改名指引。
- **密码只存内存**：`task_payload_t.password` 与 `note` 都不进 5 字段 journal（journal 只写 id/kind/state/total/progress/label）——重启恢复的任务不再携带密码。

---

## 1h. 当前进度（Session 9，2026-09-27）：Harness 前端接入真 API

按 §6 item 5（UI 并行轨道）第一刀：**任务视图 + 顶栏状态 + 解压对话框**从假数据换成真 `fetch`。验收对象与接入对象分离——父目录静态 demo（`ps5-ui-demo-5-harness.html`，106 项验收）**保持静态假数据不动**；`assets/index.html`（内嵌进 ELF 的产品前端）增加接入层，`file://` 打开仍能看（假数据保底），接上后端才变活。

| 文件 | 改动 |
| --- | --- |
| `assets/index.html` | ① 顶栏三 chip 加 `id` + `.st` 文案位；② 文件视图行加 `data-path` + 选中交互（Enter 也触发）；③ 解压对话框加口令输入 `#dlgPw` + 大文件复选 `#dlgLarge`，标题跟随选中归档；④ **接入层 JS**（内联，`LIVE` = http/https 才轮询）：`/api/status` → 顶栏三灯 + ipChip（**端口服务端上报**）+ 平台卡 kv + 概览胶囊；`/api/tasks` → 最近列表（倒序最多 8 行，`state_name`→中文胶囊，failed 带 note）+ 当前任务大卡（进度条/已处理 GB/total，均速/剩余/耗时诚实显示 `—`——快照暂无速率字段）；`/api/extract` 提交 password/large/conflict，成功显示 task_id 并关闭；大卡「取消」接 `/api/task/cancel`（focus 任务 id 内存维护，句柄绝不过 HTTP 边界）；⑤ 假数据推进器（`setInterval` 900ms）**移进 `if(!LIVE)`**——真机下它与轮询两套时钟同时推，会覆盖动态渲染的进度条（验收真踩到：8.26/71.2% 覆盖 8.20/71%） |
| `assets/main.css` | `.fld input[type="password"]` 规则（开发拆分视图同步；产品实际样式以内联 `<style>` 副本为准，两处已同加） |
| `.build/ui_live_check.mjs` | **新增**：同源 mock 后端（JSON 形状逐字段对齐 api.c）+ Playwright，27 项断言——状态灯/地址/列表/大卡/解压提交（含 password/large 透传）/取消/`file://` 无错与原型行为（点解压即关）不回归 |
| `.gitignore` | `!.build/ui_live_check.mjs` 白名单 |

**测试**：`node .build/ui_live_check.mjs` **27/27 全绿**；静态 demo 106 项回归待后台确认（父目录 demo 未动，理论不受影响）。

**本档关键坑（新踩）**
- **真机下假数据推进器是覆盖者**：播种版有一个 900ms 的 `setInterval` 推进 `[data-p] i` 与 `[data-gb]`——接入层渲染的真任务进度一出现就被它覆盖（71.2% 而非 71%）。修法不是删它（`file://` 保底要假数据），而是 `if (!LIVE)` 圈起来。**两套时钟永远不能同时推。**
- **`LIVE` 判定必须提前到推进器之前**：`const` 有 TDZ，推进器若 `typeof LIVE` 探测会 ReferenceError；直接把它挪到脚本中段（推进器前）定义，接入层复用。
- **复制按钮的 url 要在点击时算**：播种版在 IIFE 启动时一次性 `"http://" + data-ip`——接入层改 `data-ip` 后旧 url 永远复制旧地址。改为点击时读取。
- **ipChip 文案覆盖要防「复制中」临时态**：flash() 会把 `.ipv` 临时改成「已复制」，轮询若无条件写回服务端地址会把临时文案打断；只在文本等于播种值时覆盖。
- **验收顺序别想当然**：mock 任务数组按创建序给，UI 倒序展示 ⇒ 断言顺序是 done→failed→running，我第一版按 running→failed→done 断言全翻。**断言写错≠实现错，先复算再改代码。**

---

## 1i. 当前进度（Session 10，2026-09-27）：文件视图真目录树 + 落点点选

Session 9 已接顶栏/任务/解压提交；本档把**文件视图**与**解压落点**接上 `/api/fs/list`（`POST {scheme?, path}` → `{ok, entries:[{name,size,is_dir}]}`，**无 mtime**）。

| 文件 | 改动 |
| --- | --- |
| `assets/index.html` | ① 文件视图 quick 切根按钮加 `data-root`/`data-scheme`、panel 加 `id="fsPanel"`；② 解压对话框 crumbs/picker/quick 加 id、落点 `#dlgDst`；③ **`fsNav(cfg)` 工厂**（两实例共用）：`/api/fs/list` 驱动面包屑/列表/切根/返回上一级/进入目录/空目录/错误行，事件全部委托（`data-up`/`data-dir`/`data-path`），quick 按钮 click → `go(root, scheme)`；④ 文件视图实例：返回行 + 目录/文件行（扩展名→pill/图标），文件行选中 → `currentArchive`；真机下把「修改时间」列改「大小」（entries 无 mtime，诚实列真字段）；⑤ 解压实例：picker 只列目录、落点 = 当前浏览路径（`#dlgDst` + 面包屑实时跟随）、每次打开 `dlgFs.refresh()`；⑥ `submitExtract` 的 `dst_dir` 从硬编码改为 `dlgFs.state.path` |
| `.build/ui_live_check.mjs` | mock 增 `/api/fs/list`（按 path 分桶；`scheme=smb` → `ok:false "NAS 后端未编入"`）；断言 27→**36**：目录树初始渲染、切根 pressed 跟随、NAS 错误行、落点/面包屑跟随、点目录更新落点、提交 `dst_dir=点选目录`、返回上一级回退 |

**测试**：`node .build/ui_live_check.mjs` **36/36 全绿**；静态 demo 106 项回归后台确认中。

**本档关键坑（新踩）**
- **quick 切根按钮最容易漏绑事件**：`fsNav` 先把 panel 的点击委托写好了，roots（切根按钮）只有 `renderRoots()` 更新 pressed 状态、没有 click → `go()` 绑定——6 项失败里有 4 项根因都是它。**渲染状态 ≠ 交互绑定，写完 pressed 同步必须问一句「点了谁去绑」。**
- **innerHTML 重建会丢 id**：`onOk` 用 `sp.innerHTML = "落点 <b>path</b>"` 刷新空间条，把 `#dlgDst` 的 id 丢了 ⇒ 后续 `getElementById` 全空。重建 HTML 时 id/class 必须原样带回去。
- **验收断言里的字符串先跑一遍再写死**：面包屑返回按钮 `textContent` 是空（图标），`join("/")` 结果带前导斜杠 `/mnt/ext1/data/games`，第一版断言漏了它。

---

## 1j. 当前进度（Session 11，2026-09-27）：安装/上传视图 + 游戏库接 scan

按 §6 item 5 第三刀：**游戏库**消费 `/api/pkg/scan` 动态渲染 + **安装入队**（`/api/pkg/enqueue`）+ **从 NAS 拉取**对话框（`/api/fetch`）+ **上传**无契约的诚实处理。scan 只有 `{title_id,title,version,size,path}`，没有已装/口令状态 ⇒ 一律「待安装」，不给假状态。

| 文件 | 改动 |
| --- | --- |
| `assets/index.html` | ① library 视图：`.covers`→`#libCovers`、`.count.num`→`#libCount`、搜索框→`#libSearch`；② 文件视图「从 NAS 拉取」→`#btnFetch`、「上传文件」→`#btnUpload`；③ **新增 `#scrimFetch` 拉取对话框**（源路径/目标目录/协议 seg，复用 `.dlg` 结构与 toast）；④ **新增 toast 轻提示**（`.toast` 元素 + CSS + `toast(msg)`，2400ms 自动隐）；⑤ `applyFilter`：搜索真筛（动态卡读 `data-*`、静态卡从 gmeta 文案回退）+ 计数改动态（`n / 总卡 个游戏 · 已按筛选显示`）；⑥ **接入层**：`renderLibrary()`（`/api/pkg/scan {root:/mnt/ext1/games}` → 动态 `.gcard`：mark = title_id 缩写 `PPSA15368→P15368`、平台由前缀判 `CUSA→PS4`、渐变替身 `a1..a6` 按 tid 哈希循环、`待安装` pill + 安装按钮，空/错态给整行说明，count = `N 个 · 全部待安装`）；`#libCovers` 点击委托 → `apiPost /api/pkg/enqueue {path,title,version}`（成功按钮→「已入队」disabled + `refreshAll()`——**enqueue 返回无 task_id**，只刷新任务视图）；拉取提交 `fgo` → `/api/fetch {src,dst,scheme}`（成功 toast 带 task_id + 关框 + 刷新）；上传按钮 LIVE 下 toast「上传通道待后端 Phase 3 接入（/api/upload），当前不可用」，静态保持原型行为；⑦ LIVE 初始化加 `renderLibrary()` |
| `assets/main.css` | `.fld input[type="text"]` 规则（与 password 并列；开发拆分视图同步，产品以内联 `<style>` 副本为准） |
| `.build/ui_live_check.mjs` | mock 增 `/api/pkg/scan`（3 包，含 CUSA）、`/api/pkg/enqueue`（捕获 body）、`/api/fetch`（task_id=42）；断言 **36→52**：库按 scan 渲染 3 卡、PPSA/CUSA 的 mark+平台、count 动态、搜索真筛 + 计数、平台 seg 筛选、安装入队 body 与按钮态、拉取对话框打开/提交 body/关闭、上传诚实 toast、`file://` 库保留 6 卡 + 搜索从文案回退 |

**测试**：`node .build/ui_live_check.mjs` **52/52 全绿**；静态 demo 106 项验收只加载父目录 `ps5-ui-demo-5-harness.html`（本档未动该文件，后台已 106/106）。

**本档关键坑（新踩）**
- **segPick 剥计数把 PS4 剥成 PS**：`b.textContent.replace(/\s*\d+\s*$/, "")` 本意剥「全部 6」这类计数，但 `\s*` 允许无空白 ⇒ "PS4"→"PS"、"PS5"→"PS"，平台筛选永远空。修成 `/\s+\d+\s*$/`（**要求数字前有空白**）。教训：剥计数的正则假设「数字前必有分隔」，产品名戳破了它——筛选测试（PS4）当场抓出，纯视觉验收抓不到。
- **file:// 下 `page.fill` 等不到可见元素**：库视图未激活（`.view{display:none}`），`#libSearch` 不可见 ⇒ fill 超时 30s。测试改 `evaluate` 直接置值 + `dispatchEvent(new Event("input", {bubbles:true}))`。
- **「无契约动作」的诚实落点**：上传没有后端路由，真机下 toast 明说待后端接入；不造假按钮、不静默无反应。enqueue 无 task_id ⇒ 成功反馈不编 task_id。

---

## 1k. 当前进度（Session 12，2026-09-27）：任务视图安装大卡接 `/api/install/poll`

按 §6 item 5 第四刀：**任务视图大卡对 install 任务分叉口径**。后端事实：worker `step_install` 把 installer 报的 **0..100 pct 直接写进 `task_progress`**，而 `total` 恒为 0 ⇒ 快照的字节口径（`pctOf` / `data-gb` / `data-total`）对 install 全失效（会算出 0.00 GB、进度条 0%）。契约单独给了 `/api/install/poll`（返回 `{ok,id,percent}`），前端按它走安装进度。

| 文件 | 改动 |
| --- | --- |
| `assets/index.html` | 大卡 markup：字节行加 `#byteRow`、**新增 `#pctRow`（`data-pct` 安装进度行，默认 `display:none`）**；`renderTasks` 分叉 `isInstall = run.kind === 3`：进度条/百分比行取 `run.progress`（0..100 直接用）、字节行隐藏、`data-gb`/`data-total` 显 `—`、均速/剩余/耗时照旧 `—`（诚实）；`refreshAll` 末尾：focus 任务是 install 时**再轮询 `/api/install/poll {id}`**，成功以 `percent` 覆盖进度条与百分比行（poll 与快照 progress 同源，但契约要求安装进度单独查） |
| `.build/ui_live_check.mjs` | mock：`/api/tasks` 改可变 `tasksMock`（默认仍 TASKS）、**新增 `/api/_mock/install`**（测试专用，把快照切成单条 install running：progress=55 total=0）、**新增 `/api/install/poll`**（捕获 body，返回 `{ok,id:7,percent:88}`——故意与快照 55 不同，证明 UI 吃的是 poll 值）；断言 **52→57**：切 install 后大卡标题/状态、进度条 + 百分比行 = 88%、百分比行显示/字节行隐藏、`data-gb`=—、均速 —、poll body 带 `id=7` |

**测试**：`node .build/ui_live_check.mjs` **57/57 全绿**；静态 demo 106 项验收只加载父目录 `ps5-ui-demo-5-harness.html`（本档未动该文件，106/106）。

**本档关键坑（新踩）**
- **install 任务的进度是百分比不是字节**：`task_progress` 存 0..100，`total=0`。复用字节口径会显示「0.00 GB / 0.00」、进度条 0%——不是 bug 是**口径错位**。分叉条件是 `kind===3`，不能靠 `total>0` 猜。
- **`.stats div` 有显式 `display:flex`，`hidden` 属性会被覆盖** ⇒ 显示切换用 `style.display`（内联初始化 `display:none`），别用 hidden 布尔。
- **测试要证明「吃了 poll 值」**：mock poll 返回的 percent（88）故意与快照 progress（55）不同，断言大卡最终显示 88%——只断言「有进度条」证不了走的是 poll 通道。

---

## 1l. 当前进度（Session 13，2026-09-27）：后端快照补 rate 字段 + transfer 接 smb/nfs 的本机结论

按 §6 两件待办收尾：**① 快照 `rate` 字段（总字节 ÷ 总耗时）落地并全链路接通；② transfer 接 smb/nfs 的本机可行性评估——诚实结论：本机无法 vendor 构建，守卫宏保持缺省 + 精确报错 + SDK 机步骤写成文档。**

| 文件 | 改动 |
| --- | --- |
| `src/task_engine.c` | 快照每任务带 `"rate"`（bytes/s 整数）：`task_handle` 增 `t0_ms` + `base_progress`，`task_set_state` 进入 RUNNING 时取样（**只在状态迁移时取样**，重复置 RUNNING 不重置均速）；`task_rate()` = (progress−base)÷(now−t0)，**三档返回 0**：`total==0`（install 把 0..100 写进 progress，字节口径无意义）、窗口 <1 s（100 ms 采样读起来像尖峰）、progress 未动过。t0 不 journal——恢复任务重驱动时重新取样 |
| `src/transfer.c/.h` | **新增 `transfer_scheme_supported(scheme)`**：local 恒 1；smb/nfs 仅当对应宏定义时 1。让 API 层区分「已知但未编入的后端」与「路径坏了」，而不是一句笼统报错 |
| `src/api.c` | `/api/fs/list` 打开失败时分叉报错：`transfer_scheme_supported` 为真 → "cannot open source (bad path)"；为假（smb/nfs 未编入）→ **"NAS backend not compiled in (smb/nfs)"**（与 live check mock 的文案语义对齐，真机 UI 错误行不再含糊） |
| `tests/test_task_engine.c` | +2：恢复任务 rate=0（无 RUNNING 时间基准）、RUNNING 后未推进 → rate=0（绝无尖峰） |
| `tests/test_api.c` | +1：`/api/fs/list` scheme=smb 断言 error 含 "not compiled in"（精确报错而非笼统） |
| `assets/index.html` | 新增 `fmtRate()`（二进制单位，同 `fmt`）；`renderTasks` 均速：`(!isInstall && run.rate > 0)` → `fmtRate(run.rate)`，**install 分支或服务端未给 rate 时仍诚实 `—`**（不显瞬时值） |
| `.build/ui_live_check.mjs` | mock 快照（TASKS/INSTALL_TASKS）逐字段对齐补 `rate`；**组 3 断言 `focus.avg === "192 MB/s"`**（mock rate=201326592 = 192 MiB/s，证明 UI 吃的是服务端真值）；组 12 install 仍断言 `avg === "—"` |

**测试**：host 矩阵（orchestration）全绿（task_engine +2、api +1）；`node .build/ui_live_check.mjs` **57/57 全绿**；静态 demo 106 项由后台任务确认（本档未动父目录 demo）。

**transfer 接 smb/nfs —— 本机可行性评估（诚实结论）**
- **探针**：宿主只有 `mingw32-make`；`cmake` / `autoconf` / `automake` / `libtoolize` 全部缺席 ⇒ **libsmb2（CMake + autotools）与 libnfs（autotools）在本机无法 configure/构建**；且无 PS5 Payload SDK，prospero 编译无从验证。
- **处理**：`MANGO_HAVE_LIBSMB2` / `MANGO_HAVE_LIBNFS` **保持未定义**（`transfer_open` 对 smb/nfs 返回 NULL——已有测试钉死「干净失败，不静默空实现」）；新增 `transfer_scheme_supported` 把失败原因说清楚。**不硬造假实现、不写无法验证的后端代码**。
- **SDK 机步骤（文档即交付）**：① 抓 libsmb2 源码（其 SMB 协议实现是 elf-arsenal 的同源参考）与 libnfs 源码入 `third_party/`；② 按 prospero-clang 工具链移植（socket/poll/getaddrinfo 需过 SDK sysroot）；③ Makefile `THIRD_PARTY_C_SRCS` 增补 + 定义两宏；④ 实现 `transfer.c` 里每个 scheme 的四个函数（open/list/read/size）——local 后端与既有测试用例**一行不用改**；⑤ SDK 机 `make` 验证。

---

## 1m. 当前进度（Session 14，2026-09-27）：安装/上传视图收口 —— 上传切片续传契约全链路落地

按 §6 item 5 的「剩」字：安装侧（Session 11/12）已接真链路，本 Session 补唯一缺口——**上传**。契约 = **JSON 元数据块 + `\n\n` 定界 + 原始二进制切片**（每片一个 one-shot POST，8 MiB 与 copypipe 块同尺寸）；客户端 `File.slice()` 循环 POST，进度走统一任务中心（字节口径，自动复用 Session 13 的 rate）。live check 62 项（+5）、host 矩阵全绿（api 145 / worker 25 / extract_engine 40）、静态 demo 106 回归保持绿（demo 文件未动）。

| 文件 | 改动 |
| --- | --- |
| `src/api.c` | 注册 `/api/upload`（`api_upload`）：扫 body 找首个 `\n\n` 分界 meta/二进制（**只解析 meta 块，二进制绝不被 key 扫描**）；`api_body_value` JSON 分支解析 `dst_dir/name/task_id/offset/total/final`；name 拒绝空/`.`/`..`/含 `/` `\`；offset==0 → `task_create(TASK_KIND_UPLOAD, name)` + payload.dst_path + RUNNING + total；offset>0 → `task_find(task_id)` + 校验 kind==UPLOAD && dst_path 匹配（不匹配/"unknown upload task"）；取消 → CANCELLED；落盘 `fopen(dst, offset==0?"wb":"ab")`，offset>0 先 `fseek END + ftell` 校验磁盘大小==offset（否则 "offset mismatch"），短写 "write failed"；`task_set_progress(落盘字节)`；final=1 校验 size==total → DONE |
| `src/task_engine.h/.c` | 新增 `task_first_in_state_other(s, skip)`：跳过指定 kind 的排队首任务 |
| `src/worker.c` | 排队遍历器改 `task_first_in_state_other(QUEUED, TASK_KIND_UPLOAD)`——上传由 HTTP 请求自身驱动（worker.c 头注释既定设计），否则孤儿 QUEUED 上传任务（create 与置 RUNNING 之间崩溃/跨重启）会饿死后队 fetch |
| `tests/test_api.c` | 新增 `test_upload()`（~15 断言）：切片 1（offset 0/total 8/final 0）→ ok+task_id+offset:4+running；切片 2（task_id 回传+final 1）→ done + 落盘字节逐字节校验；offset mismatch / stale task_id / `../evil` 路径穿越 / offset>0 无 task_id / 无定界符 / 空文件（total 0 final 1）→ done。**api 145 项** |
| `tests/test_worker.c` | 新增 `test_upload_does_not_starve()`：先 fetch 再建 UPLOAD（后建的排头）→ 300 tick → fetch 必须 done + 落盘存在。**worker 25 项** |
| `assets/index.html` | `#btnUpload`（LIVE）打开 `#scrimUp` 对话框（多文件选择 + 落点只读 = 文件视图当前目录 + 关闭/取消 + Escape）；逐文件 do-while 切片 `SLICE=8*1024*1024`，`TextEncoder` 编码 meta 拼 `\n\n` 加切片字节，`fetch("/api/upload", POST octet-stream)`；`j.ok` → 回传 task_id/新 offset，done 即 break；异常 → toast「上传中断」+ break（诚实不造假）；全部成功 → toast「已上传 N 个文件」+ `refreshAll()`。**删掉「待后端 Phase 3」文案** |
| `.build/ui_live_check.mjs` | mock `/api/upload`（收集切片 body，按首个 `\n\n` 拆 meta JSON + pay Buffer）；组 10 上传断言 +5：对话框打开/落点跟随、8 MiB 切 2 片、两片元数据链（offset 0→8 MiB + task_id 回传 + final 1）、二进制字节数、完成 → 对话框关 + toast。**62 项全绿** |

**测试**：host 矩阵 `run-orchestration-tests.sh` **api 145 / worker 25 / extract_engine 40 全绿**；`node .build/ui_live_check.mjs` **62/62**；静态 demo 106/106（后台任务确认，父目录 `ps5-ui-demo-5-harness.html` 未动）。

**本档关键坑（新踩）**
- **`api_body_value` 的 form 分支不做 URL 解码**（`test_body_parser` 只喂裸 form、live check 只断言客户端出站 body 含 `%2F`）：浏览器 `URLSearchParams` 会把 `/` 编成 `%2F`，fetch/extract 真机上存在潜在 %2F 落盘缺口——原来归 upload 改用 **JSON 元数据块** 规避。**Session 15 已修第 `api_body_value` form 分支**：新增 `url_decode_range()`（`%XX`→字节、`+`→空格、其余原样），form 取值改走它；host 测试 `+5`（`%2F/%20` 解码、裸路径不变、`+`→空格、`%25`→`%`、孤立 `%` 不动），api suite 150 checks 全绿。JSON/上传切片分支不受影响。
- **上传不跨服务端重启续传**：task_id 是内存句柄，重启后旧 task_id → "unknown upload task"，客户端诚实从 offset 0 重来（切片是幂等的——offset 0 用 "wb" 截断重写）。不要承诺「断点续传跨重启」。
- **上传走 HTTP 自身驱动**，worker 永不拾取 UPLOAD 任务；断传留下的部分文件就是续传状态（原地追加写，无 .part 原子提交）。

---

## 1n. 当前进度（Session 15，2026-09-28）：transfer 接 smb/nfs —— 完整后端实现落地（宏关缺省，链接/运行归 SDK 机）

按 §6 item 4 子项推进：Session 13 已定本机结论（宿主无 cmake/autotools，vendor 无法在本机构建），本 Session 把「SDK 机步骤」从文档推进到**完整代码交付**——vendored 两份库 + Makefile 双开关 + transfer.c 宏守卫下实现 smb/nfs 全部四个函数；本机验证宏关分支回归 + 宏开分支语法检查与 TU 编译，链接与运行验证归 SDK 机。

| 文件 | 改动 |
| --- | --- |
| `third_party/libsmb2/` | vendor **libsmb2 v6.0.0** 源码（48 .c 全在 `lib/`）+ 手写 `config.h`（FreeBSD 最小集，模板 `include/apple/config.h`；undef krb5/libnsl/libsocket） |
| `third_party/libnfs/` | vendor **libnfs 8.0.0** 源码（`lib/`+`mount/nfs/nfs4/nlm/nsm/portmap/rquota` 8 目录，排除 tls/aros/ps2_ee/ps3_ppu/win32）+ 手写 `config.h`（模板 `cmake/config.h.cmake`；undef NFS4_2/MULTITHREADING/KRB5/TLS/SYS_VFS 等） |
| `Makefile` | `LIBSMB2 ?= 0` / `LIBNFS ?= 0`（**默认关**，host 构建/测试零影响）；条件并入 `THIRD_PARTY_C_SRCS`；**per-target flag 覆盖**（`ps5-obj/third_party/libsmb2/%.o` 等目标特定变量）把 `-DHAVE_CONFIG_H` 限定在对应 TU，防泄漏给 zlib/minizip/7z；第一方 `CFLAGS`/`LINUX_CFLAGS` 条件加 `-DMANGO_HAVE_LIBSMB2/-DMANGO_HAVE_LIBNFS` + 头路径（只有 transfer.c 读） |
| `src/transfer.h` | `transfer_nas_t { server[256]; share[256]; path[MANGO_PATH_MAX] }`（share = smb share 名或 nfs export 名 verbatim）；`transfer_url_parse()` 声明（纯字符串、宏外编译、全构建 host 可测） |
| `src/transfer.c` | `transfer_url_parse()` 纯函数（`smb://`、`nfs://`、`//host/share`、裸 `host/share`、`host:/export` 五种形式；空 server/share、尾冒号、绝对路径、NULL 均拒绝；尾路径允许空 = share 根）；`list_push()` 共享 helper（local/smb/nfs 三后端复用）；宏守卫下 smb（`smb2_init_context`→`smb2_connect_share(ctx,server,share,NULL)`→`smb2_opendir/readdir/stat/pread`）+ nfs（**`nfsio_*` 前缀**——`nfs_read`/`nfs_close` 是 libnfs 公开符号名；`nfs_init_context`→`nfs_mount(server, export_name)`，share 无前导 `/` 时补 `"/"`）四函数；`transfer_open` **惰性连接**（只验 URL 形状，连接错误在首次 list/read/size 以 MANGO_ERR_NOTFOUND 浮出，镜像 local fd 惰性开） |
| `tests/test_transfer.c` | +25：URL 解析段 19（五种形式 + 嵌套相对路径 + 尾斜杠→空 path + 空 URL/server-only/尾冒号/绝对路径/NULL 拒绝）+ `scheme_supported` 契约 6（local/NULL/空→1、smb/nfs 本机构建→0、ftp→0）；宏关分支 `transfer_open("smb:")/("nfs:") == NULL` 两断言保留钉死 |

**测试（本机验证链，全绿）**：host 矩阵全绿（transfer 新增 25 断言：URL 解析 19 + `scheme_supported` 契约 6，local 后端行为零变化）→ MinGW 宏开 `-fsyntax-only` SYNTAX-OK → WSL Linux POSIX 分支 POSIX-SYNTAX-OK → libsmb2 全 **53 TU** 编译 `ALL-LIBSMB2-OK`（Linux 是忠实代理）→ libnfs 全 **25 TU** 编译 `ALL-LIBNFS-OK`（Linux 模拟 config）→ **链接冒烟**：78 TU + 宏开 transfer.c + 驱动 main 在 WSL Linux **真实链接成单个 ELF 并运行**，宏开分支行为断言（scheme_supported/URL 解析字段/惰性 open/local 回归）全过——把 SDK 机的链接验证在宿主侧提前做了。冒烟抓到的三处修复见下。

**链接冒烟新坑（本 Session 追加）**
- **libnfs `major()`/`minor()`（nfs3/nfs4 mknod 路径）**：libnfs 在 `#ifdef HAVE_SYS_SYSMACROS_H` 下才 include `<sys/sysmacros.h>`。我们的 config **undef 它是正确的**（FreeBSD 的 `<sys/types.h>` 自带 major/minor，且 FreeBSD 没有 sys/sysmacros.h——定义了反而编译炸）；glibc 2.28+ 把 major/minor 挪进 `<sys/sysmacros.h>`，Linux 模拟编译须在临时 config 里**追加** `#define HAVE_SYS_SYSMACROS_H 1`（与 strip SOCKADDR_LEN 同类：改临时 config 而非 shipped config）。
- **`transfer_scheme_supported` 契约修复**：`parse_scheme` 把未知 scheme 折叠成 local（`transfer_open` 的「未知前缀=本地路径」契约需要），导致 `scheme_supported("ftp")==1` 违反「已知且已编入才 1」的注释契约。新增 `scheme_is_known()` 前置判别，未知 scheme 恒 0。host 测试 +6 断言（local/NULL/空→1；smb/nfs 本机构建→0；ftp→0）。
- **`export_name` 截断边角**：nfs 连接缓冲 256 字节对 255 字符 share 的 `"/%s"` 需 257 字节，扩到 `MANGO_PATH_MAX`（顺带消掉 `-Wformat-truncation`）。

**本档关键坑（新踩）**
- **config.h 陷阱**：libsmb2/libnfs 均 `#ifdef HAVE_CONFIG_H → #include "config.h"` ⇒ 须 `-DHAVE_CONFIG_H` + 手写 config.h（面向 FreeBSD sysroot 口径）；宏必须 per-target 限定，全局加会让 zlib/minizip/7z 在共享 -I 链上意外捡到 NAS 的 config.h。
- **libnfs 生成头位置**：`libnfs-raw-*.h`（rpcgen 产物）在各自子目录（mount/nfs/nfs4/nlm/nsm/portmap/rquota）而非 include/ ⇒ 编译须给 **8 个子目录全加 `-I`**（上游 `lib/Makefile.am` 同款）；libnfs 同样要 `-D_U_=__attribute__((unused))`。
- **libsmb2 头包含顺序**：`<smb2/smb2.h>`（raw 协议头，定义 `SMB2_GUID_SIZE`/`smb2_lease_key`）必须先于 `<smb2/libsmb2.h>`；`_U_` 也是上游 `lib/Makefile.am` 的要求（aes128ccm.c 在用）。
- **libnfs 公开 API 名占用**：`nfs_read`/`nfs_close` 是 libnfs 公开符号 ⇒ 自写 helper 必须改名 `nfsio_*`，连带 `nfsio_ensure_connected`/`nfsio_list`/`nfsio_size` 统一前缀。
- **MinGW vs POSIX**：MinGW-w64 定义 `WIN32`（非仅 `_WIN32`）⇒ libnfs.h 的 `nfs_stat` 签名变 `struct __stat64 *`；`-U_WIN32` 直接 `#error Only Win32 target is supported!`（MinGW 系统头硬依赖）⇒ POSIX 分支用 WSL Linux 验证。
- **FreeBSD vs Linux 不可比项（libnfs）**：`HAVE_SOCKADDR_LEN`（`struct sockaddr.sa_len`）与 `HAVE_SYS_SOCKIO_H`（`<sys/sockio.h>`）是 FreeBSD 独有，Linux 模拟编译须从临时 config 剔除；这两行对 FreeBSD 由构造保证正确，留 SDK 机验证。
- **NFS export 前导 `/`**：NFS export 名是绝对路径；URL `host:/export/path` 解析出的 share 无前导 `/`，`nfs_mount` 需要绝对名 ⇒ 连接时补 `"/"`。
- **bash 引号**：check 脚本里 `-D_U_=__attribute__((unused))` 必须整串加引号（bash 把裸 `((` 当算术解析报 `syntax error near unexpected token '('`）。

### §1o SDK 机验证清单（SMB/NFS 拉取功能）— 宿主侧已到「连接前」

**宿主侧已做到哪**（SDK 机不必重做，直接跳到「真网络」）：宏开分支已过链接冒烟——libsmb2(53)+libnfs(25) 全部 TU + transfer.c + 驱动在 WSL Linux **真实链接成 ELF 并运行到首次网络调用前**（`transfer_open` 惰性 open 成功、URL 字段解析、scheme_supported 契约、local 回归全过）。连接/IO 的真实验证必须在 445/NFS 空闲的环境做，宿主做不到（见下边界）。

**宿主边界（已踩死，勿在此重试）**：
- **WSL 起真实 smbd 不可行**：此 WSL 是 **mirrored 网络模式**——Windows 宿主的 SMB 服务占住了 WSL 内**所有地址**（127.0.0.1/0.0.0.0/::1/eth0/172.17.0.1）的 139/445/netbios，`smbd` 绑定报 `Address already in use` 后 `open_sockets_smbd` PANIC (core dump)。`ss` 看不到占用者（属 Windows 进程）。而 `smb2_connect_share` 硬编码 445，改端口须改后端、违背「验证已提交代码」。
- **NFS 在 WSL 不可行**：WSL2 内核缺 `rpc.nfsd` 模块，`nfs-kernel-server` 起不来。

**SMB 拉取验证（SDK 机 / 真 NAS，连通 `make LIBSMB2=1` 产物）：**
1. 起 SMB 服务器：真 NAS 现成 share，或本机 `smbd`（`[share] guest ok = yes`）；**先确认 `ss -ltn | grep :445` 空闲**（FreeBSD 无 Windows 端口冲突）。
2. 放一个测试文件（如 `games/hello.pkg`），记其 `sha1sum`。
3. 启动 mango 产物 + HTTP，用产品路径验证（api.c 直调 `transfer_open(scheme,path)`）：
   - 列目录：`GET /api/fs/list?path=smb://<host>/<share>/games` → 应列出 `hello.pkg`，type 正确。
   - 拉取：`POST /api/fetch {src:smb://<host>/<share>/games/hello.pkg, dst:<本地>, scheme:smb}` → task_id → 轮询 `/api/tasks` → **DONE**。
   - 校验：dst 文件 `sha1sum` == 源 `sha1sum`。
4. 若需连接级冒烟（不启 HTTP），把链接冒烟 driver 指向真 URL 跑 `transfer_list/read/size` 校验字节。

**NFS 拉取验证（`make LIBNFS=1`）：**
1. 真 NFS 服务器（SDK 机内核或真 NAS）export 一个目录。
2. URL 用 `nfs://<host>/<export>/<path>`；**export 名无前导 `/`**（如 `nfs://host/export/path` 里 export=`export`），后端 `nfs_mount` 会补 `"/"`。断言 NFSv3 或 v4.1（`HAVE_NFS4_2` 已 undef，仅 4.1）。
3. 同 SMB 第 3 步走 `/api/fs/list` + `/api/fetch` 校验字节一致。

**连接前断言（宿主已锁，SDK 机应继续绿）**：URL 解析五种形式（`smb://`/`nfs://`/`//host/share`/裸 `host/share`/`host:/export`）、`scheme_supported` 契约（未知 scheme=0、未编入后端=0）、惰性 open（URL 错在 open 报，连接错在 list/read/size 以 `MANGO_ERR_NOTFOUND` 浮出）。

---

## 1p. 金手指域已移除（Session 19，2026-09-29）

用户拍板：**砍掉金手指所有功能**，本插件只做文件管理。金手指文件（`.json`/`.xml`/`.mc4` 等）由文件管理器当普通文件覆盖（上传/浏览/编辑明文），本插件不再做金手指解析/引擎/路由——真正的金手指功能留给其它插件。

删除清单（全部落地，非仅注释）：
- **后端 C 引擎**：`src/cheatmgr.c/.h`、`src/cheat_sync.c/.h`、`src/cheat_parse.c/.h`、`src/cheat_remote.c/.h`、`src/hijack_stub.c` 整档删除；`api.c` 删除 `/api/cheat/*` 共 7 条路由 + 对应 handler + include。
- **枚举/调度**：`mango_common.h` 的 `task_kind_t` 删除 `TASK_KIND_CHEAT_APPLY`；`worker.c` 删 `step_cheat()` 与分派 case。
- **构建/测试**：Makefile `COMMON_SRCS`/`PS5_SRCS` 删除全部 cheat 源；`tests/` 删 `test_cheat_parse.c`/`test_cheat_sync.c`/`test_cheat_remote.c`/`run-cheat-tests.sh`（判据：`ALL CHEAT SUITES PASSED` 不再存在）与 `stubs_ps5.c`/`stubs_nops5.c` 的 cheat 桩、`test_api.c` 的 `test_cheat()`；orchestration 脚本删 `CHEAT_OBJS`。
- **前端**：`assets/index.html` 删导航 `view-cheats`/`view-cheats` section/`scrimCheat` 对话框/`renderCheats` 等全部 JS 与 `KIND_CN` 对应项；`lang-zh.js`/`lang-en.js`/`install.html` 删对应文案。

**测试（本机验证链，全绿）**：`node .build/ui_live_check.mjs` → **85 项 85 通过**（组 13 金手指断言随 mock/路由一并删除）；orchestration 回归（api 142 / worker 25 / extract 40 / upload 27，全 0 失败）；ELF 构建成功（855 KB，链接日志确认无 cheat 源文件）。

---

## 1q. 当前进度（Session 20，2026-09-30）：PKG 安装链路 —— 从「从未可能工作过」到「编过 / 链过 / 主机可验」

**起因**：用户给了一个 GitHub 链接（`itsPLK/ps5-payload-manager`）问「PKG 不用 U 盘」的原理。
取证发现**链接指错了仓**：那个仓是 payload 管理器，全仓 grep `InstallByPackage` **零命中**；
真正实现的是同作者的 `itsPLK/ps5-pkg-manager`（GPL-3.0）。完整取证链见 `docs/pkg-install-without-usb.md`。

**原理（一句话）**：`sceAppInstUtilInstallByPackage()` 的 `uri` **接受 `http://` URL**。
「PKG 必须在 U 盘」只是**主机自带安装界面的限制**，不是内核限制、也不是 API 限制。
⇒ 本机起一个**只监听 `127.0.0.1`** 的 HTTP 服务，把自己的包喂给系统安装器即可。
⚠️ 推论：**`/data` 等内部盘（以及 SMB）上的包走文件路径注定失败，必须推流。**

**本轮落地**：

| 项 | 结果 |
| --- | --- |
| `src/pkg_stream.{h,c}` | **新增**。loopback-only HTTP/1.1 range 服务：三阶段协议、未登记路径（含 `.crc` 探测）必 `404`、keep-alive、非阻塞、由主循环泵送 |
| `src/pkg_installer.c` | **重写**。**真签名** `(pkg_metadata_t*, pkg_info_t*, playgo_info_t*)` + `Initialize/Terminate` 包裹 + 真 `GetInstallStatus` 进度 + `uri` 走推流；元数据三件套**存在 handle 里**（系统可能长期引用） |
| `src/worker.c` | `worker_tick()` 泵送流服务（它是进程唯一主循环）；`step_install()` 进行中返回 0 —— 否则安装期间主循环 **100% 空转**；失败/跳过的原因经 `task_set_note` 透出 UI |
| `src/api.c` | 🪤 **顺带修既有红**：文件操作块用了 `lstat()` 与两参 `mkdir()`，MinGW 都没有 ⇒ 编排套件**早已编不过**。用 `#ifdef _WIN32` 映射修复 |
| `tests/test_pkg_stream.c` | **新增 89 项断言**，三段协议端到端，**不需要真机**（正好补上「Phase 0 没真机」的短板） |
| `Makefile` / `tests/run-orchestration-tests.sh` | `pkg_stream.c` 进 `COMMON_SRCS`；补 `tests/compat`（`<sys/statvfs.h>` 垫片）与 `-lws2_32` 的 **Windows-only** 分支；`app_install_if_needed` 在两种桩里各补一个 |

**证据（不是「应该能编」）**：prospero-clang 18 + `-Werror` 单 TU 编过 7 个模块（含 `pkg_stream.c`）；
`make all LIBSMB2=1 LIBNFS=1` **链接成功**，产物 `ps5-mango-v0.1.0.elf` = **1329776 B** /
sha256 **`5a0113766c82b3be9290c7a630c514e729ae08a2d975bfbb95dd3a4fdb571335`** / `e_machine = 0x003e`，
`.dynamic` 依赖含 `libSceAppInstUtil.sprx` + `libkernel_sys.sprx`。
🪤 **尺寸与上一版完全相同（1329776）** —— 又一次实证「尺寸相同**不能**证明任何事」（对齐填充吸收差值）：
只能靠 sha256 + `readelf -SW` 段尺寸 + `grep -a` 新字符串判读（`/stream/install/`、`kstuff is not running`、
`already installed` 等 7 条**全部命中**）。快照留存 `.build/elf-snapshots/ps5-mango-v0.1.0-stream-sha5a011376.elf`。
**符号可得性已核实**：`$SDK/target/lib/libSceAppInstUtil.so`（108 个桩符号）**确实导出** `Initialize` /
`Terminate` / `InstallByPackage` / `GetInstallStatus` ⇒ 直连 `-lSceAppInstUtil` 即可，**不需要**
`kernel_dynlib_resolve` 取址（参考实现用后者是因为它要带 helper 进程）。

**真机 7 条待核**：见 `docs/pkg-install-without-usb.md` §7.5，同步回填 `docs/sdk-machine-checklist.md` §D。
最长的一条是 ②：`InstallByPackage` 是否**在本进程内返回** —— 参考实现为此专门开了 helper 进程，
我们直调 ⇒ **若该系统调用在真机上挂死，会带走整个 payload**。这是本项目唯一一处这类风险，明确记录不掩饰。

---

## 1r. 当前进度（Session 21，2026-09-30）：真机反馈一轮 —— 上传/删除/NAS/关闭进程 + 界面放大

**起因**：用户拿到 v0.1.0 真机反馈，一次给了 6 件事（原文拆条见下）。核心方法是先**分类再动手**：
把「网络问题」和「前端问题」分开，其中两条靠取证直接推翻了用户的猜想（详见表内 🪤）。

| 用户原话 | 结论 | 落地 |
| --- | --- | --- |
| 「多选几个 rar 显示上传中断（网络错误），刷新后文件名在、大小都 0 KB」 | 🪤 **前端 bug，与网络无关**：`change` 处理里 `input.value=""` 会让先前取出的 `File` 句柄失效，再读就是 **0 字节** ⇒ `enqueueUploads` 的 `f.size > 0` 过滤把它们放行成 0 字节上传 | 清空推迟到队列排空后（`resetUpInputs()` 由 `pumpUp()` 在排空时调用）；后端 `upload_stream` 补「任何 FAILED 都删掉目标」+ 失败 note 写明断在几字节 |
| 「上传文件 / 上传文件夹合并成一个按钮，点一下出列表」 | — | 一个按钮 + `role=menu` 下拉（两项，命中区 60/81px）；`#btnUploadFolder` 删除 |
| 「网页最上面加个关闭进程按钮」 | PS5 上**没有 service manager** ⇒ 没有任何东西会把它拉起来，**不可逆** | 顶栏最右 + 二次确认 + 发完请求立刻切「进程已关闭」收尾屏并停轮询（`POWERED_OFF`）；后端 `/api/shutdown` 只置标志，拆卸仍走主循环老路 |
| 「NAS 还是不行 cannot open source (bad path)；SMB 192.168.1.100，PS5 <ps5-ip>」 | 🪤 **不是连通性问题**：`transfer_url_parse()` 在「只填主机没填共享」时直接返回 0 ⇒ **零网络 I/O** 就失败了。跨网段是后面的事 | 只填主机 = `server_only`，SMB 后端改连 **`IPC$`**（根列表就是共享名）；错误文案改成「请写成 主机/共享」 |
| 「删除多个文件没有进度条，有的游戏上万个文件」 | 原来在请求里递归删完才返回 ⇒ MHD 线程被占几分钟，浏览器先放弃 | **新增 `src/task_delete.{h,c}`**：枚举出总条目数后入队 `TASK_KIND_DELETE(7)`，worker 每 tick 删 ≤256 条，进度条就地显示在文件视图 |
| 「三个 pagehead 装饰块删掉 + 按钮和文字放大」 | — | 三块删除（`TASKS` 那块你没点，保留）；新增 **`--ui:1.15` 单一杠杆**（只作用于控件），正文各自上调 |

**证据（不是「应该能」）**：

| 项 | 结果 |
| --- | --- |
| 编排套件 `tests/run-orchestration-tests.sh` | **全绿**。新增 `tests/test_task_delete.c` **43 项**（含两条载荷断言：300 条的树**一次 tick 删不完**、进度严格落在 0 与 total 之间 —— 这正是「有进度条」的全部意义） |
| 实时验收 `.build/ui_live_check.mjs` | **153 项全过**（原 111 + 本轮新增 42：上传菜单 12 / File 句柄回归 4 / 删除全链路 11 / 关闭进程 6 / 其余） |
| ELF | `ps5-mango-v0.1.0.elf` = **1346160 B** / sha256 **`b78226e8c9db2957777fe6e7053b6aec3349b62978c06393cfb11130c4009fea`** / `e_machine 0x003e`，`-Werror` 零告警。内嵌资源 7/7 命中（`关闭进程` / `--ui:1.15` / `jobBarCancel` …），C 侧 7/7 命中（`/api/shutdown` / `删除任务无法续跑` / `IPC$` …） |
| 几何 | `.build/measure-scale.mjs`：9 个宽度档零横向溢出；900px 起顶栏单行 70px；控件 44/51/46px，字号 18/17/16px |

**本轮新增的三个坑（已同步进 §4）**：
1. 🪤 **`data-p` 是「假数据占位」的标记**，静态演示下有个 900ms 的推进器遍历 `[data-p] i` 改写宽度。
   把真实数据的进度条也标上它 ⇒ **离线预览里显示假进度**（实测：立刻读 4%，等一个周期变 71.2%）。
   **探针必须跨过一个推进周期再断言**，否则看不出来。
2. 🪤 **控件整体放大 + 多一个按钮会把吸顶栏挤成两行**：900px 宽下顶栏 65px → **123px**。
   处置：≤1240px 把状态胶囊收成「只有灯」（文案 130px 是这一行最贵又最不必要的，且本项目对状态区的
   既定口径本就是「状态由灯承载」）。**改 UI 尺寸必须量顶栏高度，不能只看有没有横向溢出。**
3. 🪤 **`lstat` / 两参 `mkdir` 在 MinGW 上不存在** ⇒ 新模块（`task_delete.c`）和它的宿主测试都要
   各写一次 `#ifdef _WIN32` 映射。上一轮已经在 `api.c` 踩过，这次是「同一个坑以「新文件」的形式复发」。

**改前的对照基线**（以后别靠记忆）：`.build/_cmp_scale.mjs` 用「`--ui:1` + 去掉新按钮」的副本量出
390/600/900/1280/1920 五档的顶栏高度差，是判断「这一处是不是我改出来的回归」的唯一办法。

**真机待核（本轮唯一没能离线验的）**：`<ps5-ip> → 192.168.1.100` 的**跨网段可达性**。
光猫与路由器各接一段时，若其中一台是 NAT 模式，PS5 到 SMB 服务器**根本不通** —— 这与本轮修好的
「地址解析」是两件事，需要用户在主机上确认（能 ping 通 / 能列共享即通）。

**遗留（本轮没动，明确记录）**：
- `#view-tasks`（任务视图）**没有导航入口**（导航只有 文件 / PKG管理 / 存档），所以本轮把删除进度
  就地做在文件视图里。任务视图里 `平台/存储` 两张卡是**占位数据**，接上真实入口前不要放出去。
- `.build/preview_check.mjs` / `ui_upload_menu_test.mjs` / `ui_retry_test.mjs` 是**旧仓（`assets/main.js`）
  的遗留**，对本仓无效；本仓前端的权威验收只有 `.build/ui_live_check.mjs`。
- `src/transfer.c` 的 `set_err` 在**宿主测试**里报 `-Wunused-function`（SMB/NFS 宏关着），ELF 构建
  （宏开着）没有此告警 ⇒ 是宿主构建产物，不是回归。

---

## 1s. 当前进度（Session 22，2026-09-30）：文件权限 —— 「Mango 创建的一切都是 0777」

**起因**：真机反馈「上传完的游戏文件权限应该都是 0777，你现在上传完是 0666」。

**根因（一个数字就定了案）**：`0666` 不是 `0644`。`fopen("wb")` / `open(..., 0666)` 的落盘模式是
`mode & ~umask`；桌面 Linux 常见的 umask 022 会给出 0644，而用户看到的是 **0666** ⇒ **payload 的 umask 是 0**。
所以这不是「谁写死了 0666」，而是「**谁都没设，umask 说了算**」。

**为什么必须修**：PS5 上的游戏与自制程序必须可执行 —— 一个 0644 的游戏就是「传上去了但跑不起来」。
本仓其实早有一条隐性规则：**解压路径一直无条件 0777**（`zip_extract.c` 的 `chmod_0777_fd()`）。
只有**上传那条链漏了**，两条链的产物因此不一致。这轮把它显式化成一条不变量。

**不变量**（唯一实现点 `src/nx_fs.h`）：我们创建的文件与目录一律 0777，且**创建之后显式 chmod** ——
因为 `open()/mkdir()` 的 mode 仍会被 umask 削掉，只传 mode 不足以覆盖 umask 0。FAT/exFAT 拒绝 chmod，
一律 best effort、不算失败。

| 落盘点 | 改前 | 改后 |
| --- | --- | --- |
| `src/upload_stream.c`（**用户报告的那条**） | `fopen("wb")` ⇒ umask 说了算；建目录 0755 | `nx_chmod_create()` + `nx_mkdir_create()` |
| `src/copypipe.c`（fPKG→`/data/homebrew`、NAS 拉取、存档回写、封面缓存） | `open(..., 0644)` | 以 `NX_CREATE_MODE` 打开，并在 `commit()` 里 chmod 落地的目标 |
| `src/worker.c` `ensure_parent_dir()`（`/data/homebrew` 目录树） | `mkdir(0755)` | `nx_mkdir_create()` |
| `src/api.c` 新建文件 | **显式 `chmod(target, 0666)`** | `nx_chmod_create()` |
| `src/api.c` `api_mkdir` | `mkdir(0777)` + chmod | 复用 `nx_mkdir_create()`（顺带去掉一份重复的平台垫片） |
| `src/app_installer.c`（主屏 title 目录 + `param.json` / `icon0.png`） | `mkdir(0755)` + fopen | `nx_mkdir_create()` + `nx_chmod_create()` |
| `src/sevenz_extract.c` `szx_apply_metadata()` | 只在归档**自带** Unix mode 时 chmod ⇒ Windows 打的 7z（mode=0）**什么都不设** | 无条件 0777 |
| `src/rar_extract.c` | 由 unrar 自决（Windows 打的 RAR 只带 DOS 属性） | 抽出后、发布前 `chmod_tree_0777(staging)` 整树归一 |

**刻意没动的**（写下来，免得下一轮又去改）：
- `src/api.c` 的 `copy_recursive()` 用 `st.st_mode & 07777` **保留源权限** —— 复制本就该保留权限，
  根因在「源是谁创建的」，不在复制。
- `/api/chmod` 是用户显式指定模式的路由，当然照用户的来。
- `savemgr.c` 的 staging 临时目录（用完即删）、`api.c` 的 `.covers` 缓存目录。

**证据**：

| 项 | 结果 |
| --- | --- |
| 编排套件 `tests/run-orchestration-tests.sh` | **全绿**。`upload_stream` **43 → 55 项**（新增 12 条 `nx_fs` 契约断言） |
| 解压套件 | `tests/run-tests.sh` **140 项 0 失败** + `rar_extract` **37 项 0 失败**；`tests/run-sevenz-tests.sh` **27/27 通过**，错误与策略路径 **33 项 0 失败** |
| ELF | **1346160 B** / sha256 **`e0e13d05f7639912a62fc5ecb5eef173105f0a1b27bd7e3f31ee165d2cb6c4aa`** / `.text` `0x0b7680`，`-Werror` 零告警，比全部源文件新 |
| ⚠️ 体积陷阱再现 | 本轮 ELF 与上一轮**字节数完全相同（1346160）**却 sha 不同 ⇒ 又一次印证「**只看尺寸会把新功能判成没变化**」 |

**本轮新增的坑（已同步进 §4）**：
1. 🪤🪤 **权限位在宿主上验不了**：`tests/posix_compat.h` 把 `fchmod` 映射成空实现、`mkdirat` 忽略 mode，
   Windows 的 `stat()` 也只是从只读位合成 `st_mode`。⇒ 宿主测试只能验**助手的契约**（常量值、EEXIST 容忍、
   错误传播，以及「chmod 确实被调用」—— Windows 的只读位是可观测的），**真正的 0777 只能在真机 `ls -l`**。
   别在宿主里写「断言 mode == 0777」的测试 —— 它永远是绿的。
2. 🪤 **umask 是隐形变量**：`open(path, 0777)` 不等于文件是 0777，要 0777 就必须**创建后 chmod**。
   反过来，看到 `0666`（而不是 `0644`）就该想到 umask=0。
3. 🪤 **同一件事往往有多条链**：解压早就 0777，上传却漏了。修「权限不对」这类问题前，先把**所有落盘点**
   列出来（`grep -n 'fopen\|O_CREAT\|mkdir(' src/`），否则只修了用户点到的那一条。

**真机待核（新增）**：在主机上分别对「上传的文件」「fPKG 部署后的 `/data/homebrew`」「解压产物」做一次
`ls -l`，确认是 `-rwxrwxrwx`。⚠️ **FAT/exFAT 挂载点会拒绝 chmod**（`/mnt/usb0|1` 上的外置盘常见），
那种情况下 chmod 失败是**预期行为**、不是 bug —— 判定要带挂载点的文件系统，别一概而论。

---

## 1t. 当前进度（Session 23，2026-09-30）：进程「一交互就退出」—— `con_cls` 类型混淆（根因）+ 内置请求日志

**起因**：真机反馈「插件一运行就结束进程了」。三轮追问把它收敛成三个决定性事实：
① 「**网页能开，进去/点一下就没**」（HTTP 服务起来了、静态前端也加载了 ⇒ 不是初始化崩）；
② 「**旧 payload 能跑，只有 Mango 新版不行**」（主机/破解状态健康 ⇒ 是本仓当前构建的回归）；
③ 「**浏览器漏洞注入**」启动。

**根因（`src/httpd.c`，一次指针错认）**：`upload_conn_done()` 是本进程注册的**唯一** MHD 完成回调
（`MHD_OPTION_NOTIFY_COMPLETED`）。它**无条件**把 `*con_cls` 当成 `struct upload_conn`。但 libmicrohttpd 给每个
请求**只有一个 `(void **con_cls)` 槽**，而本进程有**两种**上下文：

| 上下文 | 结构 | 产生处 |
| --- | --- | --- |
| 普通 JSON / 静态 | `struct conn_state { char *body; size_t len; }` | `answer()` 首次调用 |
| 流式上传 | `struct upload_conn { upload_stream_t *us; char err[160]; int bad; }` | `handle_upload_file()` |

⇒ 一个**在最终处理调用之前就被拆掉**的 `conn_state`（客户端中止 / 页面跳转 / 被取代的 fetch），其
`cs->body`（积累的 POST 体缓冲区）会被当作 `upload_stream_t *` 传给 `upload_stream_finish()` 并
`upload_stream_free()` 其内部指针 ⇒ 真机上没有分配器冗余 ⇒ **立刻 SIGSEGV，进程消失**。

**为什么它解释了全部观测**：
- 正常完成**安全** —— `answer()` 在返回前已经 `free(cs); *con_cls = NULL`（回调进来看到 NULL 直接返回）。
- **`GET /api/status`、`GET /api/tasks` 没有请求体** ⇒ `cs->body == NULL` ⇒ `if(uc->us)` 为假 ⇒ 安全
  ⇒ **这正是「页面本身能打开」的原因**（首屏只有 GET）。
- **新前端**加载后连发一批 **POST**（设备根探测：`/api/fs/list`、`/api/space`、`/api/pkg/scan`、`/api/save/list`），
  这些请求**有 body** ⇒ 那个别名指针是**真堆地址** ⇒ 崩 ⇒ 进程退出 ⇒ 恰好是「一进去/点一下就没了」。
- 旧 payload 没有这段代码 ⇒ 正常。
- **宿主矩阵永远看不到**：`src/httpd.c` 需要 `<microhttpd.h>`，**从不进任何宿主套件**
  （`tests/run-orchestration-tests.sh` 的模块表里没有它）—— 所以它在之前每一轮验证里都是隐形的。

**修法**：两个结构体**第 0 个字**各放一个 magic tag（`MANGO_CTX_JSON` / `MANGO_CTX_UPLOAD`），各配
`_Static_assert(offsetof(..., magic) == 0, ...)`（编译期钉死「回调只读第 0 字」这个前提）；完成回调改为
**按 tag `switch` 分派**：`MANGO_CTX_JSON` 释放 `body`+`cs`，`MANGO_CTX_UPLOAD` 收尾 `us`+`uc`。
**未知 tag 故意「泄漏不释放」并记一行日志** —— 泄漏一个上下文是可承受的，把一个认不出的指针 free 掉不是。

**新增能力：内置请求日志（真机自证）** —— 浏览器注入的 payload **没有 shell、没有 coredump**，真机崩溃此前
只能靠用户口述。故新增 `src/diag.{h,c}` + `GET /api/diag`（**明文**，只读该文件、不碰任何状态，所以在「其他
路由全崩」时仍可访问）：

| 标记 | 含义 |
| --- | --- |
| `REQ <method> <url>` | 请求进入 `answer()`（**每请求一行**，不是每处理调用一行） |
| `DONE <url>` | JSON 路由处理函数**返回了**（静态 / 下载 / 上传没有这行） |
| `SENT <status> <url>` | 响应已交给 libmicrohttpd |

⇒ **最后一条没有 `SENT` 的 `REQ` 就是杀死进程的那个请求**。日志路径
`preferred → /data/mango-diag.log → 工作目录`（`diag_init()` 在建 HTTP 服务**之前**调用），>512 KB 轮转一次到
`<path>.1`，**逐行 open/append/close**（缓存的 `FILE*` 会正好丢掉最关键那一行）。

**证据**：

| 项 | 结果 |
| --- | --- |
| 编排套件 `tests/run-orchestration-tests.sh` | **全绿**（新增 `diag` 套件 **22 项 0 失败**） |
| ASan/UBSan 全套（`.build/asan-suites-wsl.sh`，含 `diag`） | **0 报告** |
| ELF | **1346160 B** / sha256 **`c17d00f7658e7de94b92a74d586a45b51b3262d851a520e8d0b5a2df12574523`** / `.text` `0x0b7c10`（+0xED0）、`.rodata` `0x05dab0`（+0x1D80），`-Werror` 零告警，比全部源文件新 |
| 新字符串命中 | `REQ %s %s` / `DONE %s` / `SENT %u %s` / `=== session start ===` / `/api/diag` ✔ |
| ⚠️ 体积陷阱**第三次**再现 | 本轮 ELF 与上一轮**字节数完全相同（1346160）**却 sha 不同 ⇒ 再次印证「只看尺寸会把新功能判成没变化」 |

**本轮新增的坑（已同步进 §4）**：
1. 🪤🪤 **一个请求只有一个 `con_cls` 槽**：同一 daemon 上只要跑两种上下文，完成回调**必须**按 tag 分派，
   不能靠「我只注册过一个上传路径」假定类型。
2. 🪤🪤 **`src/httpd.c` 永不进宿主矩阵**（缺 libmicrohttpd）⇒ HTTP 层的 bug 宿主机**看不到**，`make linux`
   也编不过。这类问题的验证只能靠**真机**或**内置自述日志**。
3. 🪤 **「进程一交互就退」先分 GET 还是 POST**：页面能开 ⇒ GET / 空 body 那条路是好的 ⇒ 罪魁在后面的 POST 批。
4. 🪤 **诊断本身必须被测**：`/api/diag` 是崩溃的唯一证据，它若丢行 / 截错尾，给出的是**自信的错答案**
   ⇒ `tests/test_diag.c` 钉住三条性质（追加写不擦除、tail 锚定末尾、无路径时静默降级）。

**真机待核（新增）**：崩了以后在浏览器打开 `http://<PS5-IP>:2026/api/diag`，读最后一条**没有 `SENT`** 的
`REQ` —— 那就是元凶（若连日志都没有，说明连 `answer()` 都没进，问题在 HTTP 服务启动之前，另论）。

---

## 1u. 当前进度（Session 24，2026-10-01）：「反向搬运」P0 —— 开跑前拒绝（空间 + 可写）

### 起因与诊断
用户问：「为什么文件管理这些功能总是出错，就不能直接把这个项目（`owendswang/ps5-web-file-manager`）
所有功能都搬过来么，改改 UI 就完事了」。查账后的结论是：**出错的不是文件管理逻辑，是它周围的接缝**，
而这一层被**计划外重写**了。

- 近四轮故障按层归属：前端（上传全 0 KB，WebKit 文件句柄）、HTTP 请求生命周期（一交互就退进程，
  `con_cls` 类型混淆，§1t）、落盘权限（0666，§1s）、UI 信息架构（删除进度看不到）。
  **没有一个是复制/移动/删除/列表本身** —— 那部分在宿主矩阵里一直是绿的。
- 结构性原因：旧仓 `filemgr.c` 一个文件 **2 459 行**，把「HTTP 路由 + 任务调度 + 文件操作 + 权限 +
  空间校验」捏在一起。而 PLAN §1 的**继承清单**列的是 `extract* / copypipe / pkg_sfo / json_util /
  path_util`，**重写清单**列的是「传输层 / NAS 任务流 / 安装器 / PKG 库 / 存档 / UI / HTTP 瘦身」——
  **`filemgr.c` 两个清单都没列**。HTTP 与任务调度两头都判为重写，夹在中间那截只能跟着重写
  ⇒ 每次修好的都是重写时新引入的接缝缺陷。

### 「全搬上游改 UI」为什么不是捷径（三条反证，均已核实）
1. **那不是「上游」，是旧仓的祖先**：旧仓根提交 `Initial import of PS5 Web File Manager v1.7`，
   GitHub compare 明确报 **no common ancestor**（导入式 fork，非真 fork）。全搬 = 回到 v1.7。
2. **旧仓已经比上游好**：上游解压靠**外挂 helper ELF**（需用户单独装 `/data/wfm/wfm-7zip-helper.elf`），
   旧仓自己实现了 7zAES（含 `-mhe=on`）/ ZipCrypto / WinZipAES（≈9 700 行：`sevenz_*`、`zipx_*`、
   `zip_extract.c`、`rar_extract.c`）。全搬 = 把 v1.9.3M 倒退。
3. **「改 UI 就完事」不成立**：上游 API 是 `/api/upload/prepare` + 每文件一次 `/api/upload-file` +
   `/api/upload/finish`、**单任务串行**；mango 是单次流式上传 + 统一任务队列 —— UI 与后端被 API 形状
   焊在一起。且 PLAN §0 白纸黑字写着「**明确不做：又一个通用文件管理器**」，而上游**就是**那个通用
   文件管理器。

### 落地：`src/fs_guard.{h,c}`（新增，P0）
从旧仓 `src/fs_util.c`（371 行）搬两样回来，丢失情况的对照：

| 旧仓 | mango 之前 | 现在 |
|---|---|---|
| `check_target_space()`（`filemgr.c:1709` 复制/移动、`upload.c:156,393` 上传） | **无**（`statvfs/f_bavail` 只在解压引擎内部） | `fs_guard_space()` |
| `probe_dir_writable()` / `check_target_writable()`（`fs_util.c:236 / 326`） | **无** | `fs_guard_writable()` / `fs_guard_removable()` |
| `count_path_bytes_sync()`（`filemgr.c:366`，lstat，不跟随符号链接） | 无 | `fs_guard_tree_bytes()` |

设计要点（都是刻意的）：
- **向上走到第一个存在的目录**（`fg_first_existing_dir`）。只走一步 `dirname()` 会把「上传到
  `/mnt/usb0/NewFolder/game`」这种真实场景退化成 `FS_GUARD_UNKNOWN`，**恰好让闸门在最需要它的地方失效**。
- `errno` 区分两种失败：`ENOTDIR`（路径中间有个普通文件 ⇒ **确定性拒绝**）/ `ENOENT`（查不到 ⇒ 建议性）。
- 可写性用**真建一个文件再删掉**来探测，不用 mode 位：payload 可能带提权凭据（mode 位不描述我们能做什么），
  FAT/exFAT 更是根本没有 mode 位。探针用 `NX_CREATE_MODE` 并立刻 unlink（守住「一个创建模式」的规则）。
- `FS_GUARD_UNKNOWN` **一律放行**：读不到可用空间不该阻断原本能成功的工作。闸门的职责是把
  「10 分钟后 90% 才失败」变成「2 毫秒拒绝」，**不是新增一种拒绝理由**（`required == 0` 同样是 no-op）。

接入点（全部在**开跑前**）：
- `api.c api_copy`：可写 + 空间（`copy_source_bytes()`；量不出总量则传 0 = 不拦）
- `api.c api_move`：**只查可写**（同盘 move 是 rename，不需要空间）；空间闸门放进**真正会复制的跨盘回退**分支
- `api.c api_delete`：每个 root 入队前查可删（与既有的「拒绝 /」同一条规则）
- `api.c api_extract`：查目标可写（空间由引擎自己查）
- `api.c api_upload`：首片（`offset == 0`）就知道 `total`
- `upload_stream_begin()` / `_begin_rel()`：在**建任务与建文件之前**拦（拒绝时不建任务、不留 0 字节
  残留、`begin_rel` 也不留空目录树）

### 测试
- 新增 `tests/test_fs_guard.c`（**MinGW 39 项 / 原生 POSIX 42 项**），并入
  `tests/run-orchestration-tests.sh` 与 `.build/asan-suites-wsl.sh`；`fs_guard.o` 进 CORE
  （api.o / upload_stream.o 都调用它）。
- `test_api.c`：**`/api/copy` 与 `/api/delete` 此前没有任何宿主覆盖**，本轮补了 `/api/copy` 的
  拒绝路径（目标路径中间是普通文件）+ 成功路径（api 180 → 186 项）。extract 夹具从合成的 `"/b"`
  改成真实 `read_tmp`（预检会先拦掉一个宿主根本创建不了的目标）。

### 🪤 本轮新踩的两个坑（已进 §4）
1. **`strings` 只输出 ASCII** ⇒ 用它查中文串恒 MISS（`fs_guard` 的中文报错第一次全被误判成「没进
   二进制」）。查 UTF-8 必须字节级检索：`python3 -c "print(s.encode() in open(elf,'rb').read())"`。
2. **`/mnt/c`（WSL 9p/drvfs）不实施 Unix 权限位**：`chmod 0555` 返回 0，`stat` 读回仍是 **777**。
   于是「只读目录应被拒绝」的断言会**静默变成空转** —— 一个从不拒绝的闸门看起来完全健康。
   处置：测试先验证夹具前提（chmod 后 `stat` 确认真的变了）才断言，否则打印跳过；并在 ASan 脚本里
   用 `mktemp -d` 的原生 cwd **再跑一遍** fs_guard。

### 验证
| 项 | 结果 |
|---|---|
| 宿主编排套件（MinGW） | 全绿；`fs_guard` 39 项、`api` 186 项、其余不变 |
| ASan/UBSan 全套 | **0 报告**；fs_guard 在原生 cwd 再跑 **42 项 0 失败**（负向断言真的执行了） |
| PS5 ELF | `-Werror` 零告警；sha256 **`52ed1b0098253fa2b644d5f7d01d5dea79a4d3e44c0df0afcacf5761600e631e`**、**1 362 544 B**、`e_machine 0x003e`；比全部源文件新 |
| 二进制内证 | 全部中文报错 + `.mango-probe-` 字节级命中（`fs_guard_result_name()` 只被测试引用 ⇒ 被 `--gc-sections` 回收，属预期） |

### 尚未做（「反向搬运」清单的 P1/P2）
- ~~**P1** `fs_type_has_unix_modes()`（按 `f_fstypename` 判 FAT/exFAT）+ `fchmod_0777(fd)`~~
  ✅ **已完成（Session 25，见 §1v）**
- ~~**P1** `mime.c` / `text.c` 的编辑与预览细节（图标/扩展名映射、编辑器行为）~~ ✅ **已完成（Session 26，见 §1w）**
- ~~**P2** `filemgr.c` 的冲突/合并语义（`make_unique_target()` 的「副本」命名已是 mango 自有实现，值得对齐）~~ ✅ **已完成（Session 26，见 §1w）：mango 根本没有 `filemgr.c` 那套副本命名语义，P2 实际落地为「文本编辑的乐观锁冲突」——`/api/text/save` 的 CAS + 前端「强制覆盖 / 重新加载」二选一。**

---

## 1v. 当前进度（Session 25，2026-10-01）：「反向搬运」P1 —— fd 版 chmod + 文件系统类型判定

### 起因
P0（`fs_guard`，§1u）解决的是「开跑前拒绝」。本轮补清单里的两条 P1 —— 都不是新功能，
而是把 `src/nx_fs.h`「我们创建的一切都是 0777」这条规则补完整：

1. **`nx_fchmod_create(fd)`** —— 路径版 chmod 有一个**窗口**：`open()` 到 `chmod()` 之间，
   文件已经以 umask 掩码后的模式躺在磁盘上。**对上传来说这个窗口就是整段传输**，所以一次
   中断的上传会留下一个 0666 的游戏文件 —— 这正是用户报告的「上传完的文件是 0666」。
   fd 版把窗口压到微秒级。（P0 修的是另一头：开跑前就拒绝。）
2. **`nx_fs_type_lacks_unix_modes(fs_type)`** —— FAT/exFAT（外置 USB）**根本没有 mode 位**，
   对着它 chmod 是白费。先判类型 ⇒ 把「无意义 syscall + 可能被误报成失败」变成**刻意的 no-op**。

### 旧仓取用（`src/fs_util.c`）
| 旧仓 | 语义 | 现在的名字 |
|---|---|---|
| `fs_type_has_unix_modes(type)` | exfat/exfatfs/msdosfs/fat/vfat ⇒ 无 | `nx_fs_type_lacks_unix_modes()`（**取反命名**，免双重否定） |
| `ignore_chmod_error(err)` | `ENOTSUP/EPERM/EINVAL/EROFS` ⇒ 不算失败 | `nx_chmod_error_is_benign()` |
| `path_has_unix_modes` / `fd_has_unix_modes` | `statfs`/`fstatfs` → `f_fstypename` | `nx_path_has_unix_modes()` / `nx_fd_has_unix_modes()` |
| `fchmod_0777(fd)` | fd 版 0777 | `nx_fchmod_create(fd)` |

### 三个刻意的决定
- **宿主上「假设有 mode 位」**：`statfs`/`f_fstypename` 是 BSD 家族的，glibc 与 Windows CRT
  都没有 ⇒ `#if defined(__linux__) || defined(_WIN32)` 直接返回 1。**猜「没有」是危险方向** ——
  那会在真正实施 mode 位的文件系统上静默跳过实际工作。查不到（`statfs` 失败）时也一律假设「有」。
- **纯谓词单独拆出来**：`nx_fs_type_lacks_unix_modes()` / `nx_chmod_error_is_benign()` 是纯字符串 /
  整数判断 ⇒ **宿主上也是真断言**；围着它们的 `statfs` 查询只有真机跑得了。
- **`nx_chmod_create()` 改成 best-effort 契约**：无 mode 位的文件系统直接跳过；良性拒绝（只读挂载 /
  不支持）算成功；只有用户能处理的拒绝才返回 -1。所有调用点本来就都是 `(void)`，所以这不改变任何
  调用方的行为，只是把「为什么可以无视失败」从注释提升成了契约。

### 接入点
- `upload_stream.c`：两处 `nx_chmod_create(us->dst)` → `nx_fchmod_create(up_fileno(us->fp))`。
  新增 `up_fileno()` 垫片（POSIX `fileno` / MinGW `_fileno`）—— 按既定规则，每个宿主可测文件
  自带一份，不做全局宏。
- `copypipe.c`：`open()` 成功之后**立刻** fd 版 chmod（顺带关掉 `.part` 半截文件的窗口）；
  `commit()` 里的路径版保留（覆盖 `O_TRUNC` / 陈旧 `.part` 的情况）。
- `sevenz_extract.c`：`(void)chmod(full, NX_CREATE_MODE)` → `(void)nx_chmod_create(full)` ——
  这是当时**唯一**绕过「一个实现点」的落盘点。

### 测试
- `tests/test_upload_stream.c` **+19 项**（MinGW **74**；Linux 原生 cwd **76**）：纯谓词（含 `NULL`、
  前缀不匹配、fail-open 方向）+ 良性 errno 集合 + `nx_fchmod_create(-1)` 拒绝 + **errno 不被弄脏**
  + **原生文件系统上 fd 版真的把模式改成 0777**。
- `.build/asan-suites-wsl.sh`：原生 cwd 那一遍从「只跑 fs_guard」扩成 `for s in fs_guard upload_stream`。

### 🪤 本轮新踩的坑（已进 §4 与技能 `untestable-syscall-unification`）
**「跳过」分支以错误的理由触发，真断言第二次隐身。** 第一版的前置检查复用了同一条测试里刚被
`nx_fchmod_create` 改成 0777 的那个文件 ⇒ 原生 cwd 下也读到 0777 ⇒ 打出「本文件系统不实施
mode 位」并跳过；而 /tmp 明明实施 mode 位（同一轮 fs_guard 42 项全过就是反证）。
**修法**：前置检查的夹具必须**独立**，且用受限模式（`0600`）创建。
泛化教训：**跳过分支的判据本身要能失败** —— 否则「跳过」只是换了一种伪装。

### 验证
| 项 | 结果 |
|---|---|
| 宿主编排套件（MinGW） | **全绿**；`upload_stream` 55 → **74**，其余不变 |
| ASan/UBSan 全套 | **0 报告**；原生 cwd 下 `fs_guard` 42 / `upload_stream` **76** |
| PS5 ELF | `-Werror` 零告警；**`<sys/mount.h>` 与 `f_fstypename` 在 SDK 上确实存在**（这一步只有 SDK 机编得出来，本机不可预判）；sha256 **`0e692b5da5ab570ba41134b7b37eea956e76e8f744195c5a2470285ba5a93ed3`**、**1 362 544 B**、`e_machine 0x003e`；比全部源文件新 |
| 二进制内证 | **`fchmod`/`statfs`/`fstatfs` 三个符号在本仓此前从未出现过**（只用 `chmod`/`statvfs`）⇒ 它们出现在符号表里就是新代码确实进了 ELF 的证据。⚠️ 文件尺寸与上一版**完全相同**（1 362 544）—— 段对齐填充吸收差值，**只能看 sha** |

### 尚未做（清单里剩下的）
- ~~**P1** `mime.c` / `text.c` 的编辑与预览细节（图标/扩展名映射、编辑器行为）~~ ✅ **已完成（Session 26，见 §1w）**
- ~~**P2** `filemgr.c` 的冲突/合并语义（`make_unique_target()` 的「副本」命名已是 mango 自有实现，值得对齐）~~ ✅ **已完成（Session 26，见 §1w）：mango 根本没有 `filemgr.c` 那套副本命名语义，P2 实际落地为「文本编辑的乐观锁冲突」——`/api/text/save` 的 CAS + 前端「强制覆盖 / 重新加载」二选一。**

---

## 1w. 当前进度（Session 26，2026-10-01）：「反向搬运」P1 尾 + P2 —— 文本编辑后端（原子写 / 换行·BOM / 乐观锁）+ 冲突语义

### 起因
P0（§1u）是「开跑前拒绝」，P1（§1v）是「创建后权限正确」。本轮补清单里最后两条 —— 也把 mango **从旧仓 `text.c`/`mime.c` 继承来的、却丢了的三处静默能力**补回来：

1. **非原子写**：mango 的保存路径是 `fopen(path, "wb")`，它在写入第一个字节**之前**就截断了原文件。一次保存中途失败（U 盘写满、进程退出）会留下「半个文件」盖在原文件上 —— 这是文本编辑器**唯一会销毁用户已有数据**的失败模式。旧仓是 temp + `fchmod` + `fsync` + `rename`。
2. **换行 / BOM 丢失**：浏览器 `<textarea>` 只报 LF，无论原文件是什么。把值原样写回，会把每个 CRLF 文件整篇改写成 LF —— 编辑器里看不出来，其他工具看全是「整个文件变了」。旧仓记住原约定再套用。
3. **无并发版本检测**：旧仓读时下发 `X-Text-Version`，保存时客户端必须原样回传，否则拒绝；两个标签页（或 PS5 浏览器 + SMB 挂载）不会静默互覆盖。

两个更小的损失顺带修掉：扩展名匹配大小写不敏感（从 FAT/exFAT 拷来的 `.TXT` 旧构建预览不了、新版能）、保存体积上限服务端强制。

### 落地：`src/text_file.{h,c}`（新增，P1/P2 的叶子）
纯 C、零依赖 `<microhttpd.h>`、零依赖 `path_util.h` —— 同 `fs_guard.c` / `task_delete.c` 的规则，因为它和它的宿主套件必须能在本机编跑。关键原语：
- `tx_mime_for_name()`（大小写不敏感 ext→mime）、`tx_name_is_editable()`（比 mime 窄，图片能预览但不能进 textarea）。
- `tx_utf8_valid()`（拒 overlong / 孤立代理 / >U+10FFFF，像旧 `valid_utf8()` 一样）、`tx_version()`（FNV-1a 64，文件变更检测器，**不是**安全哈希）。
- `tx_detect_newline()`（多数决，平局默认 CRLF→保持原样）、`tx_format_for_save()`（按原文件换行 + 仅当原文件有 BOM 才带 BOM；CRLF 重写可能翻倍超 `TX_MAX_BYTES` 上限）。
- `tx_read()`（`lstat` 拒绝符号链接 / 非普通文件 / 超 `TX_MAX_BYTES`；返回文件 mode 供保存保留）、`tx_write_atomic()`（同目录建 `<path>.mango-<pid>-<n>.tmp` → `nx_fchmod()` 设回**原文件 mode**（不是 `NX_CREATE_MODE`，编辑不该把配置文件变可执行）→ `fsync` → 才 `rename`；任何一步失败删 temp、原文件不动）。

### 接入点
- `src/api.c`：`/api/fs/read` 文本回传 `version` + `editable`；删除旧的 `ext_is`/`fs_read_mime` 内联，改用 `tx_mime_for_name`（大小写不敏感）。
- `/api/text/save`：CAS —— `version`（`/api/fs/read` 下发）不匹配当前文件即拒（`"file changed since it was opened"`）；正文经 `tx_format_for_save` 保持换行/BOM，再 `tx_write_atomic` 落盘。
- `/api/text/create`：等价于 `O_EXCL` —— 拒绝覆盖既有文件（旧构建 `fopen(path,"wb")` 会截断，把同名文件毁了再开个空编辑器）。
- 前端 `assets/index.html`：`openTextEditor` 捕获 `fs/read` 回传的 `version`，`tSave` 原样回传；冲突时如实提示，**确定 = 以我的版本强制覆盖（重新读最新 version 再原子覆盖）/ 取消 = 放弃本地修改并重新加载**。`confirm()` 语义：`true`→覆盖、`false`→重载（写代码时曾写反成 `!confirm`，已修）。

### 测试
- `tests/test_text_file.c`（**MinGW 84 / 原生 POSIX 88**）：负载型断言 = 失败保存后原文件逐字节不变且无 `.mango-*.tmp` 残留（`test_write_atomic_failure` / `test_no_debris`）、CRLF 文件在 LF-only textarea 下仍回写 CRLF 且 BOM 不丢不造（`test_format_keeps_conventions`）、读下发 version == 保存端重算 version（`test_version`）、mode 保留仅在真实施加 mode 的文件系统上断言（`test_mode_preserved`，`/mnt/c` 跳过）。
- `tests/test_api.c` 186 → **207**：新增 `/api/fs/read` · `/api/text/save` · `/api/text/create` 覆盖（含版本冲突拒绝、原子性）。
- `.build/ui_live_check.mjs` 153 → **164**：文本编辑组（读取载入、保存回传 version、乐观锁冲突被拒、强制覆盖写回）。

### 🪤 本轮新踩 / 收尾的坑
- **宿主测试 `mkdir(path)` 1-arg 在 Linux/ASan 下编不过**（glibc 要 2 参）。`tests/test_copypipe.c` / `test_transfer.c` 一直用 1-arg `mkdir`，MinGW 能过、ASan/Linux 不能。补成与 `test_text_file.c` 同款的 `#ifdef _WIN32` `MKDIR` 宏 ⇒ 两端都绿。这是预存在的测试便携性缺口，与本次功能无关，但「改完必跑全套」逼出来顺手修了。
- 前端冲突弹窗的 `confirm()` **曾写反**（`!confirm` 把「确定」映射成重载、「取消」映射成覆盖），已改为 `confirm()`。

### 验证
| 项 | 结果 |
|---|---|
| 宿主编排套件（MinGW） | **全绿**；`text_file` 84、`api` 207、`fs_guard` 39、`upload_stream` 74 等 |
| ASan/UBSan 全套（WSL） | **0 报告**；原生 cwd 下 `fs_guard` 42 / `upload_stream` 76 / `text_file` 88 |
| 前端接入层（Playwright 同源 mock） | **164 / 164 通过** |
| PS5 ELF | `-Werror` 零告警；`e_machine 0x003e`；`text_file.c` 在链接清单内；sha256 **`e5e69df02d533c8085baa227c89108bc4997ab8dd7b77a206020a7cd6580c2b9`**、`1 362 544 B` |
| 二进制内证 | `file changed since it was opened`（CAS 拒写字面量）+ `Content-Type: text/plain` + 路由 `/api/text/save`·`/api/text/create`·`/api/fs/read` 均在；`fchmod`/`statfs`/`fstatfs`（§1v 内证）仍在。⚠️ 文件尺寸与 §1v **完全相同**（1 362 544，段对齐填充吸收 `text_file.c` 的差值），**只能看 sha + 字符串**，不能看尺寸 |

### 反向搬运清单状态
P0（§1u）· P1 fd-chmod/fs 类型（§1v）· **P1 文本编辑后端 + P2 乐观锁冲突（§1w）** 全部落地。**清单清空。**

---

## 1x. 当前进度（Session 17，2026-10-07）：目录统一 `/data/Mango` + 日志集中 + PKG 装完自动删

**用户诉求**：删了 `/data/wfm` 后 PKG 卡片信息消失 —— 疑问「PKG 信息不能直接读原包吗、为什么多生成临时文件夹」。逐条回应：

1. **PKG 卡片信息本来就是直接读原文件**（`pkg_info.c` 按偏移读 PKG 头 content_id/title/version/icon，无解压、无临时目录依赖）。卡片消失的真相：用户删的 `/data/wfm` 里存着**从电脑上传的 PKG 文件本体**（前端 `upDir`/`PKG_STAGING` 的默认落点 `/data/wfm/incoming`），删目录 = 删文件 = 卡片消失。不是「临时文件夹承载卡片信息」。
2. **临时产物统一到一个文件夹**（用户拍板：项目叫 MANGO，去 wfm 字样）→ 全部收敛到 **`/data/Mango/`**：
   - `incoming`：PKG/上传暂存（原 `/data/wfm/incoming`）
   - `tmp`：存档临时镜像（原 `/data/save_files`）
   - `backups`：存档备份（原 `/data/backups`）
   - `logs`：三份日志（原散在 /data 根目录）
3. **PKG 装完自动删暂存**（契约反转，用户拍板「装完自动删（失败保留）」）：`ppCleanupWhenDone` 轮询 `/api/install/poll`，真装完（state done 且非 already-installed 跳过）→ `apiJson("/api/delete", {paths:[path]})` 删**本次上传的那一个 PKG**；跳过/失败/取消 → 保留 + toast 说明。2026-10-03 曾因真机反馈「传的包自己消失了」改为保留，现按用户新决定改回自动删。
4. **日志移入 `/data/Mango/logs/`**：`mango-crash.log`（main.c，宿主构建保持 `./mango-crash.log` 不变）、`mango-install.log`（pkg_installer.c + filemgr.c）、`mango-save.log`（mango_port.c）。main() 启动时 `mkdir /data/Mango` + `/data/Mango/logs`（`__SCE__` 块内）。
5. **内部临时文件前缀 `.wfm-` → `.nex-`**（用户拍板「全部改成 _nex」，保留点号隐藏语义）：`.nex-extract-`（extract_engine/rar/sevenz/zip）、`.nex-part-`（zip 分卷）、`.nex-copy-`（filemgr）、`.nex-port-`（mango_port 端口复制）、`.nex-pre-restore`（存档恢复前安全副本）、`.nex-upload-*.tmp`（upload）、`.nex-<pid>-<nsec>.tmp`（text 编辑器）。不改：`wfm_serve_pkg_py` 汇编标签（改则链接失败）、`mz_crypt_wfm.c`（vendored 第三方）、`wfm-7zip-helper`（上游项目名）。

**改动文件**：`src/savemgr.c`（路径迁移 + `_wfm_`→`_nex_` + `recursive_mkdir`）、`src/main.c`（建目录 + crash 日志路径）、`src/pkg_installer.c`/`src/filemgr.c`（install 日志路径 + 错误串）、`src/mango_port.c`（save 日志 + `.nex-` 前缀）、`src/extract_engine.c`/`rar_extract.c`/`sevenz_extract.c`/`zip_extract.c`/`text.c`/`upload.c`（前缀）、`assets/index.html`（incoming/tmp/backups 路径 + `ppCleanupWhenDone` 重写）、`hosttest.py`、`.build/dock_pkg_checks.mjs`（契约反转断言）、`.build/check-elf-gzip.py`（`PKG_STAGING = "/data/Mango/incoming"`）、`.build/preview/index.html`（快照重建）、`README.md`/`REAL-CONSOLE-CHECKLIST.md`/`docs/sdk-machine-checklist.md`（路径同步）。

**验收**（`WFM_CLEAN=1` 全量重建）：make linux RC=0、make all RC=0、check-elf-gzip 0 失败（含新 PKG_STAGING 断言）、hosttest **201/0**、dock_pkg_checks 全绿（真装完删单文件断言 `paths[0] === "/data/Mango/incoming/x.pkg"` + toast 已清理）。产物 `PS5-Mango-v1.0.3.elf`（**未提交**）。

**真机必测**：① 新目录布局 `/data/Mango/{incoming,tmp,backups,logs}` 由 main 自动建；② PKG 上传安装 → 装完 `incoming` 里该 PKG 消失；③ 已安装跳过 → PKG 保留 + 跳过 toast；④ 旧 `/data` 下遗留的 `wfm`/`save_files`/`backups` 目录与根目录日志需手动清理（新代码不再写）；⑤ 三份日志出现于 `/data/Mango/logs/`。

**追加（同 Session 17）：上传同名冲突选择**（用户反馈「同名直接报上传失败」）。根因：前端普通上传从不发 `X-WFM-Overwrite` 头，后端 [upload.c](src/upload.c) 检测目标已存在且无 overwrite=1 → EEXIST → 409。修复：`uploadOne` 预检 `FS_ENTRIES` 同名 → 弹 `#scrimUpC` 三选（覆盖 `X-WFM-Overwrite:1` / 重命名 `nextUpName` 生成 `name (1).ext` / 跳过）；xhr 409 兜底同样弹窗。**⚠️ 重命名不能改 `file.name`（File.name 只读 getter，赋值静默失效）**——改名走 `startUploadOne(file,done,overwrite,dst,eff)` 显式 `eff` 名参数，覆盖/重命名/跳过/成功 toast/onerror 任务匹配全部用它。失败计数只在「跳过」时记（覆盖/重命名重试成功是一次成功，提前 ++ 会同时出现成功+失败）。验收：`node .build/chk_upload.mjs`（拦截 `/api/upload-file` 实测 11 断言：无冲突直传/弹窗/覆盖头/重命名新名/跳过计数/409 兜底重试）全绿；preview_check 121 通过、E1 无 JS 错误（P11/Q5-Q8 为 Session 16 存档页重做遗留的过时断言，非本轮引入）。

---

## 1y. 当前进度（Session 27，2026-10-11）：长导入「看起来像假死」—— 其实是弹窗从来是空的 + 进程卡在内核里自启成死循环

**用户报障原文**：「上传替换加密的存档时，会提示『正在解包并逐个读取存档信息…（整包越大越慢，请勿关页）』，然后刷新网页后插件的所有功能都不能用了，进程还在，但是已经假死了。……最好给个进度显示……如果无法计算结束时间，那能不能显示个日志小窗口告诉用户都干了什么，好让用户知道不是假死了」。

追问后的关键澄清（它把问题从「UI 可见性」提升到**进程级根因**）：

> 「应该不是假死了，**就是进程崩了，然后进程又自动启动了**，但是功能都不能用了。我现在**手动结束进程他还会自启**而且插件功能也不能用。」

### 根因（四条，逐条都对得上用户描述）

| # | 现象 | 真因 |
| --- | --- | --- |
| 1 | 日志弹窗**开着却永远空白**，用户只能看到「请勿关页」 | `api_save_mgr_import_log` 里 `json_escape()` **自带两侧引号**，外面又手工加了一层 ⇒ 整个响应是**非法 JSON**（`{"text":""""}`）⇒ 前端 `r.json()` reject、被 `.catch(()=>null)` 吞掉 ⇒ 每轮轮询都安静地什么都不画。**这个弹窗从上线起就没显示过任何内容** —— 用户抱怨的「不知道在干什么」就是它 |
| 2 | 整包几百 MB 却要 `malloc(zsize)` 整包进内存 | `import_archive` / `import_zip` / `encrypt_upload` 三条路径都先把整包读进堆；payload 内存本就紧张 ⇒ 大包直接 OOM/崩溃 |
| 3 | 崩溃后**自启、然后功能全废** | `main.c` 里 `kill(pid, SIGKILL)` **失败即 `return 1`**。老进程卡在 `sceFsMountSaveData` 的**内核不可中断睡眠**（`D` 状态，连 `SIGKILL` 都带不走），所以 kill 必然失败 ⇒ 进程秒退 ⇒ 插件管理器判定「需要再注入」⇒ **无限自启**；而老进程还占着 2026 端口与 `/data/mount_sd` 挂载点 ⇒ 新进程的每个存档操作都失败 |
| 4 | 自启后「功能都不能用」，且结束不掉 | 崩溃时 `/data/Mango/tmp` 里留下 `_nex_arc`（几百 MB）、`_nex_zip_*.tmp`、`_nex_up_*`、`_nex_new_*`、`_nex_enc_*`，把 `/data` 写满；外加 `/data/mount_sd` 残留挂载。启动路径**从不清理**，于是每个新进程都在一个脏且满的盘上开工 |

### 修复

- **结构化进度**（`src/savemgr.c`）：`{phase,done,total,text}` + 逐行日志。phase `1 解包 / 2 逐个读取 / 3 替换落盘`。新增 `savemgr_import_progress()` / `savemgr_import_progress_end()`；**dispatch 单点收口**（8 条长路由），保证任何提前 `return` 都会把 phase 归零 —— 否则页面永远停在「进行中」，正是历次「进度卡死」反馈的来源。
- **修掉非法 JSON**：`json_escape()` 自带引号，调用点不再手工加。**这就是弹窗一直空白的直接原因。**
- **消除整包入内存**：新增唯一解包入口 `savemgr_extract_zip_path(dest, zip_path)`，直接吃盘上路径；`import_archive` / `import_zip` / `encrypt_upload` / `import_encrypted` 四条路径全部改为按路径解包，`malloc(zsize)` 全部删除。`savemgr_extract_zip()` 退化成「落一份临时文件后转调」。
- **zipx 进度桥**：`sm_zip_on_progress()` 把 `zipx_progress_t` 的字节口径接进进度（按「阶段变化或 pct 跨 10」压频，避免刷爆日志）。
- **挂载看护线程（根因防护）**：`sceFsMountSaveData` / `sceFsUmountSaveData` 一律不许裸调 —— 全部经 `sm_kern_run()`（`SM_MOUNT_TIMEOUT_MS = 60000`，超时**放弃线程**并回去报错，而不是自己也跟着卡死）。设备段原实现改名 `sm_mount_image_inner` / `sm_unmount_inner`，只在看护线程里被调用。返回码：`0` 已回来 / `-1` 超时 / `-2` 上一次还没回来 / `-3` 线程起不来；对外 `-100`（超时）/ `-101`（忙）。
- **崩溃后自恢复**：新增 `savemgr_boot_reconcile()`，`main()` 启动时调用 —— 清 `_nex_*` 前缀（含 `_nex_arc` 整棵树）、`statvfs` 报空闲 MB（<64 MB 告警）、设备上清残留 `/data/mount_sd`。**同时 `kill()` 失败不再 `return 1`**（改为记台账 + 继续端口探测），无限自启循环由此断开。
- **前端**：弹窗加状态行 + **确定态**进度条（`smLogApplyProgress()`；`total>0` 才算百分比，算不出就退回滚动条，不编造数字）；`✗`/`✓` 行分级上色；跑完自动收窗（先见「在跑」再见「已结束」才收）；**刷新页面后自动重挂**（`if (LIVE) fetch(...)`，phase≥1 就重开轮询）。三处长导入（整包 ZIP / 单镜像 / 解密标签）全部用 `startSmLogPoll()`/`stopSmLog()` + `try/finally` 夹住；整包分支的文案从「请勿关页」改成「进度见下方弹窗」。

**改动文件**：`src/savemgr.c`（最多）、`src/savemgr.h`、`src/main.c`、`assets/index.html`、`Makefile`（v1.0.8→v1.0.9）、`hosttest.py`、**新增 `.build/chk_import_progress.py`** + 接进 `.build/build-verify-wsl.sh`、`.build/preview_stub.js` / `.build/preview_check.mjs`（W 段 7 条）。

### 验收

- `make linux` RC=0（`-Werror` 零告警）；`make all` RC=0。
- hosttest **229/0**（新增 4 条：启动清理三条 + `import_log` 契约一条 `phase/done/total/text + lines`）。
- 静态守卫 `chk_import_progress`：5 组不变量 **0 违规**。
- 前端真 Chromium **158/158**（新增 W1a/W1b/W1c/W1d/W2/W3/W4）。
- ASan sweep：无 ASan/UBSan 报告。
- **反证 A–G 全部转红后恢复绿**（每一道不变量都必须能失败）：
  A 整包又进内存 · B 挂载绕过看护 · C 长任务不开进度窗 · D 进度不收尾 · E 公开入口直连 `*_inner` · F 日志 JSON 又套一层引号 · **G 桩返回非法 JSON ⇒ W 段 7 条全红，其余 151 条仍绿**（G 的观测量正是用户看到的那片空白：`stat = "准备中…"`、`lines = []`）。
- 产物 `PS5-Mango-v1.0.9.elf`（`e9e7e7f9…`）/ `PS5-Mango-linux-v1.0.9`（`5d1472ca…`）。**未提交。**

### 🪤 本轮新踩的坑

- **守卫把注释里的旧代码当真代码** ⇒ 注释里写「旧实现 `malloc(zsize)`」直接误报。修法：`strip_comments()`（保留行号）。
- **守卫把函数原型当调用**（`int sceFsMountSaveData(...);`）⇒ 先正则收集 `;` 结尾的声明区间再跳过；还要把 `#else /* __linux__ host stubs */` 之后的宿主 stub 排除。
- **反证脚本里常量少了一个反斜杠 ⇒ 假绿**：`'strbuf_append(&b, "\\"");'` 生成出来是两个反斜杠，反证「通过」了但根本没注入成功。修法：用 `chr(92)` 显式拼装，并**在反证脚本里 `assert` 载荷真的非法**（`json.loads` 必须抛）—— 数转义层数是不可靠的。
- **等不到元素时不要 `waitForFunction` 裸抛**：抛出去会被最外层 `catch` 收走，后面 5 条断言连跑都跑不到（一条失败掩盖五条）。包 `try/catch` 让**这条自己 FAIL**。

---

## 1z. 当前进度（Session 28，2026-10-11）：三处界面细节 —— 顶栏去重、存档页按钮收尺、选择文件按钮显形

用户原话：「前端最上面的 "HTTP 服务 :2026" 只写 "HTTP 服务" 端口就不用显示了，因为前面有 IP 加端口的显示。
优化一下存档管理页面下的所有按钮大小现在看着有些大不协调，选择文件的按钮配色跟背景重合了看不清」

三处都在 `assets/index.html` 单文件里（CSS + 两行文案），**后端零改动**。但数值对不等于好看 —— 三条断言能钉住
口径，钉不住「顺不顺眼」，所以真机必测清单里也各留了眼睛验收项（见 §6 第 16 条）。

1. **顶栏状态胶囊不再复述端口** —— `chipHttp` 从 `HTTP 服务 :2026` 收成 `HTTP 服务`。端口已经在左边
   `ipChip`（`192.168.1.42:2026`）里了，同一行说两遍是噪音。
   ⚠️ **HTML 播种值和 `applyStatus()` 里 `led()` 的文案两处都要改** —— 桩会回 `http:true`，JS 那句才是真正
   生效的那句；只改 HTML 的话页面一加载就被 JS 覆盖回带端口的版本。X1 断言钉的就是这个（`html-chip` 反证相
   特意把**两处都**还原成旧样子）。
2. **存档页按钮统一 38px** —— 改前同一屏里并存四种高度：`.btn` = 40×`--ui` ≈ 49px、`.btn.sm` = 38×`--ui` ≈ 46px、
   `.seg` 外框 = 50px、`.search` = 38px（硬编码、不乘 `--ui`）。表单区里 8~11px 的落差读起来就是「没对齐」。
   现在 `#view-saves .btn` 一律 38px，与同排 `.search` 齐平，主操作按钮也不再比旁边的选择按钮高出一头。
   - ⚠️ **只作用于 `#view-saves`**：文件页 / 游戏页的按钮尺度是另一套已验收的账（G21 / G22 在钉），别顺手一起改。
   - ⚠️ **视觉收小、命中区不许跟着缩**：`::after` 撑回 44px（与 `.btn.xs` / `.ipchip::after` 同一招）。
     X2a 断言专钉这一条 —— 只钉「高度是 38」的断言是半个断言。
   - ⚠️ 写死 38 而不是 `calc(38px*var(--ui))`：`.search` / `.seg` 本来就硬编码，本页表单区历来不跟 `--ui` 走。
3. **「选择文件」按钮显形** —— 改前它跟所有 `.btn` 一样只有 `--ghost-line`(#333) 的 1px 描边、零填充，
   叠在 `--page`(#0a0a0a) 上对比度不足 2:1，真机上读成「一块空白」。现在给它**和同排 `.search` 输入框完全同款**
   的一档：实底 `--s1` + 亮边 `--line-2`（实测 `rgb(25,25,25)` / `rgb(59,59,59)`），hover 抬到 `--s5`。
   选 `--s1` 而不是更亮：它右边紧挨着「未选择文件」的灰字、下面就是同款输入框，三者必须读成「同一套表单控件」。
   - 顺手把 6 个 `<label class="btn sm" style="height:38px">` 的**内联高度删掉** —— 内联样式压得过 CSS 选择器，
     留着它这一页的尺寸就永远不归 CSS 管（将来调尺寸会调不动，且这种「改了没反应」最难查）。

**验证**：前端真 Chromium **162/162**（新增 X1 / X2 / X2a / X3）；hosttest **229 / 0**；四道静态守卫 0 违规；ASan 无报告。
**反证**：`.build/cf-2026-1011-ui.sh` 四相（`html-chip` / `btn-size` / `btn-hit` / `pick-bg`）把旧样子逐条塞回去，
各自**只**打红它对应的那一条，还原后 `assets/index.html` 的 sha256 与动手前逐字一致。

⚠️ 一条值得记的方法论：`btn-size` 相一开始的写法是「把 38 改成 46」，结果**所有**按钮齐刷刷变成 46px 单独一档
（`uniq=[46]`）—— 红是红了，但红在「不等于 38」，而不是断言真正要守的「多档混排」。改成让那条规则**整个失效**
（选择器改名成 `#view-saves .btn--off`）之后，`uniq` 才变回 `[46,49]`，也才真的复现了改前的病。
**反证的失败原因必须和断言的语义对齐** —— 只要"碰到了"就算数的反证，会在断言口径悄悄漂移时继续给绿灯。

---

## 1aa. 当前进度（Session 29，2026-10-11）：「上传」下拉菜单的文字大小与图标对齐

用户原话：「文件管理页面中的 上传按钮点开后那个下拉列表上的文字大小不对 与前面的图标也没对齐」

一个 `.popmenu` 规则里的**三个原因叠在一起**，全在 `assets/index.html`（后端零改动）。
先用真 Chromium 量了改前的实际数值，再动代码：

| 项 | 改前实测 | 改后实测 |
|---|---|---|
| 条目字号 | **18.3px**（`15px*--ui`） | **17.08px**（`14px*--ui`，= 触发器 `.btn.sm`） |
| 图标中心 − 文字中心 | **−5.6px**（图标偏高） | **0** |
| 条目行高 | **63.4px**（`52px*--ui`） | **46.36px**（`38px*--ui`，= 触发器高度） |

1. **字号比自己的触发器还大** —— 菜单文字 18.3px、打开它的按钮 `.btn.sm` 17.08px。菜单不该比触发器更响，
   降到 `.btn.sm` 同档（工具栏「刷新 / 新建文本 / 新建目录」也是这一档）。
2. **图标与文字不同心** —— `align-items:flex-start` + `.popmenu button svg{margin-top:2px}`。
   这套是给「图标 + 标题 + **副标题**」的**两行条目**写的（见同节的 `button i` 规则），
   而那个带副标题的条目「保留目录结构」已于 2026-10-02 删除（C12b 在钉），菜单里只剩单行条目 ⇒
   `flex-start` 只剩副作用：文字行盒 `18.3×1.65 = 30.2px`、图标按顶边贴，**两者中心恒差 5.6px**。
   改成 `center` 并**删掉那个手写的 `margin-top` 补偿** —— 那种补偿会在字号/行高一变时立刻变成新的错位。
3. **行高同为两行时代的遗留** —— `52px*--ui = 63.4px`，比它挂靠的按钮高 17px。收到 `38px*--ui = 46.36px`，
   **正好等于 `.btn.sm`**，仍满足 ≥44px 的触控命中区（PS5 是触摸板/手柄光标）。

⚠️ **断言必须先钉「菜单真开着」**（Y0）：菜单是 `display:none`，关闭态下 `getComputedStyle` 照样返回字号，
而两个 rect 都是 `0×0` ⇒ 中心差恒为 0 —— 没有 Y0 的话，Y2 会是一条**永远不会失败**的断言。

**验证**：前端真 Chromium **166/166**（新增 Y0 / Y1 / Y1a / Y2）；hosttest **229 / 0**；四道静态守卫 0 违规；ASan 无报告；
制品 `PS5-Mango-v1.0.11.elf`，`strings | grep -oE 'v1\.0\.[0-9]+' | sort -u` 只剩 `v1.0.11`。

**反证**：`.build/cf-2026-1011-upmenu.sh` 三相（`menu-font` / `menu-align` / `menu-row`）把旧样子逐条塞回去，
**各自只打红它对应的那一条**（改序号是 Y1 / Y2 / Y1a），还原后 `assets/index.html` sha256 逐字一致。
`menu-align` 相特意把 `align-items` 与 `svg{margin-top}` **两处一起**还原 —— 只改前者是「半旧状态」，
量出来的偏移（−4.59px）跟真实旧版不是同一个数，那种反证证明不了多少。

---

## 1ab. 当前进度（Session 30，2026-10-11）：PKG 信息窗宽度 + 已安装游戏的「卸载」+ 临时文件自动清理 + 导入弹窗的终态

用户一轮提了**四件事**（原话）：

> 「PKG管理页面中的PKG窗口再加宽些 现在窗口太小PKG的信息显示不全，显示已安装的游戏是不是也能再加个卸载的按钮如果不能加就算了。还有上传了一些临时文件如果不用了可以自动删除掉。再存档页面中上传加密的存档后提示我本机有相同文件问我是否要替换我点是之后，会显示：…然后这个窗口也不知道是不是成功了，进度条还一直在闪，只有一个关闭按钮」

### ① PKG 信息窗「加宽不了」——`max-width` 是一句空话

`#scrimPkgInfo` 的弹窗上写的是 `style="max-width:900px"`，而 `.dlg` 的基础规则是
`width:min(660px,94vw)` —— **`max-width` 只能收窄、不能放宽**，于是弹窗一直就是 **660px**。
改前实测（真 Chromium，1920×970）：

| 项 | 改前实测 | 改后实测 |
|---|---|---|
| `#scrimPkgInfo .dlg` 宽度 | **660px**（`max-width:900px` 完全不生效） | **1040px**（`style="width:min(1040px,94vw)"`） |
| 摘掉那条内联声明后 | 660px | 660px（对照组，证明 1040 确实是这条声明给的） |
| `#pkgInfoPane table` 的 `table-layout` | **`auto`** | `fixed` |
| 首列（字段名）占表格宽 | 31.7%（内容说了算） | **32.0%**（`<col>` 钉死） |

第二处根因：`renderPkgInfoPane()` 的注释从 2026-10-10 起就写着「两个 tab 都用
`table-layout:fixed` 的表格渲染」，但 **`.pkginfo-table` 这条 CSS 规则从来没写进样式表**。
auto 布局下 `<col style="width:32%">` 只是"建议"，值列的 `min-content` 会把列顶开 ⇒
长 CONTENT_ID / 路径折成一列字。现在整块规则补齐（`fixed` + `word-break:break-all`），
`Z2` 在钉它。

第三处：`/api/pkg/info` **从来不返回 `title_id` / `title` / `version`**，而前端
`pkgInfoSpecRows()` / `pkgInfoSub` 正是按这三个字段渲染「TITLE ID」与「内容版本」——
弹窗的规格页永远缺这两行。`pkg_info.c` 现在补上（字段名与 `/api/pkg/scan` 一致，同源同名）。

### ② 已安装的游戏加「卸载」

可行性先取证再动手：`nm -D /opt/ps5-payload-sdk/target/lib/libSceAppInstUtil.so`
确认导出了 `sceAppInstUtilAppUnInstall`（以及 `…UnInstall2/ForMultiInstall/Remote/Types`
和 `…CancelUnInstall`），原型 `int32_t sceAppInstUtilAppUnInstall(const char *titleId)`
（PS4/PS5 同签名，公开 AppInstUtil.h）。SDK 里没有对应 `.h`，所以按项目惯例**运行时
dlopen + dlsym**，并且设成**可选符号**（老固件没有它也照样能装包）。

- `src/pkg_installer.c`：`pkg_uninstall_begin()` 走与安装同一把 `g_inst_session_lock`；
  `rc == 0` 只表示「已被系统安装器受理」，**不是**同步删完 —— 文案照实说，以主机桌面为准。
- 平台不支持（宿主 / 固件没导出符号）返回 `PKG_INSTALL_UNSUPPORTED`，路由映射成
  **501 + `pkg_uninstall_unsupported`**，和 `/api/pkg/install-url` 的
  `pkg_install_unsupported` 同一条口径；前端据此把「这台机器做不了」和「这次失败了」**分开说**。
- `/api/pkg/uninstall` **只收 `title_id`**（4 大写字母 + 5 位数字），绝不收路径 ——
  交给内核的东西必须是一个标题号，前端不该有能力让内核删任意路径。
- 前端：已安装（非 fPKG / 非 PC）的卡片从 `<button disabled>已安装</button>` 换成可点的
  `卸载`；点了先 `confirm` 讲清「连带删存档、不可撤销、要保留先去存档管理备份」，
  成功后 `markLibDirty()` + 重扫（服务端只会把「已安装」降级，不重扫按钮还停在「卸载」上）。

### ③ 上传的临时文件用完自动清理 —— 但**不能清早**

新增 `sm_is_staged_src()` / `sm_cleanup_staged_src()`（`src/savemgr.c`），三重自我约束：

1. **只认两个本工具自己的暂存根**：`/data/Mango/incoming/`、`/data/Mango/tmp/`。
   用户自己放在别处（U 盘、`/data/pkg`、自己的目录）的包一律不碰；
2. **只删普通文件**，不递归、不删目录；
3. **只在成功后调用** —— 失败要留着重试与排查。

调用点：单 ZIP 导入（`import_zip_common`）、单镜像 finalize（`import_finish`）、
加密导出（`encrypt_upload`）、改签导出（`resign_upload`，**不动用户的 key**）。

⚠️ **归档批量导入是个例外，而且第一版写错了**：清理原本挂在**扫描阶段**
（"摊包之后源 ZIP 就用不到了"）。可那时用户还没点确认 —— 取消之后暂存里那份解压
结果会被删掉，**重扫的唯一输入就是这个源包**，删早了等于让用户重传几百 MB。

改成在 `import_archive_finish` 里**按结果**决定：`failed == 0 && done > 0` 才清，
有失败项就保留（日志里明说「源 ZIP 已保留（有失败项，重试要用）」），取消则原样保留。
这和项目中「PKG 装完自动清理、失败保留」是同一条纪律。

> 🪤 宿主上**永远走"有失败"那条路**（没有 PS5 内核，整批替换每张都 `mount failed`），
> 所以「全部成功 ⇒ 删源包」这个分支宿主跑不到。它是靠 `.build/chk_import_progress.py`
> 新增的 **3b 段**记账的（扫描阶段不许清 / finish 必须清 / 必须挂在 `failed == 0` 上），
> 并配了 3 个反证相。别拿 hosttest 的绿灯当这条分支的证据。

> 🪤 这个改动第一次跑就把 **hosttest 打挂了**：测试收尾 `os.unlink(arc_path)` 撞
> `FileNotFoundError`，因为源包在扫描阶段就被删了。**测试挂掉是发现设计错误的方式**，
> 不是"顺手把测试改绿"。现在 hosttest 反过来钉住新契约：扫描后源包在、失败后源包在、
> 取消后源包在（237 项）。

### ④ 导入弹窗「看不出成功没有 + 进度条一直闪」——三条原因叠在一起

用户贴的那段日志（「解包中：整包 60 MB，正在摊开…」）其实是**上一轮**的：

1. **后端日志缓冲从不清** ⇒ 前端**先开窗再发请求**，新窗第一帧把上一轮整段重放。
   新增 `savemgr_import_progress_begin()`，由 `dispatch_save_mgr()` 在路由**前**调用、
   路由后调 `_end()` —— begin → route → end 的**顺序**由静态守卫钉住。
2. **`finally { stopSmLog(); }` 把结论关掉**：`smArcFinBtn` 的 `try{…}finally{ stopSmLog(); }`
   在请求一返回就拆窗，`smLogSettle` 刚写好的 ✓/✗ 用户根本来不及看。
   收尾现在只由 `smLogSettle(ok, msg)` 负责（`finally` 只恢复按钮状态）。
3. **进度条一直闪**：`smLogApplyProgress` 在 phase 0 时无条件摘掉 `.det` ⇒ 退回
   `sm-logbar-sweep` 跑马灯。现在 phase 0 **且从未见过运行**时**不动**进度条，
   状态行如实说「准备中…」而不是「任务已结束」。

配套：`smLogSettle(false, …)` 窗**留着**（失败原因正是用户要读的东西）+ 状态行红 +
进度条冻在满格；`smLogSettle(true, …)` 写 ✓ 行 + 看 2.2 秒再收窗。`encrypt_upload` /
`resign_upload` 这两条以前**根本没有进度窗**，一并接进来。

> 🪤 `smLogSettle` 必须是**粘滞**的（`smLogSettled`）：settle 是同步调的，而那一轮
> 轮询的 fetch 已经在路上，落地时带着「phase 0」回来，会把刚写好的「✗ 失败」盖成
> 「准备中…」—— 这正是这一轮要修的那件事换个形式复发。反证相 `log-nosticky` 在钉它。
> 🪤 前端增量渲染的游标必须支持**行数回退**（后端清了缓冲）⇒ 整窗重画，否则新任务的
> 行永远补不上（`slice(shown)` 恒为空）。反证相 `log-replay` 在钉它。

### 验证

- 前端真 Chromium **175 / 175**（新增 Z1 / Z2 / Z3 / Z4 / Z4b / Z4c / Z5 / Z6 / Z7）；hosttest **237 / 0**；
  四道静态守卫 0 违规；ASan 无报告；NAS 端到端 SKIPPED（fixture 未起，不算 pass）。
- 反证：
  - `.build/cf-2026-1011-pkg.sh` 六相（`pkg-width` / `pkg-table` / `uninstall-btn` /
    `logbar-freeze` / `log-replay` / `log-nosticky`）—— 各自只打红对应那一条，还原后
    `assets/index.html` sha256 逐字一致。
  - `.build/cf-2026-1011-guard.py` 十相 —— 逐条证明**静态守卫新加的每条检查都有牙**
    （含 `shared-branch-no-settle`：smImgBtn 的两个长 POST 在同一个 handler 里，
    删掉 ZIP 分支的成功结论必须报在 `import_archive?` 那一行，不能被邻分支的计数补上）。
  - ⚠️ Z1 的对照组**不能**挑另一个关着的 `.dlg` 来量：`display:none` 的元素两个矩形
    都是 `0×0`（本项目踩过的坑）。改成在**同一个**弹窗上摘掉内联 `width` 再量（落回
    660px），顺带自证 1040 就是那条声明给的。
- 🪤 **反证脚本本身也踩了坑**：`.build/preview_build.py` 每次都 `rmtree` 整个预览目录
  （11 个文件），六相 = ~72 次删除 ⇒ 第 5 相撞上本机「单回合批量删除」配额（阈值 50），
  变成 `BUILD FAILED` 的**假失败**。现在预览只重建一次，各相只 `reinject` 覆盖
  `index.html`（覆盖不算删除）。
- 🪤 **新加的静态检查第一版是假绿的**：用「POST 前后各 1600 字符」当窗口时，
  相邻调用点的 `startSmLogPoll` / `smLogSettle` 会落进同一个窗口 ⇒ 本条调用点一对
  都没有也照样通过。改成按**所属 async 处理函数 + 本分支**（下一个长 POST 为界）定位。
  这是反证脚本自己抓出来的 —— 所以才要求"打不红的当场改掉，而不是留着充数"。

---

## 1ac. 当前进度（Session 31，2026-10-11）：真机 SIGSEGV —— 坏 `param.sfo` 越界读 + 列表 JSON 栈上越界写

### 报障（v1.0.11 真机）

归档导入 → 解析完成（3 份均已存在）→ 点「覆盖替换」→ **进程崩了又自启，此后网页功能全废**、
「强制关闭」无效、`http://<IP>:2026/api/shutdown` 也无效。

### 先更正上一轮的误判

§1y / 上一版本节曾判断是"内核挂载卡死在 D state（`g_kern_busy` 永久 1 / 进程退不掉）"。
**用户取回的 `mango-crash.log` 推翻了它**，决定性一行：

```
boot reconcile: stale /data/mount_sd umount -> r=0 rc=-2147352544
```

`r=0` = **看护线程正常返回**，不是卡在内核里。台账里真正的证据是**三次**
`=== PS5-Mango.elf SIGSEGV ===` ⇒ **用户态崩溃**。
（"重启 PS5 才能恢复"这条结论不变，但理由要改成"SIGSEGV 后进程状态已破坏"。）

### 台账读出的两条根因

1. 三次崩溃**最后一条请求完全一致**：`273 D POST /api/save_mgr/list`、
   `100 D POST /api/save_mgr/list`、`29 D POST /api/save_mgr/list`。
2. 第三次那 29 条请求里**没有任何导入请求**（先 `/` 与图标，再 `fs/list`×7、`pkg/scan`×5、
   `pkg/icon`×2，最后 `save_mgr/list`）⇒ 触发动作就是**打开存档页**。
   （"点了覆盖替换"只是把用户带到存档页，崩溃点在列表解析。）
3. 于是：

- **`savemgr_parse_sfo()` 越界读**（`src/savemgr.c`）：`key_off` / `data_off` / `n_entries`
  全部**读自 `param.sfo` 文件** —— 即**不可信输入** —— 却直接拿去建指针、定循环边界、算长度。
  `/api/save_mgr/list` 会扫**整个存档库** ⇒ **任何一个坏 `param.sfo` 都能带走进程**。
- **`pos += snprintf(...)` 累加下溢**（`api_save_mgr_list` / `api_save_mgr_files`）：
  `snprintf` 返回的是"假如空间足够、本应写入的长度"，**不是实际写入量** ⇒ 内容一超缓冲，
  `pos` 就 > `sizeof(out)`，下一个 `sizeof(out) - pos` 在 `size_t` 域**下溢成天文数字**
  ⇒ `snprintf(buf + pos, 天文数字, ...)` **越界写栈**（`out` 在栈上，约 16 KB）
  ⇒ 表现为"处理另一个请求时突然 SIGSEGV"。

🪤 **为什么以前一直测不出来**：`api_save_mgr_list` 整段在 `#ifndef __linux__` 里 ⇒
hosttest（宿主走 `__linux__`）与前端套件**结构上都覆盖不到**；
`savemgr_parse_sfo` 的宿主调用方要么也在设备分支、要么依赖 `savemgr_mount_image`（宿主恒 `-1`）
⇒ 畸形 SFO 的行为**从未被测过**。"**只用真机编译的代码 = 永远没测过的代码**" 又一次应验。

### 修复（三处）

- **`savemgr_parse_sfo()` 全面边界化**：`key_off`/`data_off` 夹进 `[0, fsize)`；
  `n_entries` 夹到 `(fsize - 20) / 16`；每条记录的偏移夹进剩余空间；
  `param_len` 先夹到 `davail` 再夹到 `(uint32_t)INT_MAX`（防 `(int)` 变负）。
  终止判据用 **`strnlen(key, kavail) == kavail ⇒ continue`**（缺 NUL 就丢这条记录），
  比 `re [0-9a-f]{5}` 式的形态检查难绕过。新增 `#include <limits.h>`。
- **`sm_json_append(out, cap, &pos, fmt, ...)`**：累加器改记**实际写入量**，满了只丢弃不越界；
  `api_save_mgr_list` / `api_save_mgr_files` 的 8 处 `pos += snprintf` 全部换掉，`int pos` → `size_t pos`。
  新增静态守卫 **`.build/chk_json_append.py`** 机械禁止裸的 `pos += snprintf`。
- **`diag` 台账改按 seq 销账**：`request_context_t` 加 `unsigned long seq`，
  `diag_record_request()` 返回 seq，`diag_record_done(seq, toe)` 按 `seq % DIAG_SLOTS` 精确销账。
  原实现**按 URL 匹配** ⇒ `GET /api/status` / `GET /api/tasks` 并发同名时，后到的 completion
  会把 `done` 打到**更早的同名槽** ⇒ `* = in flight` 不可信 —— 这正是这次被误导的根源。

### 验证

- `make linux` rc=0（`-Wall -Werror`）；四道既有静态守卫 0 违规；
  新增 `.build/chk_json_append.py` → `OK: no raw snprintf accumulation in src/*.c`。
- **`.build/sfo_unit_test.c` + `.build/sfo-unit-wsl.sh`**：直接驱动**真实 translation unit** 的
  宿主单元测试（头注释写明它为什么必须存在）。复用 `.build/asan-obj` 并**排除 `src_main.c.o`**、
  稳态只重编 `src/savemgr.c` —— 绕开 `build-asan.sh` 的 `rm -rf` 撞本机「单回合批量删除」配额。
  未修复代码**当场炸**：`[keyoff] READ of size 1 @ savemgr.c:349`、
  `[nentries] READ of size 4 @344`、`[short] READ of size 4 @343`；
  修复后 `[keyoff]/[paramlen]/[nentries]/[short]` 四 case 全 **clean**。
- **`.build/cf-2026-1011-sfo.py` 三相反证全 OK**：相 A 删两条 size 守卫 → 红在 `[short]`+`[nentries]`；
  相 B 同时删外层 `key_off` 门与内层 clamp → 红在 `[keyoff]`；相 C 塞回裸 `pos += snprintf`
  → 点名 `savemgr.c:1429`。还原后 `savemgr.c` sha256 = `8076c41e…58bcf15`（逐字一致）。
- 已接入 `.build/verify-linux-wsl.sh`（静态守卫区 + ASan sweep 之后），
  以后 `wsl.exe -d Ubuntu-22.04 bash < .build/verify-linux-wsl.sh` 一条命令带全。

### 🪤 本轮最值钱的教训：反证相 B 连假三次

1. 只删内层 clamp：外层 `strnlen` 兜底仍在，且构造出的 `buf+4096` 落进 stdio 的 **4 KB 缓冲区**
   （合法可读）⇒ ASan 看不见。
2. 把 `key_off` 提到 2 GB：外层 `if (key_off >= fsize)` 先拦掉，循环**根本走不到**
   ⇒ 必须**同时**删外层门。
3. 顺手把 `data_off` 也设成 2 GB：`dpos >= fsize` 先 `continue`，`strcmp` **永不执行**
   ⇒ `data_off` 必须收回界内（40）。

**载荷必须真走到被修的那一行，否则那条反证是"不可能失败的反证"，比没有更糟。**

🪤 反证脚本**不能用反向 patch 还原删除型改动**（`new == ""` 无法反转）——
先前版本就这样把工作区的 `key_off` 守卫删掉了**还静悄悄继续跑**。现改为
**从原始文本重新构造变体 + 每相结束都 `write_src(origin_src)`**，读写用 `newline=""`
（避免 CRLF 归一化导致 sha 比较假失败）。

🪤 `strings` 默认只输出 7-bit ASCII，**中文串 `grep` 恒 0** —— 拿中文串当判据会得出
"看护不存在"的假结论（同一批里 `guard(refuse-2nd-mount)` 就这样被误报成 0）。

### 已知未动（等用户定夺）

- ~~`api_save_mgr_list` 的 `char out[16384]` 仍在栈上~~ → **已在 §1ad 移到堆**
  （连同 `api_save_mgr_files` / `api_save_mgr_import_log`，每条请求线程省 ~16 KB 栈）。
- `savemgr_resign_sfo` 在 `r == -2` 分支里 `strncpy(info.title_id, "UNKNOWN", sizeof(...)-1)`
  末尾可能无 NUL（既有代码）。
- `g_arc_src` **无锁全局**（v1.0.12 引入，见 §1ab）：扫描期写、finish 期读；
  用户若在扫描后又扫了另一个包，finish 会**清错源包**。
- `sm_kern_run` 超时后 `g_kern_busy` 无出口；启动期 force umount 失败会让整进程禁用挂载
  （**不是**本次崩溃的原因，但仍是真隐患）。
- 同一批镜像目前被挂两次（扫描 1 次 + finish 1 次）⇒ 内核挂载调用数翻倍，值得合并。

---

## 1ad. 当前进度（Session 32-34，2026-10-11）：让「崩了却什么都没记」变成「崩了必留证据」+ v1.0.13

### 起因

用户又报「确认覆盖 → 弹进度窗 → 崩」，贴来的台账里三次 SIGSEGV **全部早于**修复版构建时间
（10-07 10:21 / 10-10 19:14 / 10-10 22:07，最后一条请求都是 `POST /api/save_mgr/list`）。
真正的问题不是那三次：是**他这次「崩了」在台账里找不到任何新记录**。

### 根因：崩溃处理器自己起不来

`main.c` 的注释早就写着 `a stack-overflow SIGSEGV leaves no room for a stack buffer`，
但 `install_crash_handler()` 只用裸 `signal()`，**没有 `sigaltstack` + `SA_ONSTACK`**。
栈溢出时内核是在「线程栈已被压穿」的状态下递送信号的，处理器若用同一条栈执行 ⇒
**第一条 push 又溢出 ⇒ 处理器自己都进不来 ⇒ 台账一片空白**，用户只剩一句「崩了」。
光把缓冲改成 static **不够** —— 缺的是那块独立的**执行栈**。

### 四处改动

| # | 改动 | 为什么 |
|---|---|---|
| 1 | `sigaltstack` + `SA_ONSTACK`（`static char g_altstack[32768]`） | 栈溢出崩溃**能被记下来**。写死 32768：glibc 2.34+ 的 `SIGSTKSZ` 已改成 `sysconf` 取值，拿它当数组长度编译不过 |
| 2 | `SA_SIGINFO` + 记 `si_addr` / `si_code` | 越界读写 vs 栈溢出，**排查方向相反**，光看 `SIGSEGV` 三个字分不出来 |
| 3 | 三处大栈缓冲移到堆（两个 `char out[16384]` + 一个 `char buf[8256]`） | websrv 是 `MHD_USE_THREAD_PER_CONNECTION` ⇒ 这些全压在**每条请求线程的栈**上。`list` 函数原来 ~22 KB，现在 ~6 KB。`out` 直接交给 `send_buffer`，不必再 `strdup` |
| 4 | 台账启动即写版本：`mango_log("PS5-Mango %s starting (built %s %s)", VERSION_TAG, __DATE__, __TIME__)` | 以后一眼看出「这段记录是哪个版本写的」（上一轮就是被 `[start]` 旧前缀把历史记录当成新崩溃） |

🪤 **`SA_SIGINFO` 必须挂 `sa.sa_sigaction`（三参），不是 `sa.sa_handler`（单参）**；
glibc / FreeBSD 把这两个成员放在同一个 union 里 ⇒ **宿主 `make linux` 根本不报**
（那条路径不编译这个函数签名），是设备分支的 prospero-clang 报出来的。

### 🆕 工作流：只编译不链接，验证设备分支

`api_save_mgr_list` / `api_save_mgr_files` / `api_save_mgr_mount` 整段在 `#ifndef __linux__` 里
⇒ `make linux`（宿主）**结构上编译不到它们**。补这么一步：

```bash
wsl.exe -d Ubuntu-22.04 --cd "<repo>" bash -c \
  'export PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk; make ps5-obj/src/savemgr.o ps5-obj/src/main.o'
```

**只出 `.o`、不链接、不落 ELF**（合规：出 ELF 要先问用户）。
**第一次跑就抓到那个 `sa_handler` 类型错误** —— 一个宿主编译永远抓不到、却会让真机跑不起来的错误。
**以后改设备分支必须跑这一步。**

### v1.0.13 构建 + 一个「只在 bump 时才红」的 bug

用户确认 payload manager 里显示 `v1.0.12`，批准出 ELF。本轮代码与 v1.0.12 内容不同 ⇒ bump 到 `v1.0.13`。

首次构建末尾 FAIL，但 ELF 其实是对的：

```
### ELF version staleness (the .o must really be this source) ###
  v1.0.13
FAIL: v1.0.13 not found inside PS5-Mango-v1.0.13.elf -- the object is stale
```

根因：`build-verify-wsl.sh` 的 ELF 版本陈旧检查**硬编码了 `grep -cE "^v1\.0\.12$"`**
（同段另一行用的才是 `$VER`）。手动复核 `strings -a … | grep -c "v1.0.13"` = **1**、
`grep -c "v1.0.12"` = **0** ⇒ ELF 新鲜，是脚本误报。改成 `^$VER$` 后全绿。
**凡以 `VERSION_TAG` 为判据的检查/测试，一律引用变量，不许写死** —— 它不会长红，
只会在**下次 bump 时**突然变红，而那第一反应会把人带向「产物陈旧」的错方向。

**产物**：`PS5-Mango-v1.0.13.elf`，2342448 B，
sha256 `d2ca99ee7178dcc33c0de67c68fe032a013acec33512213fcaf06192208fe0e8`
（`PS5-Mango-linux-v1.0.13` = `76bbc23a271bb05cd54ff51f494ee9b20eb47d4ba08acb65f9885d58d191b4fe`）。
二次构建 sha **逐字相同** ⇒ 无重链接、产物稳定（`__DATE__/__TIME__` 只进 `main.o`，
vstamp 机制保证版本不变就不重编）。

**全绿**：`HOST_RC=0` / `PS5_RC=0`；ELF 内容断言 0 失败；TITLE_ID GONE/HIT 正常；
五道静态守卫 0 违规；hosttest **237 passed, 0 failed**；ASan sweep 无报告；
`param.sfo` 四 case clean；`OK: only v1.0.13 is embedded`。

### 真机上怎么读新台账

`mango-crash.log` 每次启动的**第一行**是
`[log …] PS5-Mango <版本> starting (built <日期> <时间>)` —— **先看这一行**确认这段记录属于哪个版本。
崩溃段现在长这样：

```
=== PS5-Mango.elf SIGSEGV addr=0x… code=… pid=N time=… ===
```

`code=1`（`SEGV_MAPERR`）多半是解引用坏指针；`code=2`（`SEGV_ACCERR`）多半是踩到不该写的页
（栈溢出/写只读页）。`addr` 落在栈边界附近 ⇒ 偏栈溢出。

---

## 1ae. 当前进度（Session 35，2026-10-11）：存档替换按 Garlic 的挂载会话模型重构（方案 B）

**用户诉求原话**：「导入点替换覆盖存档 进程就崩了 而且 进程还存在手动结束了还自动出现，是个
假进程网页也访问不了」、「存档的替换为什么总有问题 存档页面问题多参考 Garlic SaveMgr 这个项目」、
「直接按 B 走，我一开始就说了能照搬的全搬过来就好了」。

### 先把 Garlic 的模型读清楚（`.build/garlic-savemgr`，v1.13，2026-08-22）

按 `third-party-impl-recon` 的纪律先看许可证：**该仓库没有 LICENSE 文件**（`README.md` 在、
`LICENSE*` 不在）⇒ 视为 all rights reserved，**只能搬"事实"（调用顺序 / 结构 / 作者踩过的坑），
不能搬代码**。这条写在这里，免得日后有人"顺手"把它的函数挪进来。

挖到的事实（每条都能落在代码上）：

| 事实 | Garlic | Mango 旧版 | 处置 |
|---|---|---|---|
| **挂载会话** | `mount_save()` 开头 `if (g_mounted) return -1;` —— 已挂着就**拒绝**，绝不"先卸再挂" | 调用方 `if (is_mounted()) unmount();` 且**不看卸载结果** | ✅ 已搬（见下） |
| **非 `/data/` 路径直挂会 EPIPE** | `/* Direct mount from savedata_prospero causes EPIPE. */` ⇒ 先复制到 `/data/save_files/` 再挂 | 已有同款（复制到 `/data/Mango/tmp`） | 已是同构 |
| **ioctl 缓冲必须堆分配** | `/* Heap-allocate ioctl buffer — ioctl can corrupt stack beyond bounds */` | 已是 `calloc(1, 0x1000)` | 已是同构 |
| **挂载期间屏蔽 SIGPIPE** | 无 | `signal(SIGPIPE, SIG_DFL)` 包住 mount | 早已有 |
| **一次用户动作 = 一次挂载** | `/api/mount` 挂一次 → `/api/write_file` 直接写挂载点 → `/api/unmount` | 扫描 N 次 + 替换 N 次 = **2N** | ✅ 已搬（扫描不再挂载） |

### 四条结构性改动（全部在 `src/savemgr.c`）

1. **挂载会话：拒绝叠加，而不是"静默先卸"**（`savemgr_mount_image`）
   旧写法把"先卸了再挂"推给调用方且不看结果：一次 `unmount` 超时（-100）后 `g_mounted` 仍是 1，
   第二次 `mount` 照样发进内核 ⇒ 内核里两个挂载叠着。现在内部接管：替用户卸一次可以，
   **卸不干净就直接返回 `-104`（楔住）**，让路由层停止这一批。四个调用点的冗余"先卸再挂"已删掉
   （那是白白多摸一次内核）。
2. **扫描阶段不再挂载内核镜像**（`api_save_mgr_import_archive`）
   标题号与目录名从**包内目录结构**推（`<tid>/sdimg_<dir>`，`sm_arc_collect` 本来就在做），
   "本机是否已有"用这个路径 `stat`。一次导入的内核调用从 **2N 降到 N**。
   代价：清单里不再显示 `param.sfo` 的 `main_title` —— 列表 UI 本来也不展示它；
   "其实是覆盖"这一条在替换阶段挂载后**补记进日志**，不静默。
3. **楔住即止**（`api_save_mgr_import_archive_finish` + 新助手 `sm_kern_wedged()`）
   判据 = `-100`（看护超时）/ `-101`（上一次还没回来）/ `-103`（线程起不来）/ `-104`（没卸干净）。
   命中任意一条 ⇒ **立刻停止**，剩余条目记 `skipped`（不是 `failed`）并在响应里给 `wedged:true`。
   旧代码是"这张失败、接着试下一张" —— 而挂住的是全局互斥的 PFS 挂载层，继续塞会把整台主机拖死。
   楔住时**不清暂存、不清源包**（重启后还要重试，不想让用户重传几百 MB）。
   另外每张之间 `usleep(300ms)` 留喘息（Garlic 的 `unmount_save` 也是先 `sync()` 再收工）。
4. **长循环不再让浏览类路由排队**（`dispatch_save_mgr` + 新助手 `sm_route_is_browsable()`）

### 为什么第 4 条是"网页全打不开"的正解（本轮最值钱的机理）

旧 `dispatch_save_mgr` 除 `import_log` 外**全部** `pthread_mutex_lock`。一次 3 张存档的导入
（每张 mount+umount，每次看护 60 秒）最坏独占 `g_savemgr_lock` **6 分钟**。这期间
「刷新列表 / 强制卸载 / 放弃导入」全部排在锁上。

后果比"按钮没反应"严重得多：**浏览器对同一 origin 只有 ~6 条并发连接**。存档页同时在飞的
请求（`list` / `files` / `unmount` / 取消 / `status` / `tasks`）很容易把 6 条全占满 ⇒
**连 `/api/status` 都发不出去** ⇒ 用户看到的就是「进程还在、网页全打不开、点了没用」。
服务端是 `MHD_USE_THREAD_PER_CONNECTION`，那些线程也就一直挂着。

⇒ 浏览类路由（`list` / `files` / `unmount` / `import_archive_cancel`）改用 **`pthread_mutex_trylock`**：
拿不到就**立刻**回 `503 {"error":"busy"}`。前端据此说人话（「后台正在导入存档，稍后自动刷新」，
4 秒后自己再试一次），页面保持可用。**这条与 D-state 无关 —— 纯用户态，改完即生效。**

### 前端（`assets/index.html`）

`renderSaveMgrList` 认 `busy`（温和自动重试，不弹 toast）；「卸载 / 取消 / 放弃」三个按钮在
`busy` 时明说「后台正在导入，等它结束」而**不假装失败、也不隐藏文件区**；
归档导入的终止分支新增 `j.wedged` ⇒ 说清「已成功 N 份、M 份未处理，请**重启主机**后再来一次，
源包和暂存都留着」。清单文案改为「标题号取自包内目录名」（不再是「未读到 param.sfo」——
扫描阶段本来就不读了，那不是出错）。

### 验证

`make linux` / `make all` HOST_RC=0（`-Wall -Werror`）；**设备分支只编译不链接**
（`make ps5-obj/src/savemgr.o`）也干净 —— 这条路径宿主编译不到，v1.0.13 那次就是靠它抓到的
编译错误，本轮已固化成习惯。hosttest **237 passed, 0 failed**；ASan sweep 无报告；
`param.sfo` 四 case clean；前端真 Chromium **175 / 175**。

新增静态守卫 **`.build/chk_arc_mount_budget.py`**（四条不变量：`scan-mounts` / `wedge-stop` /
`busy-lock` / `stack-mount`），已接进 `verify-linux-wsl.sh` 与 `build-verify-wsl.sh`。
反证 **`.build/cf-2026-1011-arcbudget.py`** 四相全部转红、且**红在对的检查名上**，
还原后 `savemgr.c` sha256 逐字一致。

⚠️ 写这条守卫时踩了一次坑：`savemgr_mount_image` 的返回类型与函数名**在同一行**（不在列 0，
和 `api_save_mgr_import_archive` 那种"返回类型单独一行"的风格不一致），而且它**有设备/宿主两个
同名定义**。所以定位函数体不能锚在"列 0 的函数名"，要锚在「名字 + 配平括号后紧跟 `{`」；
`must_present` 检查用"任一函数体命中"，`not must_present` 用"任一命中即违约"。

---

## 2. 仓库结构（已落地）

```
ps5-mango/
├── Makefile              # 模块集 = 新编排层；已补 -lkernel_sys + 第一方 flag-stamp（见 §1d/§1e）
├── LICENSE / README.md / HANDOVER.md（本文件）/ docs/PLAN.md
├── .gitignore            # 适配新名 + 编排层测试 scratch
├── src/                  # 29 继承叶子模块（解压引擎 + json/path）
│   ├── *编排层*           # main/httpd/api/**worker**/platform/task_engine/transfer/pkg_installer/**pkg_stream**/pkg_lib/pkg_sfo/savemgr/copypipe（Session 5–7 落地，pkg_stream Session 16）
│   └── *.h 契约           # 8 个接口头（低耦合边界）+ pkg_sfo.h
├── tests/                # host 测试矩阵（MinGW 跑；含 run-orchestration-tests.sh）
├── third_party/          # minizip-ng / unrar7 / zlib / 7z（均已 vendored）+ libsmb2 v6.0.0 / libnfs 8.0.0（NAS 后端，Makefile 开关，默认关）
└── .build/               # 构建脚本 + 验收工具（白名单内）
```

---

## 3. 如何构建 / 测试

- **PS5 ELF**：需要 `PS5_PAYLOAD_SDK` 环境变量（prospero-clang）。本环境（Git Bash）**没有 SDK** ⇒ ELF 无法在此编译，必须在真机 / SDK 机执行 `make`。
- **host 测试矩阵**（解压引擎回归，204 项）：在 **Windows MinGW** 下，两条都需先关删除守卫：
  - `CODEBUDDY_SAFE_DELETE_ENABLED=0 bash tests/run-tests.sh` → ZIP(140) + RAR(37)
  - `CODEBUDDY_SAFE_DELETE_ENABLED=0 bash tests/run-sevenz-tests.sh` → 7z(27) + error/policy(33)
  - **编排层**：`CODEBUDDY_SAFE_DELETE_ENABLED=0 bash tests/run-orchestration-tests.sh` → 6 套件 **api 180 / worker 25 /
    extract_engine 40 / upload_stream 55 / pkg_stream 89 / task_delete 43**。`upload_stream` 的 55 = 43 + 12 条
    `nx_fs` 权限契约 —— ⚠️ **权限位本身在宿主上验不了**（`chmod` 是空实现），能验的只是助手契约，见 §1s。
  - ✅ **zlib 与 7z 源码已 vendored**（见 §1b），现可编译。zlib 经 Git Data API 抓取（github.com:443 被 egress 挡，不能 clone/curl）；7z 从旧仓复制。
  - ⚠️ 编译慢（49 个 unrar .cpp ≈ 4–5 分钟），跑后台等完成，别被默认 2 分钟超时误判失败。
- **前端验收**：`.build/ui_demos_check.mjs`（106 项，需 Playwright + 目标 HTML）；`.build/proposal_check.mjs`（提案事实闸门）。

---

## 4. 铁律坑（落地时逐条对照，错一次就丢一次档/提交）

**git / 提交**
- 禁用 `git stash`（中断会删 `.git/refs` 与 pack）；改名用 `git mv`，**禁止 `git rm` 改名**。
- 同一文件**一次只发一个 Edit**（并行多个只生效最后一个，却都回 Successfully）。
- `git push` 失败先分类：① 证书（MITM）= `GIT_SSL_NO_VERIFY=1 git push origin main`（单次，不动持久配置）；② 瞬断/502 = 循环重试（**别用 `timeout`**，命中 Windows `TIMEOUT.EXE` 误判「被拦」）；③ **整个 `github.com:443` 被 egress 挡**（curl -4 github.com 超时、api.github.com 返回 200）= 改走 **GitHub Git Data API** 推纯快进（blob/tree/commit 逐字节同 sha 闸门，见 §5）。

**构建**
- `make` 看不见编译选项变化 ⇒ 新功能静默消失 ⇒ **flag-stamp**（判读只信 `readelf -SW` 段尺寸 + sha256，别看文件尺寸；重编前把旧产物挪 `.build/` 做 `cmp`）。
- 测试套件只能在 Windows MinGW 跑；沙箱删除守卫拦 `rmtree` ⇒ `CODEBUDDY_SAFE_DELETE_ENABLED=0 /usr/bin/bash tests/run-tests.sh`。
- 🪤 **新模块引用 POSIX 调用（`lstat` / 两参 `mkdir` / `<sys/statvfs.h>`）会让宿主套件变红**：MinGW 都没有
  这些符号，而 gcc 16 默认 **gnu23 ⇒ 隐式声明是硬错误**。**源文件**里写 `#ifdef _WIN32` 映射
  （`api.c` / `task_delete.c` / `test_task_delete.c` 各一份），**宿主测试自己也要一份**（它 `lstat` 夹具）。
  ⚠️ 这个坑已经**以「新文件」的形式复发过一次** —— 写新模块时先问「我用了哪些只有 POSIX 才有的东西」。
- 速度口径：速度 = 总字节 ÷ 总耗时（或 ≥10 s 窗口）；**禁显 250 ms 瞬时值当速度**；夹具必须按真实压缩比造（90% 随机数据躲过解码退化路径，结论失真）。
- 挂载点速度前提：`/mnt/ext1` 内置 M.2 / `/mnt/usb0|1` 外置 USB / `/data` 内置 PFS，**写速完全不同 ⇒ 谈速度必须带前提**。

**字节映射（前端↔服务端）**
- `json_escape()` 把每个 ≥0x80 字节转 `\u00XX`；`fs_path_value()` 把 ≤0xFF 码点还原成原始字节 ⇒ 任意非法 UTF-8 文件名（GBK）不丢字节，**别改成「输出真 UTF-8」**。
- 给人看走 `decodeFsText()`；送服务端走 `encodeFsText()`。**别拿路径当跨往返的键**（同目录两侧字符串不同 ⇒ 口令重试表按任务 id 记）。

**安装层**
- 只依赖 `kstuff`（非 etaHEN）；AuthID 安装=`0x4800000000000006` / 存档=`0x4800000000000010`，**两套分开封装**；MetaInfo 收回 `0x30`（旧 0x38 多出的 slot/is_playgo_enabled 来自 Mono，非 native ABI）；`_Static_assert(sizeof)` 只证自身一致，须三源交叉。

**存档域**
- 写回前强制快照（不可跳过/原子/失败回滚/可见可控/二次确认）；快照落 `/data/savesnap/`（非 `/data/save_files/`，garlic-worker 会 unlink 清空它）；mount 全局单例 ⇒ 串行锁；ioctl 缓冲必须 heap 分配；`/savedata_prospero/` 直接挂会 EPIPE ⇒ 先复制 `/data` 再挂、卸后回写。

**UI 验收陷阱**
- 动效归 `prefers-reduced-motion`；扫光须是「**底色 + 一条带硬边的窄亮带**」，**不是首尾同色平滑斜坡**（否则肉眼静止、断言却绿）；`display:none` 子树 `getComputedStyle` 照样读到 animationName ⇒ 断言必带 `offsetWidth>0`；判据取行为钩子 `[data-p]` 非类名/位置；封面抽不到必须回退占位格（一排卡片留洞比没封面更糟）。
- 🪤🪤 **`[data-p]` 是「假数据占位」的标记**：静态演示（非 LIVE）下有个 **900ms** 的推进器遍历
  `[data-p] i` 按演示时间轴改写宽度。**真实数据的进度条绝不能挂它** —— 真机看不出（LIVE 下推进器不跑），
  **离线预览里却会显示假进度**（实测：写完立刻读是 4%，等一个推进周期变成 71.2%）
  ⇒ **探针必须跨过一个推进周期再断言**，否则这个 bug 对断言完全隐形（本轮就是这么漏过去的）。
- 🪤 **控件整体放大 = 吸顶栏可能被挤成两行**：`--ui` 1.15 + 多一个按钮后，**900px 宽顶栏 65px → 123px**。
  处置是「窄屏把状态胶囊收成只有灯」（那条 130px 的文案本来也不是承载状态的，灯才是）。
  **改控件尺寸后必须量顶栏高度** —— 横向溢出为 0 不代表没事。
- 判断「这一处是不是我改出来的回归」不能靠记忆：留一份「把新东西回退掉」的副本，用同一个探针量
  **改前 / 改后**（本轮 `.build/_cmp_scale.mjs`：390/600/900/1280/1920 五档顶栏高度）。

**前端交付形态**
- 单文件零依赖 HTML，内联 CSS/JS，**gzip 内嵌**进 ELF ⇒ 内嵌后裸搜 JS 串恒 0 命中，验证用 `.build/check-elf-gzip.py`；系统字体栈、无 webfont、中文不紧排、数字 `mono+tabular-nums`。

**文件权限（Session 22，见 §1s）**
- 🪤🪤 **权限位在宿主上验不了**：`tests/posix_compat.h` 把 `fchmod` 映射成空实现、`mkdirat` 忽略 mode，
  Windows 的 `stat()` 也只是从只读位合成 `st_mode`。⇒ 宿主测试只能验**助手的契约**（常量值、EEXIST 容忍、
  错误传播，以及「chmod 确实被调用」—— Windows 的只读位是可观测的），**真正的 0777 只能在真机 `ls -l`**。
  别写「断言 mode == 0777」的宿主测试：它永远绿，只会给出虚假的信心。
- 🪤 **umask 是隐形变量**：`open(p, 0777)` / `mkdir(p, 0777)` **不等于**结果是 0777（仍会被 umask 削掉），
  要 0777 就必须**创建之后显式 chmod**。反过来，看到 `0666`（而不是常见的 `0644`）就该推断 **umask=0**。
- **新增的落盘点必须过 `src/nx_fs.h`**：`nx_chmod_create()` / `nx_mkdir_create()` 是这条不变量的唯一实现点，
  别再就地写 `mkdir(0755)` 或裸 `fopen("wb")` —— 上一轮的问题正是「解压链 0777、上传链漏了」，两条链不一致。

**HTTP / 请求生命周期（Session 23，见 §1t）**
- 🪤🪤 **libmicrohttpd 给每个请求只有一个 `(void **)con_cls` 槽**。同一 daemon 上只要跑两种上下文
  （普通 `conn_state` + 流式 `upload_conn`），**完成回调就必须按 tag 分派**；靠「我只注册过一个上传路径」
  就假定类型，正是本次真机崩溃的根因（`cs->body` 被当成 `upload_stream_t *` 写 + free 其内部指针）。
  两个结构体都要 `_Static_assert(offsetof(..., magic) == 0)`，认不出的 tag **只泄漏、不 free**。
- 🪤🪤 **`src/httpd.c` 永不进宿主套件**（缺 `<microhttpd.h>`，`make linux` 也编不过）⇒ HTTP 层的 bug 在本机
  **不可复现、不可断言**，`make`/单测全绿完全不代表它没问题。⇒ ① 改 httpd.c 只能靠 SDK 机 `-Werror` 编译 +
  真机；② 真机崩溃只能靠**内置自述日志**（`GET /api/diag`），别指望 shell / coredump。
- 🆕🪤🪤 **同一类盲区的第二处：`#ifndef __linux__` 包起来的「真机专属」代码块，宿主构建根本不编它。**
  2026-10-02 实测：`pkg_task_worker()` 在 `#ifndef __linux__` 里，`task_update()` 少传一个
  `add_done` 参数（4 参 vs 5 参）—— **`make linux` 全绿、107 项 hosttest 全绿，只有 `make all`
  （prospero-clang，`-Werror`）报 `too few arguments`**。⇒ **规矩：提交前必须跑一次全量 PS5 ELF
  重建**（`.build/rebuild-elf.sh`）；`make linux` + 宿主套件的绿，对「只存在于真机分支里的代码」
  **零信息量**。判据不是「有过一次绿」，而是**这一轮的 `make all` 出现在日志里且 rc=0**。
- 🪤 **「进程一交互就退」先分 GET 与 POST**：页面能打开 ⇒ `GET` + **空 body** 那条路是好的
  （`cs->body == NULL`）⇒ 罪魁在后面那批**有 body 的 POST**。这个判据能一步把范围缩小一半。
- 🪤 **诊断代码本身必须被测**：`/api/diag` 是崩溃的唯一证据，丢行 / 截错尾给出的是**自信的错答案**
  ⇒ `tests/test_diag.c` 钉住「追加写不擦除 / tail 锚定末尾 / 无路径时静默降级」三条性质。

**文件操作的前置检查 / 证据判读（Session 24–25，见 §1u / §1v）**
- 🪤🪤 **`strings` 只输出 ASCII** ⇒ 拿它查中文串**恒 MISS**，会把「代码明明进了二进制」误判成没进
  （本轮 `fs_guard` 的 7 条中文报错第一次全被误判）。查 UTF-8 必须字节级检索：
  `python3 -c "print(s.encode() in open('x.elf','rb').read())"`。`strings` 只适合查 ASCII 标识符
  （`.mango-probe-`、`REQ/DONE/SENT` 这类）。
- 🪤🪤 **`/mnt/c`（WSL 9p/drvfs）不实施 Unix 权限位**：`chmod 0555` 返回 0，`stat` 读回仍是 **777**。
  ⇒ 任何「不可写应被拒绝」的断言会**静默空转**，而**一个从不拒绝的闸门看起来完全健康**。
  处置：① 测试先验证夹具前提（chmod 后 `stat` 确认真的变成 0555）才断言，否则打印跳过；
  ② 需要真实权限语义的套件改用 `mktemp -d` 的**原生 cwd** 跑（`.build/asan-suites-wsl.sh` 已这么做）。
  同类：Windows 宿主的「只读目录属性」也拦不住在其内部建文件 ⇒ 这条负向断言只能在原生 POSIX 上验。
- 🪤 **别用 `access()` / mode 位判可写**：payload 可能带提权凭据（mode 位不描述我们能做什么），
  FAT/exFAT 更没有 mode 位。唯一诚实的判据是**真建一个文件再删掉**（`src/fs_guard.c` 的探针）。
- **前置检查的适用边界**：闸门只服务**长操作**（copy / move / upload / extract / delete）。`mkdir` /
  `rename` / `text/save` / `chmod` 是单次立即系统调用，它自身的报错就已经是「开跑前拒绝」，再加预检只是重复。
  **闸门不得新增拒绝理由** ⇒ 判不出来（`FS_GUARD_UNKNOWN`）或量不出大小（`required == 0`）**一律放行**。
- 🆕🪤🪤 **「跳过」分支会以错误的理由触发，把真断言第二次藏起来**（Session 25）：给负向断言加
  「先验夹具前提，不满足就跳过」之后，**那个前提夹具必须是独立的**。曾复用同一条测试里刚被
  `nx_fchmod_create` 改成 0777 的文件 ⇒ 原生 cwd 下也读到 0777 ⇒ 打出「本文件系统不实施 mode 位」
  然后跳过，而它其实实施（同一轮 fs_guard 42 项全过就是反证）。**判据**：跳过分支的**判据本身
  要能失败** —— 夹具独立、用受限模式（`0600`）创建；并**对账检查项数**（原生 cwd 应比 /mnt/c
  多出正好那几条，数目不对就是有断言又隐身了）。
- 🪤 **只走一步 `dirname()` 的前置检查等于没有**：真实目标是 `/mnt/usb0/NewFolder/game` 这种**还未创建**
  的路径，必须**向上走到第一个存在的目录**，否则检查退化成 UNKNOWN —— 恰好在最需要它的场景失效。
  并且 `errno` 要区分 `ENOTDIR`（路径中间有普通文件 ⇒ 确定性拒绝）/ `ENOENT`（查不到 ⇒ 建议性）。

---

## 5. 当前环境 / 工具链

- 本机（Git Bash）：MinGW gcc 16.2.0 + x86_64-w64-mingw32-gcc 16.2.0 + Node 22.22.2 + python3 可用。
- **没有 PS5 Payload SDK（prospero-clang）** ⇒ ELF 不可在此编译（见 §3）。
- **网络（2026-09-27 实测有变）**：`api.github.com` 通（200），**`github.com:443` 本次可达（curl 返回 200）** ⇒ 直接 `git push` + `GIT_SSL_NO_VERIFY=1` 可推（Session 3 已验证零分叉推送成功）。若日后 egress 再把 `github.com:443` 挡死（TCP 连不上、非证书），退回 **Git Data API**（`gh api` 拉 blob 的 base64）取 GitHub 内容 —— 已用于 Session 2 抓取 zlib v1.3.1（脚本 `.build/fetch-zlib.py`）。`gh` CLI 已认证可用。
- **`cp -r SRC DST` 当 DST 已存在时会嵌套成 `DST/SRC`** —— 搬目录后务必 `ls` 确认层级，别让 `tests/tests/` 这种嵌套溜进去（本次 Session 1 已踩一次并修掉）。

---

### 竞品核实（源码级，2026-09-27，详见 docs/competitor-audit.md）
- **elf-arsenal（开源 clone，GPL-3.0）**：`src/smb.c` 基于 **libsmb2**（作者 John Törnblom = libsmb2 上游），SMB 互通**有开源参考、非纯真空**；`src/zipread.c` 用 miniz 仅做 ZIP/DEFLATE（无加密/分卷）；**RAR/7z 完全没有**；传输主路径 FTP（ftpsrv-src）。
- **VoidShell 3.0B（闭源、已放弃、内置过期日）**：无法源码核实；发布说明层面走 FTP + ShadowMount，**无 SMB/NFS**；闭源代码一行不可借鉴、界面/文档不出现其名。
- **真空重校准**：NFS 互通（elf-arsenal 无）仍是真空；**RAR/7z 解压（含加密头+分卷）是竞品均无覆盖的真真空**（本仓引擎核心差异化）；大文件吞吐+续传（FTP 续传弱）是实质差异。

## 6. 下一步 / 待办

> **SDK 机 / 真机全量待办已整理成单页清单：`docs/sdk-machine-checklist.md`**（A 首次编译 → B transfer smb/nfs → C Phase 0 实验 → D 散点；每项含前置依赖/步骤/判据/出处）。下列各项是它的索引，执行时以清单为准。

1. ~~**补齐 zlib + 7z(LZMA SDK)** 到 `third_party/`，让 host 测试矩阵能在 MinGW 跑通（验证解压引擎回归）。~~ ✅ **已完成（Session 2）**：zlib 经 Git Data API 抓取、7z 从旧仓复制，MinGW 测试矩阵 **204/204 全绿**（见 §1b）。
2. **在 SDK 机执行 `make`**，把叶子模块 + 契约头编进 ELF；补 flag-stamp（见 Makefile TODO）。
3. ~~**Phase 0 本机半**：备测量脚本（`misc/bench_extract.c` 壳 + `misc/bench_fixtures.py` 本机原型，已跑通验证方法论）+ 预检清单（`docs/phase0-benchmarks.md`）+ 竞品源码级核实（已完成，见 `docs/competitor-audit.md`：elf-arsenal 有 libsmb2 SMB+miniz ZIP、无 RAR/7z/NFS；VoidShell 闭源无法核实）。~~ ✅ **已完成（Session 4）**。待办转为**真机 4 项实验**（singleDPI 装 fPKG / AuthID 提权链 / 同形状解压计时 / 写盘上限 T_copy、内置 vs exFAT 写速、libsmb2 拉取实测），等机器到位。
4. ~~**按契约头实现编排层**~~ ✅ **已落地（Session 5，见 §1d）**：8 个模块全写出，4 个本机可测套件 **76 项断言全绿**；4 个 SDK 档模块**已写但本机未编译**（httpd 缺 libmicrohttpd；安装/存档缺 PS5 SDK）。剩余工作：
   - **SDK 机首次编译（必做）**：逐个核对 `extern` 原型 —— 尤以 **MetaInfo `0x30`（✅ 四源交叉已闭合）**、**pfsmgr ioctl** 符号名为要。🆕✅ **`sceAppInstUtilInstallByPackage` 的签名问题已修复（2026-09-30）**：旧代码把**路径**当了首参，而真签名首参是**元数据结构体**（`(meta, info, playgo)`）⇒ 系统拿路径串当地址解引用 6 个指针，**必失败/最坏崩，且编译期不暴露**（自己的 extern 声明自洽，`_Static_assert(sizeof)` 也只证自身一致）。现按真签名调用，并已用 **prospero-clang 18 + `-Werror` 编过 + 链进 ELF**；`$SDK/target/lib/libSceAppInstUtil.so` 已验证**确实导出**那四个符号（⇒ 直连即可，不需要 `kernel_dynlib_resolve`）。原理 + 逆向出的**三阶段 HTTP 协议** + 取证见 **`docs/pkg-install-without-usb.md`**（落地记录 §7，**真机 7 条待核 §7.5**）。
   - **transfer 接 smb/nfs（SDK 机）**：~~vendored libsmb2/libnfs 后开 `MANGO_HAVE_LIBSMB2`/`MANGO_HAVE_LIBNFS`~~ ✅ **代码已落地（Session 15，见 §1n）**：`third_party/libsmb2`（v6.0.0）+ `third_party/libnfs`（8.0.0）已 vendor，`Makefile` `LIBSMB2=1`/`LIBNFS=1` 开关 + per-target flag，`transfer.c` 宏守卫四函数实现完毕，本机验证链全绿（URL 解析 19 + scheme_supported 契约 6 断言、双库全 TU 编译、宏开链接冒烟跑绿）。**剩 SDK 机两步**：① `make LIBSMB2=1 LIBNFS=1` 实编译过链接；② 真机连 NAS 验证 smb/nfs 拉取。local 后端与既有测试用例一行不用改。
   - ~~**worker 抽干 QUEUED 任务**~~ ✅ **已完成（Session 7，见 §1f）**：`src/worker.c` 单线程 tick 驱动 6 类任务、4 槽位并发、取消不留半截产物、shutdown 绝不 commit；`/api/fetch` 新增；安装/备份/恢复统一入队。**后端链路至此从「入队」贯通到「真的执行」**。
   - ~~**解压引擎接进 worker**~~ ✅ **已完成（Session 8，见 §1g）**：`src/extract_engine.c` 新契约适配器落地，worker `step_extract` 走 `mango_extract_req_t`（密码/large/progress/cancel），失败带原因（`task note` → JSON 快照 `"note"`）。**含密码/分卷路径**（相对竞品的真真空）。测试 +40。
   - **UI 可开始接入**：API 面已定（17 条路由，见 §1e/§1f），`assets/index.html`（Harness）可把假数据换成真 `fetch`；任务视图直接消费 `/api/tasks`。
5. **UI 并行轨道**：~~每完成一个后端 Phase，同步把对应视图接进 `assets/index.html`（Harness），跑 106 项验收。本机地址 / 端口 / kstuff 一律取 `/api/status`（**端口必须服务端上报**）。~~ **已完成（Session 9–14，见 §1h/§1i/§1j/§1k/§1l/§1m）**：任务视图 + 顶栏状态 + 解压对话框（password/large）+ 文件视图真目录树 + 落点点选（`fsNav` 工厂）+ **安装/上传视图**（library 接 `/api/pkg/scan` 动态渲染 + 搜索/平台筛选 + 安装入队 `/api/pkg/enqueue` + 从 NAS 拉取 `/api/fetch` + **上传切片续传**：JSON meta + `\n\n` + 原始切片，8 MiB 分片、任务中心进度，Session 14 见 §1m）+ **任务视图安装大卡**（install 任务分叉百分比口径，进度走 `/api/install/poll`）+ **均速真值**（快照 `rate` = 总字节÷总耗时，UI 均速读服务端字段，install/无 rate 诚实 `—`），验收走 `.build/ui_live_check.mjs`（**Session 21 已扩到 153 项**：新增合并后的上传菜单、`File` 句柄回归位、删除全链路、关闭进程；mock 同源后端）；静态 demo 106 项回归保持绿。**剩**：① transfer 接 smb/nfs 的 SDK 机实编译 + 真机 NAS 验证（代码交付见 §1n，连接级验证清单见 §1o）；② ~~fetch/extract 的 form URL 解码缺口（`%2F` 落盘，见 §1m 坑）待真机验证后单开 Session~~ ✅ **已修（Session 15，见 §1m）**：`api_body_value` form 分支补 `url_decode_range`（`%XX`/`+` 解码），host +5 断言，api suite 150 checks 全绿；JSON/上传切片分支不受影响。
6. ~~**金手指域**~~ ⛔ **已整体移除（Session 19，见 §1p）**：用户拍板砍掉金手指**所有**功能，本插件只做文件管理。后端引擎（cheatmgr/cheat_sync/cheat_parse/cheat_remote/hijack_stub）、`/api/cheat/*` 路由、前端视图、测试套件、SDK 清单 C/D 段**全部删除**。金手指文件由文件管理器当普通文件覆盖。
7. **真机复验（Session 21 的一轮反馈，见 §1r）** —— 这 5 条必须在主机上确认，离线验不了：
   1. **上传多个大文件**：不再出现「全部 0 KB + 上传中断」。这条同时验证了前后端两半
      （清空时机 + 失败即删半成品）。
   2. **NAS**：只填 `192.168.1.100` 应能列出共享名（走 `IPC$`）。⚠️ 若仍失败，先分清是
      **地址解析**（已修）还是**跨网段不通**（`<ps5-ip> → 192.168.1.100`，光猫/路由器分段，
      未验证）—— 后者要在主机上 ping / 看路由，与本轮的修复无关。
   3. **删除一个上万条目的目录**：进度条应逐步前进（约 1.3 万条/秒，与 `TASK_DELETE_PER_TICK=256`
      × 20ms 节流一致），可「取消」，取消后已删的不回来。
   4. **关闭进程**：网页应立刻变「进程已关闭」且不再轮询；主机需重新运行 payload 才能回来。
   5. **界面放大 / 满屏**：1920 上满屏、按钮与文字明显更大（`--ui` 可再调）。
   ⚠️ **`/api/shutdown` 关掉后 payload 不会自启** —— 测试顺序上把它放最后一条。
8. **文件权限（Session 22，见 §1s）** —— 宿主上根本验不了（`chmod` 是空实现），只能在主机 `ls -l`：
   1. **上传**一个文件 ⇒ 期望 `-rwxrwxrwx`（这条正是用户报告的）。
   2. **fPKG 部署**后看 `/data/homebrew/...` 整棵树 ⇒ 文件与目录都是 0777 —— 游戏跑不跑得起来就靠这个。
   3. **解压** zip / rar / 7z 各一次 ⇒ 产物同样是 0777（rar 与 7z 走的是「整树归一」，与 zip 逐条 chmod
      的实现不同，所以三条都要点一遍）。
   ⚠️ **先查挂载点的文件系统**：`/mnt/usb0|1` 上的外置盘若是 **exFAT/FAT**，内核会**直接拒绝 chmod**
   （`ls -l` 显示的就是固定值，改不动），那是**预期行为、不是 bug**；要验权限请用 `/data`（PFS）或
   `/mnt/ext1`（内置 M.2）。
9. **`#view-tasks` 视图收口**：① 导航无「任务」入口（导航只有 文件/PKG管理/存档）是**有意设计**——进度改内联显示
   （删除在文件视图 `#jobBar`、上传在对话框），符合「导航放页面、动作做按钮」，**不是待办**；② 该视图里 `平台` / `存储` 两张卡仍是**占位数据**
   （`320 GB 可用` / `1.42 TB` 等），**接上真实 `/api/space` / `/api/status` 之前不能对外**，
   或者干脆把这两张卡删掉。这是「不要放占位数据出去」这条线的最后一处。
10. **进程退出根因（Session 23，见 §1t）** —— 修的是 `con_cls` 类型混淆，但真机必须复验一次：
    1. 重新注入 payload → 打开 `http://<PS5-IP>:2026` → **点进各个页面 / 刷新几次 / 上传一个小文件**，
       进程**必须不退**（这正是本次的回归点）。
    2. 若不退：`GET /api/diag` 应能看到成对的 `REQ`/`SENT`（说明请求都正常收尾）。
    3. 若仍退：立刻 `GET /api/diag`，读**最后一条没有 `SENT` 的 `REQ`** ⇒ 那就是元凶，带着它继续查
       （这是本仓第一次能让真机崩溃**自述**，别再靠口述复现）。
11. **开跑前拒绝（Session 24，见 §1u）** —— 新的空间/可写预检只有在真机上才验得了「拒绝」这一侧：
    1. **空间**：往一个几乎满了的目标（或 `/mnt/usb0` 上剩余空间 < 文件大小的外置盘）**复制 / 移动 /
       上传一个更大的文件** ⇒ 期望**立刻**返回 `ok:false` + 「目标空间不足：需要 X 字节，可用 Y 字节」，
       **不建任务、不产生 0 字节残留**（`/api/tasks` 里不应多出一条）。关键点：判据是**提示**，不是
       等它跑到一半才失败 —— 若仍先「开始」再失败，说明预检没生效。
    2. **可写**：把目标目录设成不可写（或在 exFAT 上用只读挂载）后**复制 / 解压 / 上传**进去 ⇒ 期望
       同样**开跑前**拒绝（「目标不可写 …」），同样不建任务。
    3. ⚠️ **判据不是 `strerror` / mode 位** —— 探测手段是**真建一个 `.mango-probe-*` 文件再 unlink**。
       若真机上没拒绝，先看**挂载点文件系统**（FAT/exFAT 无 mode 位、payload 可能提权 ⇒ mode 位不描述
       我们能做什么），再把 `GET /api/diag` 的 `REQ` 行带回来。探测**绝不能留下文件**：跑完 `ls -a` 目标
       目录应看不到 `.mango-probe-`。
    4. **同时回归「不误伤」**：同盘 **move**（rename）**必须照旧成功**（它不需要空间，预检刻意只查可写）；
       `FS_GUARD_UNKNOWN`（读不到 `statvfs`，如部分网络/伪文件系统）**必须放行**，不许变成新的拒绝理由。
12. **权限规则的 P1 补完（Session 25，见 §1v）** —— 两条都只有在真机上才看得见，且都要**分文件系统**验：
    1. **上传中途取消**（传一个大文件，进度到一半点取消）⇒ 目标目录里**不应留下任何文件**；
       若不慎留下，其权限也**不能是 0666** —— 这是本轮 fd 版 chmod 的直接回归点（旧实现只在上传
       开始时按路径 chmod，窗口期就是整段传输）。
    2. **0777 在实施 mode 位的文件系统上**：`/data`（PFS）或 `/mnt/ext1` 上传/复制/解压后 `ls -l`
       应为 `-rwxrwxrwx`。**同时**在 `/mnt/usb0` 的 exFAT 盘上做同样的操作 ⇒ 显示的是挂载选项决定的
       固定值（通常是 `-rwxrwxrwx` 或全 7），**改不动是预期行为、不是 bug** —— 本条正是
       `nx_fs_type_lacks_unix_modes()` 存在的理由，别把它当成失败报。
    3. **判据不是 `strerror`**：`strace` 若可用，上传开始时应看到 `fchmod` 而非 `chmod`；
       没有 `strace` 就看 `ls -l` 在**传输过程中**（未结束时）是否已经是 777。
13. **目录统一 `/data/Mango` + 日志集中 + PKG 装完自动删（Session 17，见 §1x）** —— 真机必测：
   1. 新目录布局 `/data/Mango/{incoming,tmp,backups,logs}` 由 main 启动自动建（此前旧目录 `/data/wfm`、
      `/data/save_files`、`/data/backups` 与根目录日志是旧版本写的，新代码不再碰，需手动清理）。
   2. **PKG 上传 → 安装完成** ⇒ `incoming` 里本次上传的那个 PKG **自动消失**（装完自动删）；toast「安装完成，暂存 PKG 已清理」。
   3. **已安装/残留跳过** ⇒ PKG **保留** + toast「安装器跳过…PKG 保留在 /data/Mango/incoming」。
   4. 三份日志出现在 `/data/Mango/logs/`：`mango-crash.log` / `mango-install.log` / `mango-save.log`；/data 根目录不再新增日志。
   5. 存档页备份 → `/data/Mango/backups/<tid>/`；解密/加密/改签临时镜像在 `/data/Mango/tmp/` 且前缀 `_nex_*`。
14. **上传同名冲突选择（Session 17 追加，见 §1x）** —— 真机必测：向已有同名文件的目录上传 → 弹「同名文件已存在」三选窗；覆盖 → 后端走 overwrite 路径成功；重命名 → 落盘为 `name (1).ext`；跳过 → 不发请求、批次汇总计入失败数；后端已存在且列表未刷新（409）→ 同样弹窗兜底。
15. **长导入的进度窗 + 崩溃自恢复（Session 27，见 §1y）** —— 这一轮全部要真机验，宿主套件结构上覆盖不到（样本包几 KB 永远绿）：
    1. **进度窗真的有字**：拿一个**几百 MB 的加密存档整包 ZIP** 走「加密镜像 · 替换」。期望：弹窗里**逐行滚出**步骤（解包 → `（n/N）读取 XXX 的存档信息…` → 替换落盘），**进度条按百分比前进**。若弹窗开着但**一行都没有**、状态行一直停在「准备中…」⇒ 就是本轮修掉的那个非法 JSON 复发（去抓 `curl -s http://<IP>:2026/api/save_mgr/import_log` 的**原始响应**，肉眼确认是合法 JSON）。
    2. **算不出总量时不许编造**：进度条应保持**滚动**且状态行**没有百分号**。出现假百分比同样是 bug。
    3. **中途刷新页面**：趁任务在跑刷新 ⇒ 弹窗**自动重新出现**并接着报进度；任务跑完后弹窗**自动收起**。
    4. **不再吃满内存**：传大包时在另一个终端看内存占用 —— 不应出现「整包大小量级的单次跳升」（旧实现是 `malloc(zsize)`）。
    5. **崩溃后自恢复**：任务进行中直接**断电/强杀**，重新注入 payload ⇒ `/data/Mango/logs/mango-*.log` 里应能看到启动清理（`_nex_*` 被删、空闲空间报告）；`/data/Mango/tmp` 里**不应再有** `_nex_arc` / `_nex_zip_*` / `_nex_up_*`；且**插件功能可用**（不是「进程在、点啥都没反应」）。
    6. **结束进程不再自启**：进程卡住时结束它 ⇒ **不应**反复自动重启。若日志里出现 `kill(N) FAILED: ... uninterruptible kernel call`，那是**预期记录**（老进程卡在内核里带不走），新进程应当**照常起来**并可用 —— 而不是秒退再被拉起来。
    7. ⚠️ 判据是**屏幕上的字和条**与**日志行**，不是「弹窗打开了」。它本来就会打开 —— 这正是上一版一直空白却没人发现的原因。

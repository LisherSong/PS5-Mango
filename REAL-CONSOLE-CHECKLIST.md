# PS5 Nexus — 真机复测清单

**产物**：`PS5-Mango-v1.0.0.elf`（ELF64 / x86-64 PIE，PS5 payload）
**版本号可见性**：payload manager / etaHEN 的 *plugins → payload ELFs* 菜单**按文件名列出**，
所以版本号是**编进文件名**的 —— 列表里会直接看到 `PS5-Mango-v1.0.0`。
⚠️ `src/main.c` 里的 `PROCESS_NAME` 故意**不带版本**：它是「杀掉旧实例」的匹配键，
带版本会导致升级时匹配不到旧进程，两个实例并存、新实例悄悄退到 2027 端口。
**构成**：owendswang/ps5-web-file-manager 最新版后端 `src/` + Mango 的 `assets/` 前端 + 新写的 `src/mango_port.c`。
**本机验证**：`hosttest.py`（**80 项，全绿，连跑两次结果一致**）+ `.build/preview_check.mjs`（前端 57 项）。

---

## 0. 本轮（v1.0.0 改名）改了什么

| # | 改动 | 为什么 |
|---|------|--------|
| 1 | `BIN := PS5-Mango-$(VERSION_TAG).elf`；`VERSION_TAG := v1.0.0` | payload manager 只显示**文件名**，版本不写进文件名就等于没版本。附带好处：版本进了 target 名，`make` 不会再因为「CFLAGS 变了但没有任何依赖变化」而复用旧产物 |
| 2 | `PROCESS_NAME` → `"PS5-Mango.elf"`（不带版本），并把旧实例清理改成遍历 `{PS5-Mango.elf, PS5-Mango.elf, PS5-Mango.elf}` | 旧名在真机上还跑着，不清掉就会两个实例抢 2026 端口 → 表现为「升级没生效」 |
| 3 | `TITLE_ID` 修正为 `MANG88888` | 原来 Makefile 写 `FMGR88888`，而 `assets/param.json` 里写的是 `MANG88888` —— `app_installer.c` 按 TITLE_ID 建 `/user/app/<id>`，两边不一致会装到启动器不认的目录 |
| 4 | `/api/status` 增加 `version` 字段 | payload manager 只能显示「你部署的那个文件名」，而 `/api/status` 返回的是**正在跑的那个二进制**编译进去的版本 —— 这才是「升级到底生效没有」的答案 |
| 5 | 补 `LICENSE`（GPL-3.0）、`THIRD_PARTY_NOTICES`；README 重写为 PS5 Nexus 的真实说明 | 原 README 还是上游「Web File Manager / 8888 端口」的原文，与现状（2026 端口、NAS/PKG/存档）不符 |

---

## 1. 上一轮修掉的两个真问题

| # | 症状 | 根因 | 修法 |
|---|------|------|------|
| 1 | 列表一页显示不下时，往下滑上面的功能按钮就没了 | 面包屑 + `.fmbar` 就是普通文档流元素，没有任何东西钉住它们 | 包进 `.fmstick`（`position:sticky`）；`top` 由 JS 实测顶栏高度写进 `--topH`（顶栏 1920 下 70px、900 下换行成 123px，**不是常数**），配 `ResizeObserver` |
| 2 | NAS 设置成 `192.168.1.3\PS5_Games` 后报 `Invalid address: ... Can not resolve into IPv4/v6` | 主机名扫描只认 `/` 和 `:`，整串 `192.168.1.3\PS5_Games` 被当成主机名丢给 `getaddrinfo` | 四层都按「`\` 等同 `/`」修：`transfer.c`（`path_sep()`）、`list.c`（`nas_build_url`）、`mango_port.c`（`/api/fetch` 取 basename）、前端 `normalizeNasAddr()`。**口令里的 `\` 故意不转换**（转了就悄悄改了密码）。前端**读取时也归一**，因为已经踩坑的机器 localStorage 里存的就是坏地址 |

### 更早一轮：接缝层的 5 个 bug

| # | 症状 | 根因 | 修法 |
|---|------|------|------|
| 1 | 几乎所有前端操作都报「路径无效 / 读取失败 / Success」 | `body_json_value()` 解析完第一对 key 后**不跳逗号** → 多键 JSON 只认第一个键 | `path_util.c`：每对后面 `if(*p==',') p++` |
| 2 | 中文/日文文件名乱码或整条路径对不上 | `json_escape()` 逐字节转义 | `json_util.c`：先按 UTF-8 解码成码点再 `\uXXXX`（含代理对）；非法序列逐字节兜底 |
| 3 | `mkdir`/`rename` 失败报 `{"error":"Success"}` | `errno` 在传进 `strerror()` 前被重置 | 失败后**立刻** `int err=errno;` |
| 4 | `text/*`、`fs/read` 只认 JSON body | 用了 `body_json_value`（只 JSON） | 改用共享的 `req_param()`：JSON → form → query |
| 5 | `/api/pkg/info`、`/api/pkg/icon` 恒失败 | 前端是 POST + form，后端只放行 GET 且只读 query | 放行 GET/POST，改走 `req_param` |

## 2. 已搬过来的能力

- **存档域** `/api/save/list|backup|restore`：强制快照 → 原子落盘（temp+rename）→ 失败回滚；快照目录 `/data/savesnap/<TITLE_ID>/<时间戳>/`（**不放 `/data/Nexus/tmp/`**，那里是存档域的临时工作目录，装完/下载完即清理）。
  ⚠️ 只实现了**文件级**快照/回滚（安全关键的那半）。PFS 挂载 / 解密 / 重签是 PS5 内核 + 存档 AuthID(`…0010`) 的活，尚未接线。
- **PKG 库** `/api/pkg/scan|enqueue` + `/api/install/poll`：递归扫 `.pkg`（读 PARAM.SFO / param.json 取 TITLE_ID/标题/版本）与 `.ffpkg/.ffpfsc/.exfat`（按文件名取标题）；`.pkg` 进内核安装队列，fPKG 镜像**复制进 `/data/homebrew`**（ShadowMount 挂载，不碰内核）。
- **从 PC 安装** `/api/pkg/install-url`：PC 上 `python tools/serve-pkg.py [端口]`（默认 9898，需与 PS5 同网段、防火墙放行），PS5 网页「PKG管理 → 从 PC 安装」填 `PC IP:端口` → 卡片列表 → 安装。**系统安装器自己从 PC 下载（Range 206），PKG 不落 PS5 硬盘**。真机验证：① catalog 能列出卡片；② 点安装后系统下载列表出现该游戏；③ PC 脚本窗口能看到 PS5 的 Range 请求；④ 安装中断后重试不残留半成品（下载是系统安装器内部状态）。
- **NAS**（SMB2/3 + NFSv3/4，vendored libsmb2 / libnfs）：浏览 + `/api/fetch` 拉取（**默认续传**：`stat` 已有目标 → `lseek` + 不 TRUNC）。
  ⚠️ 顺带修了 vendored libsmb2 的一个 **opendir 错误路径 use-after-free**（`free_smb2dir()` 无条件 free 调用者的 `cb_data`）—— 真机上表现为「进程直接没了」，已按上游方案移植 `dir->free_cb_data` + `smb2_dir_take_cb_data()`。
- **`/api/status`** 上报真实本机 IP（UDP connect 取）+ **当前运行版本**；前端顶栏「本机地址」不再是播种值。
- **`/api/app/register`**：真机上走 `sceAppInstUtil*` 注册主屏图标（前端已内置「确认没有游戏在运行」的二次确认）。
- **`/api/diag`**：请求账本（最近 64 条 `REQ`/`DONE`），真机排查「点一下就退」用。
- **删除已任务化**，进度**就地显示在文件视图**（`#view-tasks` 没有导航入口，别指望用户去任务中心看）。

---

## 3. 真机复测清单（按顺序，一条不合格就停）

> 部署：把 `PS5-Mango-v1.0.0.elf` 放到 PS5 上能跑 payload 的位置，起来后浏览器开 `http://<PS5 IP>:2026/`。

| # | 操作 | 期望 |
|---|------|------|
| 1 | 在 payload manager 里看列表 | 显示 **`PS5-Mango-v1.0.0`**（版本号就在文件名里）；启动通知也写明版本与端口 |
| 2 | 打开 `http://<IP>:2026/` | 页面正常；顶栏 **HTTP 灯亮**，端口 2026（被占用会顺延，前端自动跟随） |
| 3 | `http://<IP>:2026/api/status` | `version` 字段是 **v1.0.0** —— 与文件名一致才说明跑的是新构建 |
| 4 | 看顶栏「本机地址」胶囊 | 显示**真实内网 IP:端口**；点一下能复制 |
| 5 | `/data` 列目录，**条目多到一屏放不下**，往下滑 | 面包屑 + 工具栏**钉在顶栏下面不动**；滚动中按钮可点 |
| 6 | 中文/日文名 | 不乱码 |
| 7 | 点开一个较大的 `.log` / `.txt` | 弹出预览，有内容；**进程不退出** |
| 8 | 选中文件 → **删除** | 先弹确认；确认后入队，**文件视图内联显示进度**；完成后文件消失，**进程不退出** |
| 9 | 上传一个文件（按钮或拖拽） | 进度条走完 → 「上传完成」；落点字节数与源一致（**不再出现 `0/4294967296`**）；上传中若中断，落点**不应留下半截文件**（写的是 `.nex-upload-*.tmp`，完成才 rename） |
| 10 | 上传一个**文件夹** | 子目录树在落点重建 |
| 11 | 新建文件夹 / 改名 / 复制 / 移动（粘贴） | 各自成功；任务出现在任务中心，完成后列表刷新 |
| 12 | NAS：地址栏填 `192.168.1.x\share`（**反斜杠**） | **能连上**（这是本轮修的）；再填一次正斜杠写法作对照 |
| 13 | NAS：从共享里拉一个文件到 `/data` | 有进度；中断后重拉**从断点续传**（不从头开始） |
| 14 | 右键/点击 `.pkg`（游戏库扫描也走一遍） | PKG 库出现卡片（标题/版本或 fPKG 镜像标签）；点「安装」→ 进度弹窗；fPKG 镜像会复制进 `/data/homebrew` |
| 15 | 存档页：填 TITLE_ID → 列表 → 备份 → 回滚 | 备份入队完成后出现新快照；回滚后存档回到快照内容 |
| 16 | 「主屏图标」按钮（**确保没有游戏在运行**） | 主屏出现/已存在本应用图标 |
| 17 | 再部署一次同一 ELF（升级/重载） | 旧实例被自动清掉，**仍然是 2026 端口**（不是 2027）—— 说明进程名复用链没断 |
| 18 | 最后一条再测：顶栏「关闭进程」 | 二次确认 → 收尾屏 → 进程退出（**不可逆**，PS5 没有 service manager 拉起） |
| 19 | 存档页「加密镜像 · 替换」传一个**几百 MB 的整包 ZIP** | 弹窗里**逐行滚出**步骤（解包 → `（n/N）读取 XXX 的存档信息…` → 替换落盘），进度条**按百分比前进**；**不是**空窗口 + 「准备中…」。中途刷新页面 ⇒ 弹窗自动重挂；跑完自动收起。详见 `docs/HANDOVER-nexus.md` §1y / 真机必测 §15 |
| 20 | 任务跑到一半**强行断电**，重新注入 payload | 日志里有启动清理记录；`/data/Nexus/tmp` 不再有 `_nex_arc`/`_nex_zip_*`/`_nex_up_*`；**功能可用**（不是「进程在、点啥都没反应」）；结束进程**不再反复自启** |
| 21 | 三处界面细节（v1.0.10）：顶栏 / 存档页按钮 / 选择文件按钮 | ① 顶栏胶囊是 `● HTTP 服务`（**没有** `:2026`），端口只在左边那个可点的 IP 胶囊里出现一次（窄窗口 <1240px 下文字收起只剩绿灯 —— 是既有行为，不是 bug）；② 存档页六个标签挨个看：同屏按钮**一般高**（含浏览的「刷新列表/卸载」、文件头的「上级/上传替换」、各标签的「选择…」与主操作按钮），**主操作按钮不再比「选择…」高出一头**；③「选择文件」是**有底色的胶囊**（跟旁边输入框同款），一眼看得出可点、不是一块空白，光标移上去底色略亮。详见 `docs/HANDOVER-nexus.md` §1z |
| 22 | 文件管理 → 点开工具栏「上传」下拉菜单（v1.0.11） | 两条目「上传文件 / 上传文件夹」的**字号与旁边的「新建文本 / 新建目录」一样大**（改前菜单明显更大一号）；每条的**图标与文字在一条水平中线上**（改前图标偏高、箭头和文件夹都悬在字的上半截）；两行比改前**紧凑**（行高 ≈46px，改前 63px，空档明显）。详见 `docs/HANDOVER-nexus.md` §1aa |
| 23 | 四件事（v1.0.12）：PKG 信息窗 / 卸载按钮 / 临时文件自动清理 / 导入弹窗终态 | ① **PKG 信息窗加宽**：进 PKG 管理页点一张卡片的空白处（不是「安装」按钮）→ 弹窗明显比其它弹窗**宽一截**（1040px，其它仍是 660px）；「规格」页的 **TITLE ID / CONTENT ID / 内容版本** 三行都在，长 CONTENT ID **折行**而不是把值列顶成一列字。② **卸载按钮**：`已安装` 的卡片脚上现在是可点的 **「卸载」**（不再是灰的「已安装」）→ 点击弹确认（写清连带删存档、不可撤销）→ 确认后提示「已提交卸载…请回主机桌面确认」并重扫，**卡片变回「待安装」**；⚠️ 卸载是**异步**的，以主机桌面为准。**取消确认则什么都不该发生**。③ **临时文件**：存档页「加密镜像 · 替换」传一个整包 ZIP → 解析成功时**源 ZIP 仍在**（取消/失败后靠它重扫，不用重传）；全部替换成功后再看文件页 `/data/Nexus/incoming`，**该源包已自动消失**；单张镜像 / 加密导出 / 改签导出同样在成功后自动清理。④ **导入弹窗终态**：上传加密存档 → 提示同名 → 点「是」之后，弹窗里是**本次**的日志（不是上一轮那段「解包中：整包 60 MB，正在摊开…」），跑完出现 **✓ 已完成** 且**进度条停住不再闪**，约 2 秒后自动收窗；**失败**时窗**留着**、状态行红字「✗ 失败（原因见下方日志）」、进度条冻在满格。详见 `docs/HANDOVER-nexus.md` §1ab |
| 24 | 两处越界读/写（v1.0.12）：存档库里有坏 `param.sfo` / 存档条目多到列表 JSON 超 16 KB | ① **坏 SFO**：往存档库里某个 TITLE_ID 目录手工放一个**被截断**或**头部偏移被改坏**的 `param.sfo`（`key_off`/`data_off`/`n_entries` 越界），然后**打开存档页**（或点「刷新列表」）：列表**正常返回**（坏条目被丢弃，不是整页报错）、**进程活着**（`/api/status` 仍有响应）、`nexus-crash.log` **不新增** `=== SIGSEGV ===` 段。改前这种文件会让进程崩溃 —— 2026-10-11 三次崩溃的**最后一条请求都是** `POST /api/save_mgr/list`。② **大列表**：存档条目多到 `/api/save_mgr/list` 的 JSON **超过 16 KB** → 响应被**截断**（`ok` 仍在、数组可能不完整），但**进程不崩**、页面能渲染已返回的部分。改前 `pos += snprintf` 会在 `size_t` 域下溢 ⇒ 越界写栈 ⇒ 表现为"处理任何请求时突然 SIGSEGV"。③ 删掉那个坏 `param.sfo`，列表恢复正常条目。详见 `docs/HANDOVER-nexus.md` §1ac |
| 25 | 崩溃留证（v1.0.13）：栈溢出也能记台账 / 版本行 / 设备分支编译 | ① **台账首行**：注入后 `nexus-crash.log` 新增段的第一行是 `[log …] PS5-Mango v1.0.13 starting (built <日期> <时间>)` —— 据此判断这段记录属于哪颗 ELF（v1.0.12 及以前**没有这行**）。② **崩溃段带地址**：`=== PS5-Mango.elf SIGSEGV addr=0x… code=… ===`；`code=2`(SEGV_ACCERR) 且 `addr` 落在栈边界附近 ⇒ 偏**栈溢出**（v1.0.12 及以前这种情况**台账完全空白**，因为处理器在已被压穿的栈上起不来）。③ **打开存档页 / 刷新列表 / 走一遍导入弹窗**各来一遍，进程不崩、`/api/status` 仍有响应。详见 `docs/HANDOVER-nexus.md` §1ad |
| 26 | 存档替换按 Garlic 挂载会话重构（v1.0.14）：整包 ZIP 导入中途，**页面必须一直活着** | 这是 2026-10-11「进程还在 / 杀了还自启 / **网页全打不开**」的正解，重点看**卡住时的表现**而不是只看最后成不成。① **扫描阶段不再挂载**：传一个整包 ZIP → 解析期间清单里标题号显示为 `PPSA#####/CUSA#####`（来自包内目录名），汇总行写明「（标题号取自包内目录名）」——**不再出现**「未读到 param.sfo」。② **替换期间页面保持可用**（最关键）：点「替换 / 导入到本机」之后，**立刻**切到别的标签再切回来、或点「刷新列表」：应看到灰字 **「后台正在导入存档，稍后自动刷新」**并在 4 秒后自己重试，而**不是**长时间转圈/卡住；此时点「卸载」或「放弃本次导入」应弹 **「后台正在导入存档，等它结束再卸载」**（不是假装成功）。改前这几条会全部排队不动，把浏览器那 6 条并发连接占死 ⇒ 连 `/api/status` 都发不出去 ≡「网页全打不开」。③ **内核真卡住时**（少见但要知道长什么样）：弹窗出现「**导入被中断：内核挂载卡住了。已成功 N 份，M 份未处理。请重启主机后再来一次 —— 源包和暂存都留着，不用重新上传**」；去文件页看，那个源 ZIP **确实还在**。此后 `/api/status` 仍应有响应（进程没被拖死）。④ 全程 `nexus-crash.log` **不新增** `=== SIGSEGV ===` 段；注入后首行是 `[log …] PS5-Mango v1.0.14 starting (built …)`。详见 `docs/HANDOVER-nexus.md` §1ae |

### 出问题时的定位手段
```
http://<IP>:2026/api/diag            # 最近 64 条请求
/data/Nexus/logs/nexus-crash.log     # 进程台账 + 崩溃记录（O_APPEND，重启后旧记录仍在）
```
- `/api/diag` 列最近 64 条请求：`{seq, method, url, done, toe}`，**按 `seq` 升序**。
  ⚠️ **不要只看 `done:false`**：崩溃时进程来不及跑完完成回调，会有一堆条目**停在 `done:false`**，
  单看它分不出谁是真凶。判据是 **`seq` 最大（最后一条）的那个 `url`** —— 崩溃就发生在它身上。
  （2026-10-11 真机三次崩溃的最后一条**都是** `POST /api/save_mgr/list`。）
- `nexus-crash.log` 里出现 `=== PS5-Mango.elf SIGSEGV ===` 就是**用户态崩溃**（不是内核卡死）。
  它下面紧跟的 `last requests` 表就是上面那张表 —— **重启后旧记录仍在，可事后取**。
- 同一行链里 `boot reconcile: stale /data/mount_sd umount -> r=<n> rc=<n>`：
  **`r=` 是挂载看护线程的返回值**，`r=0` 表示它**正常返回**（不是卡在内核里）。
- 把**最后一条请求**连同 `/api/tasks` 输出一起发我，就能定位到具体路由。
- **先看版本行**（v1.0.13 起）：每次启动在 `nexus-crash.log` 写一行
  `[log …] PS5-Mango vX.Y.Z starting (built <日期> <时间>)` —— 用它确认「这段记录属于哪颗 ELF」。
  别再被历史上遗留的 `[start …]` 旧前缀误导：**源码里根本没有 `[start` 这个前缀**，
  那是更早的构建留下的（日志 `O_APPEND` 跨版本累积）。
- **崩溃段现在带 `addr` / `code`**：`=== PS5-Mango.elf SIGSEGV addr=0x… code=… ===`。
  `code=1`(`SEGV_MAPERR`) 偏「解引用坏指针」；`code=2`(`SEGV_ACCERR`) 偏「栈溢出 / 写只读页」，
  `addr` 落在栈边界附近基本就是栈溢出。**v1.0.12 及以前栈溢出会完全不留记录**
  （处理器用被压穿的栈执行，进不来）。

---

## 4. 已知空缺（不要当成 bug）

- **真 PS5 `.pkg` 内核安装**需要 `kstuff` 在跑；没装时 `/api/pkg/enqueue` 会报 unsupported（文件浏览/上传/解压不受影响）。
- **PFS 存档真正写回**（解密/重签）未接：`save/backup` 做的是文件级快照，真机上若存档目录本身不可直接写，需要在挂载层补。
- **解压已改为进程内引擎**（自研 `extract_engine`，下挂 UnRAR / LZMA SDK / minizip-ng，全部 vendored 进 `third_party/`）：不再依赖外部 helper ELF，`wfm-7zip-helper.elf` 相关报错（`archive_helper_elf_missing` 等）已整体移除。加密包由 UI 弹口令；分卷从任意卷入队都会归一到第一卷。
- ⚠️ **「从 PC 安装」页的两个下载按钮当前是坏的**（`#ppDlExe` / `#ppDlPy`，`assets/index.html` 约 1743 行）：它们指向
  `https://github.com/LisherSong/ps5-nexus/releases/latest/download/serve-pkg.{exe,py}`，而该仓库**一个 Release 都没有**（`releases/latest` 回 404），
  且 **2026-10-11 起仓库已转私有**（外部用户即使有 Release 也下载不到）。**这在本轮之前就是坏的**，不是新引入。
  ⇒ 正式发布时必须一起做：① 仓库改回 public；② 建 Release 并上传 `serve-pkg.exe`（~9.2 MB，`.gitignore` 明确它只走 Release、不进仓库）与 `serve-pkg.py`。
  在那之前，真机上这两个按钮点了就是 404 —— 别当成 bug 报。
- 本机（非 PS5）`:2026` 上 `app/register`、Sony `.pkg` 安装会返回 PS5-only 错，属预期。
- **表头冻结已修**（旧案：`.fmtable` 的 `overflow:hidden` 困住 sticky th + `border-collapse:collapse` 下 sticky 边框撕裂 ⇒ 改 `separate` + 滚动收进 `main` 容器）：列表下滚时表头毛玻璃钉在吸顶工具条下沿，三个视图共用。真机复核点：WebKit 的 `backdrop-filter` 与 sticky 组合表现。

## 5. 怎么在本机重跑验证
```bash
# Windows 侧（注意用 stdin 重定向，别用 bash -lc '...$VAR...'）
wsl.exe -d Ubuntu-22.04 bash < .build/build-verify-wsl.sh   # 构建 + ELF 内容 + 四道静态守卫 + hosttest(237) + ASan
wsl.exe -d Ubuntu-22.04 bash < .build/verify-linux-wsl.sh   # 同上但**不产出 ELF**（改完代码的常规一步）
wsl.exe -d Ubuntu-22.04 bash < .build/hosttest-wsl.sh       # 只跑测试，连跑两次
bash .build/frontend-verify-windows.sh                      # 前端真 Chromium 175 项
bash .build/cf-2026-1011-pkg.sh                             # 反证：四件事的 UI 六相（必须各自转红）
bash .build/nas-fixture.sh                                  # 可选：起 1500 上的 smbd，解锁 NAS 端到端那一段

# WSL 内手动
cd "/mnt/c/Users/songl/Desktop/Web File Manager/ps5-nexus"
make linux && python3 hosttest.py
make all                                                     # 需要 PS5_PAYLOAD_SDK
```

> ⚠️ `.build/build-verify-wsl.sh` 里的 `make all` 会**落一个带版本号的 ELF 到仓库根**。
> 按项目约定，产出 `PS5-Mango-vX.Y.Z.elf` 前先问用户；只想做常规验证就用
> `.build/verify-linux-wsl.sh`。

⚠️ **测试沙箱 `/tmp/ps5-nexus-hosttest` 必须在原生 Linux 文件系统上**（不能在 `/mnt/c`）：
drvfs 不实施 mode 位，套件里故意 `chmod 0555` 的只读夹具会**静默失效**。
若哪次是在 root 下跑的，残留目录普通用户删不掉，用
`wsl.exe -u root -d Ubuntu-22.04 bash < .build/clean-sandbox-root-wsl.sh`。

# PS5 Nexus — 真机复测清单

**产物**：`PS5-Nexus-v1.0.0.elf`（ELF64 / x86-64 PIE，PS5 payload）
**版本号可见性**：payload manager / etaHEN 的 *plugins → payload ELFs* 菜单**按文件名列出**，
所以版本号是**编进文件名**的 —— 列表里会直接看到 `PS5-Nexus-v1.0.0`。
⚠️ `src/main.c` 里的 `PROCESS_NAME` 故意**不带版本**：它是「杀掉旧实例」的匹配键，
带版本会导致升级时匹配不到旧进程，两个实例并存、新实例悄悄退到 2027 端口。
**构成**：owendswang/ps5-web-file-manager 最新版后端 `src/` + NEXUS 的 `assets/` 前端 + 新写的 `src/nexus_port.c`。
**本机验证**：`hosttest.py`（**80 项，全绿，连跑两次结果一致**）+ `.build/preview_check.mjs`（前端 57 项）。

---

## 0. 本轮（v1.0.0 改名）改了什么

| # | 改动 | 为什么 |
|---|------|--------|
| 1 | `BIN := PS5-Nexus-$(VERSION_TAG).elf`；`VERSION_TAG := v1.0.0` | payload manager 只显示**文件名**，版本不写进文件名就等于没版本。附带好处：版本进了 target 名，`make` 不会再因为「CFLAGS 变了但没有任何依赖变化」而复用旧产物 |
| 2 | `PROCESS_NAME` → `"PS5-Nexus.elf"`（不带版本），并把旧实例清理改成遍历 `{PS5-Nexus.elf, NEXUS.elf, ps5-nexus.elf}` | 旧名在真机上还跑着，不清掉就会两个实例抢 2026 端口 → 表现为「升级没生效」 |
| 3 | `TITLE_ID` 修正为 `NEXS88888` | 原来 Makefile 写 `FMGR88888`，而 `assets/param.json` 里写的是 `NEXS88888` —— `app_installer.c` 按 TITLE_ID 建 `/user/app/<id>`，两边不一致会装到启动器不认的目录 |
| 4 | `/api/status` 增加 `version` 字段 | payload manager 只能显示「你部署的那个文件名」，而 `/api/status` 返回的是**正在跑的那个二进制**编译进去的版本 —— 这才是「升级到底生效没有」的答案 |
| 5 | 补 `LICENSE`（GPL-3.0）、`THIRD_PARTY_NOTICES`；README 重写为 PS5 Nexus 的真实说明 | 原 README 还是上游「Web File Manager / 8888 端口」的原文，与现状（2026 端口、NAS/PKG/存档）不符 |

---

## 1. 上一轮修掉的两个真问题

| # | 症状 | 根因 | 修法 |
|---|------|------|------|
| 1 | 列表一页显示不下时，往下滑上面的功能按钮就没了 | 面包屑 + `.fmbar` 就是普通文档流元素，没有任何东西钉住它们 | 包进 `.fmstick`（`position:sticky`）；`top` 由 JS 实测顶栏高度写进 `--topH`（顶栏 1920 下 70px、900 下换行成 123px，**不是常数**），配 `ResizeObserver` |
| 2 | NAS 设置成 `192.168.1.3\PS5_Games` 后报 `Invalid address: ... Can not resolve into IPv4/v6` | 主机名扫描只认 `/` 和 `:`，整串 `192.168.1.3\PS5_Games` 被当成主机名丢给 `getaddrinfo` | 四层都按「`\` 等同 `/`」修：`transfer.c`（`path_sep()`）、`list.c`（`nas_build_url`）、`nexus_port.c`（`/api/fetch` 取 basename）、前端 `normalizeNasAddr()`。**口令里的 `\` 故意不转换**（转了就悄悄改了密码）。前端**读取时也归一**，因为已经踩坑的机器 localStorage 里存的就是坏地址 |

### 更早一轮：接缝层的 5 个 bug

| # | 症状 | 根因 | 修法 |
|---|------|------|------|
| 1 | 几乎所有前端操作都报「路径无效 / 读取失败 / Success」 | `body_json_value()` 解析完第一对 key 后**不跳逗号** → 多键 JSON 只认第一个键 | `path_util.c`：每对后面 `if(*p==',') p++` |
| 2 | 中文/日文文件名乱码或整条路径对不上 | `json_escape()` 逐字节转义 | `json_util.c`：先按 UTF-8 解码成码点再 `\uXXXX`（含代理对）；非法序列逐字节兜底 |
| 3 | `mkdir`/`rename` 失败报 `{"error":"Success"}` | `errno` 在传进 `strerror()` 前被重置 | 失败后**立刻** `int err=errno;` |
| 4 | `text/*`、`fs/read` 只认 JSON body | 用了 `body_json_value`（只 JSON） | 改用共享的 `req_param()`：JSON → form → query |
| 5 | `/api/pkg/info`、`/api/pkg/icon` 恒失败 | 前端是 POST + form，后端只放行 GET 且只读 query | 放行 GET/POST，改走 `req_param` |

## 2. 已搬过来的能力

- **存档域** `/api/save/list|backup|restore`：强制快照 → 原子落盘（temp+rename）→ 失败回滚；快照目录 `/data/savesnap/<TITLE_ID>/<时间戳>/`（**不放 `/data/save_files/`**，那里会被周期清理 unlink）。
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

> 部署：把 `PS5-Nexus-v1.0.0.elf` 放到 PS5 上能跑 payload 的位置，起来后浏览器开 `http://<PS5 IP>:2026/`。

| # | 操作 | 期望 |
|---|------|------|
| 1 | 在 payload manager 里看列表 | 显示 **`PS5-Nexus-v1.0.0`**（版本号就在文件名里）；启动通知也写明版本与端口 |
| 2 | 打开 `http://<IP>:2026/` | 页面正常；顶栏 **HTTP 灯亮**，端口 2026（被占用会顺延，前端自动跟随） |
| 3 | `http://<IP>:2026/api/status` | `version` 字段是 **v1.0.0** —— 与文件名一致才说明跑的是新构建 |
| 4 | 看顶栏「本机地址」胶囊 | 显示**真实内网 IP:端口**；点一下能复制 |
| 5 | `/data` 列目录，**条目多到一屏放不下**，往下滑 | 面包屑 + 工具栏**钉在顶栏下面不动**；滚动中按钮可点 |
| 6 | 中文/日文名 | 不乱码 |
| 7 | 点开一个较大的 `.log` / `.txt` | 弹出预览，有内容；**进程不退出** |
| 8 | 选中文件 → **删除** | 先弹确认；确认后入队，**文件视图内联显示进度**；完成后文件消失，**进程不退出** |
| 9 | 上传一个文件（按钮或拖拽） | 进度条走完 → 「上传完成」；落点字节数与源一致（**不再出现 `0/4294967296`**）；上传中若中断，落点**不应留下半截文件**（写的是 `.wfm-upload-*.tmp`，完成才 rename） |
| 10 | 上传一个**文件夹** | 子目录树在落点重建 |
| 11 | 新建文件夹 / 改名 / 复制 / 移动（粘贴） | 各自成功；任务出现在任务中心，完成后列表刷新 |
| 12 | NAS：地址栏填 `192.168.1.x\share`（**反斜杠**） | **能连上**（这是本轮修的）；再填一次正斜杠写法作对照 |
| 13 | NAS：从共享里拉一个文件到 `/data` | 有进度；中断后重拉**从断点续传**（不从头开始） |
| 14 | 右键/点击 `.pkg`（游戏库扫描也走一遍） | PKG 库出现卡片（标题/版本或 fPKG 镜像标签）；点「安装」→ 进度弹窗；fPKG 镜像会复制进 `/data/homebrew` |
| 15 | 存档页：填 TITLE_ID → 列表 → 备份 → 回滚 | 备份入队完成后出现新快照；回滚后存档回到快照内容 |
| 16 | 「主屏图标」按钮（**确保没有游戏在运行**） | 主屏出现/已存在本应用图标 |
| 17 | 再部署一次同一 ELF（升级/重载） | 旧实例被自动清掉，**仍然是 2026 端口**（不是 2027）—— 说明进程名复用链没断 |
| 18 | 最后一条再测：顶栏「关闭进程」 | 二次确认 → 收尾屏 → 进程退出（**不可逆**，PS5 没有 service manager 拉起） |

### 出问题时的定位手段
```
http://<IP>:2026/api/diag
```
- 它列最近 64 条请求：`{seq, method, url, done, toe}`。
- **最后一条 `done:false` 的 `url` 就是元凶**（那条请求没走完，进程就没了）。
- 把它连同 `/api/tasks` 的输出一起发我，就能直接定位到具体路由。

---

## 4. 已知空缺（不要当成 bug）

- **真 PS5 `.pkg` 内核安装**需要 `kstuff` 在跑；没装时 `/api/pkg/enqueue` 会报 unsupported（文件浏览/上传/解压不受影响）。
- **PFS 存档真正写回**（解密/重签）未接：`save/backup` 做的是文件级快照，真机上若存档目录本身不可直接写，需要在挂载层补。
- **解压已改为进程内引擎**（自研 `extract_engine`，下挂 UnRAR / LZMA SDK / minizip-ng，全部 vendored 进 `third_party/`）：不再依赖外部 helper ELF，`wfm-7zip-helper.elf` 相关报错（`archive_helper_elf_missing` 等）已整体移除。加密包由 UI 弹口令；分卷从任意卷入队都会归一到第一卷。
- 本机（非 PS5）`:2026` 上 `app/register`、Sony `.pkg` 安装会返回 PS5-only 错，属预期。
- **表头冻结已修**（旧案：`.fmtable` 的 `overflow:hidden` 困住 sticky th + `border-collapse:collapse` 下 sticky 边框撕裂 ⇒ 改 `separate` + 滚动收进 `main` 容器）：列表下滚时表头毛玻璃钉在吸顶工具条下沿，三个视图共用。真机复核点：WebKit 的 `backdrop-filter` 与 sticky 组合表现。

## 5. 怎么在本机重跑验证
```bash
# Windows 侧（注意用 stdin 重定向，别用 bash -lc '...$VAR...'）
wsl.exe -d Ubuntu-22.04 bash < .build/build-verify-wsl.sh   # 构建 + ELF 内容 + hosttest(83)
wsl.exe -d Ubuntu-22.04 bash < .build/hosttest-wsl.sh       # 只跑测试，连跑两次
node .build/preview_check.mjs                                # 前端 57 项

# WSL 内手动
cd "/mnt/c/Users/songl/Desktop/Web File Manager/ps5-nexus"
make linux && python3 hosttest.py
make all                                                     # 需要 PS5_PAYLOAD_SDK
```

⚠️ **测试沙箱 `/tmp/ps5-nexus-hosttest` 必须在原生 Linux 文件系统上**（不能在 `/mnt/c`）：
drvfs 不实施 mode 位，套件里故意 `chmod 0555` 的只读夹具会**静默失效**。
若哪次是在 root 下跑的，残留目录普通用户删不掉，用
`wsl.exe -u root -d Ubuntu-22.04 bash < .build/clean-sandbox-root-wsl.sh`。

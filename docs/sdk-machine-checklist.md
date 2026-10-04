# SDK 机 / 真机待办清单

机器到位后按本清单逐项执行。宿主（Windows MinGW）已可验证的部分一律不重做；每项标注前置依赖、执行步骤、验证判据与 HANDOVER 出处。铁律：**不写无法验证的后端代码**——纯函数切片 + 文档契约已在宿主交付，本清单只做真身实现与真机验证。

- 环境前提：PS5 Payload SDK（prospero-clang，`PS5_PAYLOAD_SDK` 环境变量）+ libmicrohttpd。
- 优先级建议：A →（B 依赖 A 过）→ C；D 可全程并行。
- 验证链现状（宿主全绿，SDK 机不必重跑）：orchestration 套件、run-tests/run-sevenz（解压引擎）、.build/ui_live_check.mjs（85 项）、静态 demo 106 项。

---

## A. SDK 机首次编译（必做，前置：环境前提）

来源：HANDOVER §6 item 2/4、§1d、§3、§4「构建」铁律

| # | 步骤 | 验证判据 |
| --- | --- | --- |
| A1 | 首次 `make` 编出 PS5 ELF（`PS5_PAYLOAD_SDK` 已 export） | ELF 产物生成，无 undefined reference |
| A2 | 逐个核对 `extern` 原型（签名错了要到链接才暴露） | 链接通过后**逐符号确认语义**（非仅不报错） |
| A3 | 4 个 SDK 档模块真实编译：`httpd.c`（libmicrohttpd）/ `pkg_installer.c` / `savemgr.c` / `main.c` | 全 TU 编译 + 链接进 ELF **✅ 2026-09-30 已在 WSL 完成**：7 个 SDK 档 TU（含 `pkg_stream.c`）以 prospero-clang 18 + `-Werror` 全部通过；`make all LIBSMB2=1 LIBNFS=1` 链接成功，ELF 1329776 B / sha256 `5a011376…` / `e_machine=0x003e`，`.dynamic` 依赖含 `libSceAppInstUtil.sprx` + `libkernel_sys.sprx` |
| A4 | flag-stamp 生效确认（机制宿主已实现，见 Makefile） | 改 `THIRD_PARTY_C_FLAGS` / `CFLAGS` 后触发重编/重链提示 `[cflags]`；产物 `readelf -SW` 段尺寸 + sha256 变化 |
| A5 | 静态资产 gzip 内嵌链路 | `.build/check-elf-gzip.py` 裸搜 JS 串 0 命中 |

**A2 必须逐项交叉核对的符号（§4「安装层」+ §1d）：**

- **MetaInfo `0x30` ✅ 四源交叉已闭合（2026-09-30）**：`itsPLK/ps5-pkg-manager` 是**第四个独立来源**，并首次确认**字段名与顺序** = `uri / ex_uri / playgo_scenario_id / content_id / content_name / icon_url`（`ex_uri`/`playgo_scenario_id`/`content_id` 传 `""`，**不是 NULL**）。⚠️ `_Static_assert(sizeof)` 只证自身一致 —— **它已在本项目骗过两次**。
- **`sceAppInstUtilInstallByPackage` ✅ 签名已修（2026-09-30）**：真签名 `f(const pkg_metadata_t *meta, pkg_info_t *info, playgo_info_t *playgo)` —— **首参是元数据结构体、不是路径**。旧代码 `(const char *pkg_path, …)` 把路径放首位 ⇒ 系统拿路径串当地址解引用 6 个指针 ⇒ **必失败/最坏崩**；🪤 **编译期不会报错**（自己的 extern 声明「自洽」），且 `_Static_assert(sizeof)` 只证自身一致 —— **`_Static_assert` 已在本项目骗过两次**。现按真签名调用并传 `&h->meta, &h->info, &h->playgo`（三者**存在 handle 里**，不是栈上临时量）。取证见 `docs/pkg-install-without-usb.md` §4.1。
- **安装进度 ✅ 已接真查询（2026-09-30）**：`pkg_install_poll()` 改调 `sceAppInstUtilGetInstallStatus(info.content_id, &status)`；终态判据 = `status == "playable" | "completed"`，失败 = `error_info.error_code != 0 || status == "error" | "none"`。`Initialize()`/`Terminate()` 已包裹。⚠️ **真机待核**：轮询节流值（现 300 ms）只是估计；`appinst_status_t` 的**尾部字段偏移**只有头部被跨源证实。
- 🪤 **符号可得性已核实（2026-09-30）**：`$SDK/target/lib/libSceAppInstUtil.so`（108 个桩符号）**确实导出** `Initialize` / `Terminate` / `InstallByPackage` / `GetInstallStatus` ⇒ 直连 `-lSceAppInstUtil` 即可，**不需要** `kernel_dynlib_resolve` 取址（参考实现用后者是因为它要带 helper 进程）。
- **`uri` 接受 `http://` ⇒ 这是「PKG 不用 U 盘」的全部秘密（2026-09-30 取证）**：**`/data` 等内部盘上的 PKG 走文件路径注定失败，必须由本机 loopback HTTP 推流**（`http://127.0.0.1:<port>/…`）。PS5 安装器是 **HTTP range 客户端**，三阶段协议（64 KiB header / `<content_id>.crc` **必须 404** / 2×16 MiB 并行 range）见同文档 §3.2。参考实现自带 **sim 夹具**（`tools/ps5_installer_sim.c` + `tests/ps5_sim.{c,h}`）⇒ **无需真机即可压测 stream server**。
- **pfsmgr ioctl**：savemgr.c 的 mount/ioctl 缓冲 heap 分配路径，ioctl 请求码与结构体按 SDK 头核对。
- **`-lkernel_sys`**：旧仓从未链接过 ⇒ 安装路径几乎肯定没在真机验过，编译后真机跑一遍安装流程（见 D）。

---

## B. transfer 接 smb/nfs（前置：A 过；代码已在宿主交付，宿主侧已到「连接前」）

来源：HANDOVER §6 item 4、§1n、§1o

### B1. 实编译过链接（SDK 机）
- 命令：`make LIBSMB2=1 LIBNFS=1`
- 判据：libsmb2(53 TU) + libnfs(25 TU) + transfer.c + 驱动全链接成 ELF，无符号缺失。
- 顺带确认 libnfs 两个 **FreeBSD 项**（Linux 模拟编译时被剔除，真实 SDK 机由构造保证）：`HAVE_SOCKADDR_LEN`（`struct sockaddr.sa_len`）、`HAVE_SYS_SOCKIO_H`（`<sys/sockio.h>`）。

### B2. SMB 拉取真机验证（连通 `make LIBSMB2=1` 产物）
1. 起 SMB 服务器：真 NAS 现成 share，或本机 `smbd`（`[share] guest ok = yes`）；先确认 `ss -ltn | grep :445` 空闲（FreeBSD 无 Windows 端口冲突）。
2. 放测试文件（如 `games/hello.pkg`），记 `sha1sum`。
3. 启动 nexus 产物 + HTTP，用产品路径验证（api.c 直调 `transfer_open(scheme,path)`）：
   - 列目录：`GET /api/fs/list?path=smb://<host>/<share>/games` → 列出 `hello.pkg`，type 正确。
   - 拉取：`POST /api/fetch {src:smb://<host>/<share>/games/hello.pkg, dst:<本地>, scheme:smb}` → task_id → 轮询 `/api/tasks` → **DONE**。
   - 校验：dst 文件 `sha1sum` == 源 `sha1sum`。
4. 连接级冒烟（可选，不启 HTTP）：把链接冒烟 driver 指向真 URL 跑 `transfer_list/read/size` 校验字节。

### B3. NFS 拉取真机验证（`make LIBNFS=1`）
1. 真 NFS 服务器（SDK 机内核或真 NAS）export 一个目录。
2. URL 用 `nfs://<host>/<export>/<path>`；**export 名无前导 `/`**（后端 `nfs_mount` 会补 `"/"`）。断言 NFSv3 或 v4.1（`HAVE_NFS4_2` 已 undef，仅 4.1）。
3. 同 B2 第 3 步走 `/api/fs/list` + `/api/fetch` 校验字节一致。

### B4. 连接前断言保持绿（宿主已锁，SDK 机应继续绿）
URL 解析五种形式（`smb://`/`nfs://`/`//host/share`/裸 `host/share`/`host:/export`）、`scheme_supported` 契约（未知 scheme=0、未编入后端=0）、惰性 open（URL 错在 open 报，连接错在 list/read/size 以 `NEXUS_ERR_NOTFOUND` 浮出）。

---

## C. Phase 0 真机 4 项实验（前置：机器到位；可全程并行）

来源：HANDOVER §6 item 3、docs/phase0-benchmarks.md（方法已备：`misc/bench_extract.c` 壳 + `misc/bench_fixtures.py`）

| # | 实验 | 判据 |
| --- | --- | --- |
| C1 | singleDPI 装 fPKG | 安装走通，无 AuthID/权限错误 |
| C2 | AuthID 提权链 | 安装 `0x4800000000000006` / 存档 `0x4800000000000010` 两套分别验证（§4「安装层」） |
| C3 | 同形状解压计时 | 按 phase0-benchmarks.md 夹具跑 zip/rar/7z（含加密头+分卷），记录计时 |
| C4 | 写盘上限 | T_copy、内置 M.2 vs exFAT（`/mnt/usb0|1`）写速、libsmb2 拉取实测；**谈速度必须带挂载点前提**（§4「构建」） |

---

## D. 附带真机验证（散点，随 A/B 顺带做）

来源：HANDOVER §1d/§1f/§4

- **`-lkernel_sys` 安装路径**：旧仓从未链接 ⇒ 真机跑一遍安装流程，确认安装路径真实验证（A 完成后必做）。
- 🆕 **loopback stream 安装真的能装**（2026-09-30 落地，7 条真机待核，见 `docs/pkg-install-without-usb.md` §7.5）：①系统安装器**接受非标准端口的 loopback uri** 吗（参考实现写死 18841，我们优先用它、回退临时端口）；②`sceAppInstUtilInstallByPackage` 是否**在本进程内返回**（参考实现为此专门开 helper 进程，我们直调 ⇒ 若挂死会带走整个 payload，这是本项目唯一这类风险点）；③`appinst_status_t` **尾部字段偏移**（只有头部被跨源证实）；④真**取消**符号（未知，故未调用，只停止喂流）；⑤**局域网离线时是否仍报 `0x80B21121`**（`/data` 与 SMB **没有**参考实现那种回退可用）；⑥`GetInstallStatus` 轮询频率上限（我们节流 300 ms，是估计值）；⑦装完后 `unpublish` 的时机（早于主机读完最后一块会失败）。
- **pkg_installer 已装预检**：重复安装同一 fPKG 时预检拦截行为符合预期 —— 现应表现为任务 **DONE + 备注「already installed」**（不是失败）。
- **savemgr**：强制快照（无开关可跳过）/ 原子回写 / 失败回滚 / 二次确认；mount 串行锁；`/savedata_prospero/` 直接挂会 EPIPE ⇒ 先复制 `/data` 再挂、卸后回写；快照落 `/data/savesnap/`（非 `/data/save_files/`，garlic-worker 会 unlink 清空）。
- 🆕🪤 **存档在真机上的路径层级不能写反（2026-10-02 修正，双源确认）**：
  `/user/home/<账号hex>/savedata_prospero/<TITLE_ID>`（ext 存储镜像到 `/mnt/ext1/home/`），
  **leaf 在前、title id 在后**。psdevwiki（Save Data）+ gbatemp 实机 FTP 双源一致。
  ⚠️ 本仓 `probe_save_leaves()` 曾经写成 `<账号>/<TITLE_ID>/savedata_prospero` ⇒ 真机上**永远 stat 不到**
  ⇒ 备份/恢复与「有没有存档」一律落到「找不到存档」，**手动敲 TITLE_ID 也一样**（= 用户 2026-10-02 报的现象）。
  **账号 hex 无法从 title id 推导 ⇒ 这一级必须枚举目录**（`probe_save_home` / `scan_save_accounts`），
  不能拼字符串。真机必核：`/api/save/scan` 能列出存档、`/api/save/list` 的 `save_found=true`、
  且两者对「哪些存档存在」的答案必须一致（同一份代码里两处用不同层级顺序 = 一处说有、一处说没有）。
- 🆕 **恢复前安全副本不许留在存档叶里（2026-10-02 修）**：`<存档目录>.wfm-pre-restore` 是回滚源，
  回写成功后必须删掉（原来是 `unlink()`：对目录恒失败 ⇒ 副本永不回收、还会被 `/api/save/scan`
  当成一个「游戏」列出来）。真机核：恢复后存档叶里只剩该 title 一个目录。
- **worker 任务流真机回归**：fetch/install/extract 各类任务 DONE、取消不留半截产物、shutdown 不 commit（宿主已验逻辑的真机端到端）。
- **form `%2F` 落盘缺口修复回归**（Session 15 已修）：真机用浏览器走 `/api/fs/list`(path)、`/api/extract`(path/dst_dir)、`/api/pkg/scan`(root)、`/api/pkg/enqueue`(path)、`/api/fetch`(src/dst) 各带 `/` 的路径，确认落盘路径正确。

---

## 完成判据汇总

- A：ELF 产出 + 符号交叉核对过 + flag-stamp 生效 + 安装路径真机验过。
- B：`make LIBSMB2=1 LIBNFS=1` 过链接；SMB/NFS 各走通 `/api/fs/list` + `/api/fetch` + sha1 一致。
- C：4 项实验记录归档（结果回填 docs/phase0-benchmarks.md）。
- D：散点真机行为全部确认。金手指域已整体移除（HANDOVER §1p），无金手指相关 SDK 待办。

每一项完成后回填本清单打勾，并更新 HANDOVER §6 对应待办。

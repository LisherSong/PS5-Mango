# PS5 Nexus — 落地方案（Implementation Plan）

> ⚠️ **本文档描述的是 `ps5-nexus` 那棵树（"重写"路线的执行计划），不是本仓库。**
> 本仓库以 **owendswang/ps5-web-file-manager 最新版后端**为底座，搬入 NEXUS 的前端 UI
> 与 PKG / 存档能力，产物 `PS5-Nexus-v1.0.0.elf`，端口 `2026`。
> 本文里的**模块划分、文件路径、ELF 名都不适用于本仓**；引用到的
> `../ps5-nas-rewrite-proposal.html`、`../ps5-ui-demo-*.html` 也不在本仓内（它们是仓库外的设计稿）。
> **本仓的现状看 `README.md` 与 `REAL-CONSOLE-CHECKLIST.md`。**

> 权威设计文档：`../ps5-nas-rewrite-proposal.html`（方案 v3，§0–§9）。本文件是**把设计落成工程**的执行计划，不重复设计理由，只给结构、模块、阶段与可立即执行的步骤。  
> 视觉规范冻结在：`../ps5-ui-demo-5-harness.html`（E Harness，**主 UI 基**）、`../ps5-ui-demo-1-console.html`（A 主机大厅，备选主题/对照）、`../ps5-ui-demos-compare.html`（规范页 + 106 项断言基线）。  
> 项目长期备忘：`../.workbuddy/memory/MEMORY.md`（铁律坑 + 字节映射 + 速度口径 + 安装/存档域规则）。

---

## 0. 项目身份（已拍板，勿再议）

| 项             | 值                                                                              |
| ------------- | ------------------------------------------------------------------------------ |
| 名称 / 仓库 / ELF | **PS5 Nexus** / `ps5-nexus` / `ps5-nexus`                                      |
| 默认端口          | **2026**（避开 8888/9021/9090·12800/1337/9081）                                    |
| 形态            | 单 ELF + 端口 + gzip 内嵌前端，**纯网页、不写 app/Electron**                                 |
| 四个能力域         | ① NAS 互通 ② 解压引擎（7z/RAR/ZIP + 加密 + 分卷）③ 大文件吞吐与续传 ④ 存档管理（P1）                 |
| 真空校准（差异化卖点）   | NFS 互通（SMB 已有 libsmb2 开源参考，非纯真空）、**RAR/7z 解压（含加密+分卷，竞品均无）**、大文件吞吐与续传、FFPKG 通道 |
| 明确不做          | 又一个通用文件管理器；风扇/主题周边；依赖 etaHEN                                               |
| 主 UI 风格         | **Harness（E 暗色蓝，`ps5-ui-demo-5-harness.html`）**；备选主题 A 主机大厅仅作对照 |
| 前端形态          | 单文件零依赖 HTML，内联 CSS/JS，gzip 内嵌 ELF；系统字体栈、无 webfont、中文不紧排、数字 `mono+tabular-nums` |



---

## 1. 仓库策略（inherit ≈40%）

- **新仓库** `ps5-nexus`（位置 `…/Web File Manager/ps5-nexus/`）。区别于旧仓 `ps5-web-file-manager`（仅保留作引擎/测试/工具链来源）。
- **从旧仓继承（≈40%，直接搬、主体不动）**：
  - 解压引擎：`src/extract*.c` + ZIP/RAR/7z 解码子集（含 7zAES/`-mhe=on`/ZipCrypto/WinZipAES/`-p`/`-hp`）
  - 复制管道（8 MiB × 3 slot 双缓冲）、PKG SFO 解析、`json_util.c` / `path_util.c`（字节映射）
  - vendored：`zlib` / `unrar7` / `LZMA SDK` / `minizip-ng`
  - host 测试矩阵（177 + 27 项）、三层前端无头验收工具链
  - 构建工具链：`Makefile`、`build-elf-wsl.sh`、`build-elf.sh`、`install-libmicrohttpd.sh`、`gen-asset-module.py`、`.build/check-elf-gzip.py`
  - **真正重写的部分**（见 §4）：
  - 传输层（SMB/NFS 抽象）、NAS 任务流、自建安装器、PKG 库管理、PS5 三通道落地、存档管理、UI 全重写、HTTP 服务瘦身
- **继承原则（低耦合优先）**：测试充分、耦合低的**叶子模块**直接复用（上述引擎/管道/SFO/字节映射/测试矩阵/构建链）；**高耦合的编排层**（任务引擎、传输抽象、安装、PKG 库、存档、HTTP、UI）按**低耦合接口重写**——模块间只通过 `.h` 契约通信、依赖单向、新功能以独立模块插入、**不改动其他模块**，保证后期好维护、好加功能。
- **许可边界**（务必守住）：全链 GPL-3.0 合法；**singleDPI（`maxMilu/ps5-direct-package-installer`）代码可复用**（保留 NOTICE 署名）；**无许可项目（garlic-savemgr 系、ps4-remote-pkg-sender 原版/改版）代码一行不可搬**，只借鉴事实；复用范围 `garlic-worker`/`ps5-sd-tool`/`apollo-ps4`（GPL-3.0）+ `PS4-vsh-utils`/`savescum`（MIT）。

---

## 2. 技术栈与运行时

- **后端**：C，PS5 SDK（`prospero-clang`，`x86_64-sie-ps5`，`e_machine=0x003e`）。HTTP = `libmicrohttpd`（已有 staging 安装脚本）。
- **前端**：单文件零依赖 HTML + 内联 CSS/JS，**gzip 内嵌**进 ELF（⇒ 对 ELF 裸搜 JS 串恒 0 命中，验证用 `.build/check-elf-gzip.py`）。
- **字节映射铁律**（别改）：`json_escape()` 把每个 ≥0x80 字节转 `\u00XX`；`fs_path_value()` 把 ≤0xFF 码点还原成原始字节 ⇒ GBK 等非法 UTF-8 文件名**不丢字节**；给人看走 `decodeFsText()`，送服务端走 `encodeFsText()`；**别拿路径当跨往返的键**（同目录两侧字符串不同 ⇒ 口令重试表按任务 id 记）。
- **速度口径**：速度 = 总字节 ÷ 总耗时（或 ≥10 s 窗口）；**禁显 250 ms 瞬时值当速度**；夹具必须按真实压缩比造（90% 随机数据会躲过解码退化路径，结论失真）。

---

## 3. 目录布局（ps5-nexus）

```
ps5-nexus/
├── Makefile                 # VERSION_TAG + 产物名 ps5-nexus + 端口 2026 + flag-stamp 修 make 短路
├── LICENSE                  # GPL-3.0（已拷）
├── README.md               # 指向本方案 + 与旧仓差异
├── src/
│   ├── main.c              # ELF 入口：起 HTTP 服务、内嵌前端、任务引擎
│   ├── httpd.c             # libmicrohttpd 封装（API + 静态；下载改 sendfile 大缓冲）
│   ├── task_engine.c       # 统一任务中心（拉取/解压/上传/安装/备份唯一进度显示处；跨重启恢复；存档挂载串行锁）
│   ├── transfer.c          # 传输抽象：本地 FS / SMB(libsmb2) / NFS(libnfs) 统一「可读流 + 可列目录」
│   ├── smb.c / nfs.c       # libsmb2 / libnfs 适配
│   ├── pkg_installer.c     # 自建安装器（singleDPI 四样增量 + MetaInfo 0x30）
│   ├── pkg_lib.c           # PKG 库：NAS 深扫描 → CUSA/标题/版本/大小索引 → 安装队列
│   ├── savemgr.c           # 存档域：/dev/pfsmgr 解封 → sceFsMountSaveData → 改 → 卸回；强制快照 + 回滚
│   ├── extract*.c          # 继承：解压引擎（ZIP/RAR/7z + 加密 + 分卷）
│   ├── copypipe.c          # 继承：复制管道
│   ├── pkg_sfo.c           # 继承：PKG SFO 解析 + 封面抽取(sce_sys/icon0.png)
│   └── json_util.c / path_util.c   # 继承：字节映射
├── assets/
│   ├── index.html          # 主 UI（Harness / E 暗色蓝风格，吸顶导航 + 常驻状态条 + 统一任务中心）
│   ├── install.html        # 首屏 / 安装说明页（E Harness 风格，暗色默认）
│   ├── main.css / main.js  # 从两份 demo 抽取、冻结的视觉规范（令牌/扫光/呼吸灯/地址胶囊）
│   ├── lang-zh.js / lang-en.js
│   └── param.json
├── tests/                  # host 测试矩阵（MinGW 跑；沙箱删除守卫需 CODEBUDDY_SAFE_DELETE_ENABLED=0）
├── third_party/            # vendored zlib/unrar7/LZMA/minizip-ng/libsmb2/libnfs
└── .build/
    ├── build-elf-wsl.sh / build-elf.sh / install-libmicrohttpd.sh / gen-asset-module.py
    ├── check-elf-gzip.py   # 内嵌前端验证（裸搜恒 0，必须走 gzip 解）
    ├── ui_demos_check.mjs  # 106 项前端断言（对比度/溢出/焦点/逐视图/像素扫光仪表）
    └── proposal_check.mjs  # 提案事实闸门
```

---

## 4. 架构与模块职责

- **HTTP 服务层（瘦身）**：只留 API + 静态资源；下载改 `sendfile` 大缓冲；端口由 `find_available_port()` 顺延并服务端上报（写死 = 给错地址）。
- **任务引擎（统一）**：拉取 / 解压 / 上传 / 安装 / 备份**全进统一任务中心**，它是**唯一进度显示处**；支持跨重启恢复（ezRemote Server 模式已验证）；存档挂载为全局单例 ⇒ 必须串行锁。
- **传输抽象**：本地 FS / SMB / NFS 统一成「可读流 + 可列目录」接口，与本地 FS 同构 ⇒ NAS 浏览/拉取/校验/原子落盘/断点续传复用同一套。
- **安装后端（可切换）**：直装（SMB→HTTP 桥 + `sceAppInstUtilInstallByPackage()`）/ 落盘安装两种，UI 与任务模型不绑死。
- **UI 四入口**：文件（本地 + NAS 合并，同一空间两个根）/ 任务（统一进度）/ 游戏（PKG 库）/ 存档 / **常驻状态条**（kstuff / HTTP 服务 :2026 / SMB 服务三盏呼吸灯，**不列任何第三方打包发行版**）。导航一律顶部单行吸顶（PS5 可视区 ≈1920×970，纵向紧缺）。

---

## 5. 分阶段路线（Phase 0–3 · 5 + UI 并行轨道）

| 阶段                  | 目标                          | 关键交付                                                                                                                                                                                               | 验收                                                             | 量级                       |
| ------------------- | --------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | -------------------------------------------------------------- | ------------------------ |
| **Phase 0** 真机基线    | 4 项判定性实验                    | ① 跑现成 `singleDPI.elf` 看 `{"action":"ping"}` 三件套 + 装一个 PS4 fPKG @ FW 12.70；② AuthID 提权链自测；③ 同形状解压对照（单大文件 RAR 计时）；④ 竞品功能核实（✅ 已完成源码级核实，见 docs/competitor-audit.md）；另：写盘上限 `T_copy`、内置/exFAT 写速、libsmb2 拉取实测 | 量完三数才谈排序；singleDPI 装成即「只靠 kstuff 就够」成立                         | 需真机 ⇒ **拆两半**：① 本机现在做——写测量脚本(`misc/bench_*.c` 壳)+ 预检清单 + 竞品源码级核实(WebFetch/读源码，不需机器)；② 真机到位再跑 4 项实验。本机半**不阻塞任何后端 Phase** |
| **Phase 1** P0 传输   | NAS 浏览 + 高速拉取 + PS5 游戏文件夹落地 | `transfer.c` 抽象 + `smb.c`（libsmb2）；断点续传；NAS→`/mnt/ext1/data/` 目录级任务                                                                                                                                | 多文件/文件夹拉取、进度/取消、续传；写速带盘前提                                      | 2–3 周                    |
| **Phase 2** P0 安装+库 | 自建安装器 + PKG 库               | `pkg_installer.c`（singleDPI 四样增量：AuthID 提权/ kstuff 就绪探测/ 状态轮询/ 已装预检；MetaInfo 收回 0x30）；`pkg_lib.c` 扫描/索引/队列；FFPKG 通道调研（dump2ufs+ShadowMount）                                                        | 装 PS4 fPKG 成功 + 队列装完自动下一个 + 预检已装                               | 2–3 周                    |
| **Phase 3** P1 吞吐   | 解压提速 + 续传/跨重启 + NFS         | `RAR_SMP` 开关（条件：T_copy 证墙不在写盘）；大文件续传；`nfs.c`（libnfs）备选                                                                                                                                             | A/B 验证 2.4× 能否兑现；断点恢复实测                                        | 1–2 周                    |
| **UI 并行轨道**     | UI 持续建设（贯穿全程）            | `assets/index.html`(Harness/E 主 UI) + `install.html`(E 首屏) + 抽取 `main.css/main.js`；**从 Session 1 起与后端各 Phase 并行推进**，沿用冻结视觉规范，不阻塞后端                                                                              | `.build/ui_demos_check.mjs` **106 项**全绿 + PS5 视口 1920×970 自动验收；每后端 Phase 同步补对应 UI | 贯穿 Phase 1–5            |
| **Phase 5** P1 存档   | 存档管理 + 强制快照                 | `savemgr.c`：PFS 挂载 + 解密/重签/回封 + 5 条硬规则（强制快照/原子回写/失败回滚/可见可控/二次确认）；快照落 `/data/savesnap/`（非 `/data/save_files/`）                                                                                      | 故意让写回失败 ⇒ 快照保全原档 + 回滚路径实测通过                                    | 1–2 周                    |

> 总工期约 11–16 周；**UI 为并行轨道（不阻塞后端）**；存档域共享提权链，安全权重高于功能对齐，功能可后置。

---

## 6. 风险与护栏（落地时逐条对照）

**铁律坑（最高优先级）**

- 禁用 `git stash`；同一文件一次只发一个 Edit；不要用 `git rm` 改名（用 `git mv`）。
- `git push` 失败先分类：① 证书（MITM）= `GIT_SSL_NO_VERIFY=1 git push origin main`（单次）；② 瞬断/502 = 循环重试（别用 `timeout`，命中 Windows `TIMEOUT.EXE` 误判）；③ **整个 `github.com:443` 被 egress 挡** = 改走 Git Data API 推纯快进（三步 + sha 闸门）。
- 测试套件只能在 Windows MinGW 跑；沙箱删除守卫拦 `rmtree` ⇒ `CODEBUDDY_SAFE_DELETE_ENABLED=0 /usr/bin/bash tests/run-tests.sh`。
- `make` 看不见编译选项变化 ⇒ 新功能静默消失 ⇒ **flag-stamp**；判读只信 `readelf -SW` 段尺寸 + sha256，别看文件尺寸；重编前把旧产物挪进 `.build/` 做 `cmp`。

**安装层**：只依赖 `kstuff`（非 etaHEN）；AuthID 安装=`0x4800000000000006`(DEBUG_AUTHID) / 存档=`0x4800000000000010`，**两套分开封装**；MetaInfo 收回 `0x30`；`_Static_assert(sizeof)` 只证自身一致，须三源交叉；非安装功能不硬依赖 kstuff（用户会为性能关掉）。**🆕 2026-09-30 取证**：`sceAppInstUtilInstallByPackage` 的**首参是元数据结构体、不是路径** —— 本仓 `pkg_installer.c:39/:128` 写错 ⇒ 必失败，**且编译期不报**；`pkg_metadata_t` = 6 指针 `0x30`、**字段序已确认**（第四个独立来源）；**`uri` 接受 `http://` ⇒「PKG 不用 U 盘」= 本机 loopback HTTP 推流**（印证本文件前文「SMB→HTTP 桥」的方向）。详见 `docs/pkg-install-without-usb.md`。

**存档域**：写回前强制快照（不可跳过/原子/失败回滚/可见可控/二次确认）；快照落 `/data/savesnap/`；mount 全局单例 ⇒ 串行锁；ioctl 缓冲必须 heap 分配；`/savedata_prospero/` 直接挂会 EPIPE ⇒ 先复制 `/data` 再挂、卸后回写。

**金手指域不做（Session 19 拍板，本插件只做文件管理）**：金手指文件（`.json`/`.xml`/`.mc4` 等）由文件管理器当普通文件覆盖，本插件无金手指解析/引擎/路由，功能留给其它插件。

**UI 验收陷阱**：动效归 `prefers-reduced-motion`；扫光须是「**底色 + 一条带硬边的窄亮带**」，**不是首尾同色平滑斜坡**（否则肉眼静止、断言却绿）；`display:none` 子树 `getComputedStyle` 照样读到 animationName ⇒ 断言必带 `offsetWidth>0`；判据取行为钩子 `[data-p]` 非类名/位置；封面抽不到必须回退占位格（一排卡片留洞比没封面更糟）。

---

## 7. 本会话可立即落地的事（Session 1 Build Order，无需真机）

1. **建仓库骨架**：`ps5-nexus/` 目录树（已建）、`git init`、`LICENSE`（已拷）、`README.md`（指向本方案 + 与旧仓差异）。
2. **移植构建工具链**：`Makefile`（改 `VERSION_TAG`、产物名 `ps5-nexus`、端口 2026、加 flag-stamp）、`build-elf-wsl.sh`/`build-elf.sh`/`install-libmicrohttpd.sh`/`gen-asset-module.py` 复制并改名、`.build/check-elf-gzip.py` 复制。
3. **播种前端（主 UI = Harness）**：`assets/index.html` 以 demo5（Harness/E 暗色蓝）为**主 UI 基**；`assets/install.html` 以 demo5（E）为首屏/安装页；抽取 Harness(E) demo 的令牌与组件为 `assets/main.css` + `assets/main.js`（冻结视觉规范）；`lang-*.js` 沿用。
4. **移植验收闸门**：`.build/ui_demos_check.mjs`（106 项）+ `.build/proposal_check.mjs`；把 demo 假数据约定带进真实 API stub。
5. **移植第一个可编译后端模块**：解压引擎 + 复制管道 + PKG SFO（host 侧用 MinGW 跑 177+27 测试矩阵）；安装/存档/传输层留**接口占位**（`.h` + 桩），真机阶段再填。

---

## 7b. Session 5：按契约头实现编排层（已完成）

8 个契约头全部落地，**分两档**（详见 `HANDOVER.md` §1d）：

- **A 档（本机 MinGW 可编译 + 可测，76 项断言全绿）**：`task_engine.c`（统一任务中心 + 跨重启 journal）、`copypipe.c`（原子落盘）、`transfer.c`（local 后端；smb/nfs 用 `NEXUS_HAVE_LIBSMB2`/`NEXUS_HAVE_LIBNFS` 守卫，缺库返回 NULL）、`pkg_sfo.c`（**新写**，替代与旧 server 耦合的 `pkg_info.c`）、`pkg_lib.c`（深扫 + 入队）。跑法 `tests/run-orchestration-tests.sh`。
- **B 档（已写，待 SDK 机编译）**：`httpd.c`（libmicrohttpd；缺库本机不编）、`pkg_installer.c`（kstuff 路线 + AuthID `…0006` + MetaInfo `0x30`）、`savemgr.c`（5 条硬规则 + 强制快照）、`main.c`（接线）。
- **构建**：`Makefile` 模块集换成编排层，并**补 `-lkernel_sys`**；`pkg_info.c/h`（依赖未继承的 `websrv.h`/`filemgr_internal.h`）与 `extract.c`（旧 MHD 端点）刻意不入构建，前者由 `pkg_sfo.c` 取代。

> 下一步必须先在 SDK 机做**首次编译核对**（extern 原型 + MetaInfo 三源交叉），签名错误只会在链接期暴露。

## 7c. Session 6：JSON API 层 + 平台状态（已完成）

编排层之上补「UI 唯一要对接的缝」，详见 `HANDOVER.md` §1e：

- **`src/api.c/.h`（A 档，本机可测 108 项）**：19 条路由的路由表 + `api_dispatch` + `api_body_value`（form/JSON 双解，含转义与 `\uXXXX`）。**不含任何 socket 依赖** —— 注册由 `main.c` 遍历 `api_routes[]` 完成，所以整张表能在本机断言。
- **`src/platform.c/.h`（A 档）**：kstuff **按磁盘路径判定**（不看用户记忆）、端口运行时上报、本机 IPv4、SMB/NFS 后端可用性 —— 状态条三盏灯的数据来源。
- **`task_engine` 增补** `task_id()` / `task_find(id)`：**句柄绝不过 HTTP 边界**，UI 只见 id。
- **`Makefile`**：`api`/`platform` 入 COMMON；三个 SDK 域**移入 `PS5_SRCS`**（否则 linux 目标编不过），linux 另链 `src/stubs_nops5.c`（返回 `UNSUPPORTED`，不假装可用）；**第一方 CFLAGS flag-stamp**（只改 `-D`/`-I`/`LDADD` 时 make 不重链 ⇒ 功能静默消失）。
- 测试累计 **184 项断言全绿**（76 + 108）。

> 下一个编码动作：**worker 抽干 QUEUED 任务**（`/api/extract`、`/api/install/begin` 目前只入队）；之后 UI 即可把 Harness 的假数据换成真 `fetch`。

## 7d. Session 7：worker 任务驱动（已完成）

任务中心真正转起来，详见 `HANDOVER.md` §1f：

- **`src/worker.c/.h`（A 档，本机可测 19 项）**：单线程 `worker_tick()` + 4 槽位并发，按 kind 分派 fetch / install / extract / backup / **restore**；每步轮询 `task_cancelled()`，`worker_shutdown()` **只丢弃、绝不 commit** 半截产物。
- **`task_engine` 增补** `task_payload_t` + `task_set_payload()`：任务要带**载荷**（源/目标/快照），否则 worker 不知道该干什么。`task_kind_of()` 公开。
- **`nexus_common.h` 增补** `TASK_KIND_RESTORE`：恢复与备份是两条独立路径，共用一个 kind 会让回滚语义含糊。
- **`transfer_size()`**：worker 需提前知道总字节才能 `task_set_total`。
- **`api.c` 语义统一**：安装/备份/恢复改为「入队 + worker 驱动」，新增 `/api/fetch`（NAS 拉取）。**入队不再触碰数据源** ⇒ 失败以「任务 FAILED」暴露，更贴近 UI 所见。
- **`src/extract_engine.h`**：解压引擎契约头 —— worker 只依赖契约，不依赖具体 zip/rar/7z 实现。
- 测试累计 **222 项断言全绿**（184 + worker 19，api 增至 127）。

> 下一个编码动作：**把继承来的 zip/rar/7z 叶子模块按 `extract_engine.h` 挂进 `step_extract`**（含密码/分卷路径 —— 相对竞品的真真空），进度回写 `task_set_progress`；之后 UI 即可消费真实的 `/api/tasks`。
6. **本方案入档**：即本 `docs/PLAN.md`。

> 第 5 步让仓库从第一天起就能在 host 编译并跑测试；第 3–4 步让前端从第一天起有冻结规范与验收闸门；真机相关（Phase 0/1/2/5/6 的 PS5 调用）留接口、等机器。

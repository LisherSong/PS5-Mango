# 「PKG 不用 U 盘也能装」——原理取证

> 取证日期 **2026-09-30**。来源：`itsPLK/ps5-pkg-manager`（130★，**GPL-3.0**，最后推送 2026-09-27）。
> 目标是**只取事实、不搬代码**；结论全部可追溯到具体文件与行号。

---

## 0. 先说一件容易走错的事：链接指错了仓

用户给的链接是 **`itsPLK/ps5-payload-manager`**（371★，GPL-3.0），它是一个 **payload 管理器**，
**不含 PKG 安装功能**：

- 全仓（C / JS / JSX / MD）grep `.pkg` / `ffpkg` / `fpkg` / `InstallByPackage` ⇒ **0 命中**
- 唯一碰 `sceAppInstUtil` 的地方是 `src/app_installer.c` 的**主屏注册**：
  `sceAppInstUtilAppInstallTitleDir(title_id, "/user/app/", 0)`，NID `Wudg3Xe3heE`，
  经 `kernel_dynlib_handle(-1, "libSceAppInstUtil.sprx", &handle)` + `kernel_dynlib_resolve(-1, handle, nid)` 取址，
  再把 `param.json` + `icon0.png` 写进 `/user/app/PLDM00001/sce_sys/`
- 这**与我们已有的 `app_installer.c` 是同一条路**（同一个 NID、同一个 `kernel_dynlib_*` 取址方式）

**真正实现「不用 U 盘装 PKG」的是同作者的 `itsPLK/ps5-pkg-manager`。**

> 教训：**认符号，不认仓库名**。判断「某项目是否能装 PKG」只需 grep 一个符号
> `sceAppInstUtilInstallByPackage` —— 它是任何 PKG 安装器的必经入口，比读 README 可靠得多。

---

## 1. 核心原理（一句话）

> **`sceAppInstUtilInstallByPackage()` 的 `uri` 字段接受 `http://` URL。**
> 于是主机起一个**只监听 `127.0.0.1` 的 HTTP 服务**，把自己的 PKG 喂给**自己的系统安装器**。

```
http://127.0.0.1:18841/stream/install/package-<unixtime>-<seq>.pkg
```

**「PKG 必须放在 U 盘」只是 PS5 自带那个安装界面（设置 → 存储 → 安装 PKG）的限制，
不是内核限制、也不是 API 限制。** 绕过它不需要任何新的内核能力 —— 只需要换一个 `uri`。

---

## 2. 证据链

### 2.1 `uri` 的取值分支（`src/installer.c:1183-1206`、`:972-978`）

```c
int is_filesystem_install = 0;
int can_fallback_to_direct = is_usb_or_disc_path(worker_pkg_path) && !worker_is_multipart;
if (can_fallback_to_direct && !installer_is_network_connected()) {
    install_log("Network not connected; using direct storage install");
    is_filesystem_install = 1;      /* ← 只有在「离线 + USB/光盘」时才走文件路径 */
}
...
if (is_filesystem_install) {
    snprintf(target_uri, sizeof(target_uri), "%s", worker_pkg_path);        /* 直接路径 */
} else {
    snprintf(target_uri, sizeof(target_uri),
             "http://127.0.0.1:%d/stream/install/package-%lu-%u.pkg",
             STREAM_SERVER_PORT, (unsigned long)time(NULL), ++s_stream_seq); /* ← 默认走这里 */
}
```

**要点：默认所有来源都走 HTTP**（USB、光盘、SMB、浏览器上传），**文件路径只是离线回退**。
⇒ **`/data` 等内部盘上的 PKG 走文件路径注定失败，必须走 HTTP 推流。**

### 2.2 传给系统的元数据（`src/install_helper.c:149-159`）

```c
metadata = (pkg_metadata_t){
    install_request.uri,     /* uri                 ← 上面那个 http://127.0.0.1:... */
    "",                      /* ex_uri              ← 空串，不是 NULL */
    "",                      /* playgo_scenario_id  ← 空串 */
    "",                      /* content_id          ← 空串 */
    install_request.name,    /* content_name */
    install_request.icon_url /* icon_url            ← 也是 http://127.0.0.1:... */
};
response.result = sceAppInstUtilInstallByPackage(&metadata, &info, &playgo);
```

### 2.3 结构性事实（`include/install_appinst.h`）

```c
typedef struct {                 /* 6 个指针 = 48 B = 0x30 */
    const char *uri, *ex_uri, *playgo_scenario_id, *content_id, *content_name, *icon_url;
} pkg_metadata_t;

typedef struct { char content_id[48]; int type; int platform; } pkg_info_t;

typedef struct {                 /* ≈9984 B */
    char languages[30][8]; char playgo_scenario_ids[64][3];
    char content_ids[64][48]; unsigned char unknown[6480];
} playgo_info_t;
```

> 本仓此前记录的「**etaHEN / websrv / singleDPI 三方一致 = 6 字段 `0x30`**」，
> 到这里是**第四个独立来源**，并且首次给出了**字段名与顺序**。

---

## 3. 三个让这条路「能跑通」的关键设计

### 3.1 必须在**独立 helper 进程**里调

系统调用会**挂死**。参考实现的做法（`src/install_process.c`）：

1. 主进程开一个 loopback 监听口（`:18843`，fd 固定 `198`「避开 libc 启动自用描述符」）
2. 连 **elfldr `127.0.0.1:9021`**，把 `.incbin` 内嵌的 helper ELF 推过去
3. helper 被 elfldr 加载后**回连**主进程的监听口
4. 主进程通过这个 socket 发指令；**卡住就 `SIGKILL` 掉整个 helper，再 spawn 一个新的**

作者原话（`install_service.c:161`）：**"A system service call may be stuck; never reuse that process."**

另两条自设纪律：
- **「失败的调用也消耗掉这个进程」** —— `used = 1` 在调用**之前**置位（`install_helper.c:147`）
- **参数结构体必须一直存活**，注释原文：
  *"Keep every native argument alive and unchanged while the system may still refer to it,
  including after subsequent status requests."* ⇒ **不能放栈上临时量、不能提前 free**

调用序列：`Initialize()` → `InstallByPackage` →（循环）`GetInstallStatus(content_id)` → `Terminate()`。

### 3.2 系统安装器是个 **HTTP range 客户端**

从真实抓包逆向出来的三阶段（`tests/ps5_sim.h`，这是权威规格）：

| 阶段 | 行为 | 我们这边要满足 |
|---|---|---|
| ① header | 一串**短连接**，每条重复读**前 64 KiB**（`Range: bytes=0-65535`），观察 2–7 条连接 × 1–2 请求 | 支持 `Range`，且对同一区间可重复响应 |
| ② sidecar CRC | 一次**无 Range** 的 GET `<content_id>.crc` | **必须 404**。🪤 这里回 `206` PKG 字节会**毒化分片校验并中止安装** |
| ③ bulk | **2 条并行长连接**，服务 `[65536, end)` 的**连续 16 MiB range**，A/B 乒乓 | `206 Partial Content` + keep-alive |

- 每个请求都带 console 追加的查询串：`?product=0287&serverIpAddr=127.0.0.1&r=00000000`
  ⇒ **路由必须先剥 `?` 再匹配**
- 🪤 **URI 必须每次唯一**（时间戳 + 自增序号）：*"the system remembers recently used stream URLs across
  payload restarts (reusing package-1.pkg after a redeploy gets rejected)"*
- 🪤 离线直装时 `icon_url` 必须传 `""`，否则一样撞 PlayGo 网络错 **`0x80B21121`**

### 3.3 进度不靠猜，靠官方查询接口

安装跑在系统后台，**关掉浏览器也继续**。进度来自 `sceAppInstUtilGetInstallStatus(content_id, &status)`：

```c
typedef struct {
    char status[16]; char src_type[8];
    uint32_t remain_time;
    uint64_t downloaded_size, initial_chunk_size, total_size;
    uint32_t promote_progress;
    struct { int32_t error_code, version; char description[512]; char type[9]; } error_info;
    int32_t local_copy_percent; bool is_copy_only;
} SceAppInstallStatusInstalled;
```

---

## 4. 对我们（`ps5-nexus`）意味着什么

### 4.1 ⚠️ 本仓当时这段代码**从未可能工作过** —— 签名写错了（**已修复**）

> **状态（2026-09-30 同日修复）**：签名、`Initialize/Terminate` 包裹、loopback
> 流服务、真正的进度查询**均已落地**；见 §7。本节保留原始取证，因为这是
> `_Static_assert` 在本项目里**第二次**骗过我们的记录。

```c
/* src/pkg_installer.c:39 —— 现在的写法 */
extern int sceAppInstUtilInstallByPackage(const char *pkg_path,
                                          const void *meta, void *reserved);
...
sceAppInstUtilInstallByPackage(pkg_path, &h->meta, NULL);   /* :128 */
```

```c
/* 真签名 */
int sceAppInstUtilInstallByPackage(const pkg_metadata_t *meta,
                                   pkg_info_t *info,
                                   playgo_info_t *playgo);
```

**首参是元数据结构体，不是路径。** 现在等于把「路径字符串」当地址交给系统去解引用 6 个指针
⇒ **必失败，最坏情况崩**。

🪤 注意：`_Static_assert(sizeof(pkg_metainfo_t) == 0x30)` **查不出这个错误** —— 结构体尺寸是对的
（我们早就收回了 `0x30`），**错的是它在参数表里的位置被整个弄反了**。
`_Static_assert` 只证明「自己和自己一致」，这是它第二次在本项目里骗过我们。

另有配套缺口：
- 没有 `sceAppInstUtilInitialize()` / `Terminate()` 包裹
- 工作台没有 `sceAppInstUtilGetInstallStatus`，`pkg_install_poll()` 现在返回的是**桩值**（`pkg_installer.c:145-149` 自己标了 TODO）
- 没有 loopback HTTP stream 服务 ⇒ 即使签名修对，`/data` 里的 PKG 仍装不上

### 4.2 四层拆解：哪些是硬条件、哪些我们已有

| 层 | 内容 | 归属 | 我们的状态 |
|---|---|---|---|
| (a) **API** | `sceAppInstUtilInstallByPackage` + `Initialize/Terminate/GetInstallStatus` | Sony `libSceAppInstUtil.sprx` | ✅ 已在直调（**签名要修**） |
| (b) **权限** | `kernel_set_ucred_authid(0x4800000000000006)` | 我们自己 | ✅ 已有 |
| (c) **数据格式** | `pkg_metadata_t` 6 指针 / range 三阶段 | 逆向可得 | ⚠️ 结构体对了，**调用位置错了** |
| (d) **宿主前提** | kstuff（签名/DRC 绕过） | **叶子载荷 = kstuff** | ✅ 已有判定逻辑 |

⇒ **四层里三层已就绪**，缺的是 (c) 的落地：修签名 + 加 loopback stream 服务 + 接真正的进度查询。
**不需要任何我们不掌握的内核能力。**

### 4.3 许可

`ps5-pkg-manager` 是 **GPL-3.0** ⇒ **代码可合法取用，需署名**。本仓自有代码 + 全链 GPL-3.0，兼容。

---

## 5. 可离线复用的验收夹具（比抄代码更值钱）

参考实现自带一套**不需要真机**的模拟器：

| 文件 | 作用 |
|---|---|
| `tests/ps5_sim.{c,h}` | **PS5 安装器的纯客户端模型**（三阶段协议 + 查询串 + 参数） |
| `tests/test_stream_sim.c` | 把上面的模型**对着真的 `stream_server.c`** 跑端到端断言 |
| `tools/ps5_installer_sim.c` | 命令行版，可指向任何活着的 server |
| `tools/ws_push_sim.c` | WebSocket 推流模拟（浏览器上传那条路） |

⇒ 我们做 stream server 时，**可以先用这套形状自建夹具**，在 PC 上把 range 逻辑压到确定性通过，
再上真机 —— 这正好补上本项目「Phase 0 没真机」的短板。

---

## 6. 附：走这条路能解锁的三种「无 U 盘」安装

| 来源 | 路径 | 备注 |
|---|---|---|
| **内部盘 / `/data`** | `pread()` → range → `127.0.0.1` | 本仓场景最直接的一条 |
| **SMB / NAS 共享** | `smb2_read()` → range → `127.0.0.1` | 🪤 原生安装器**读不了 SMB**，必须由我们桥接 |
| **浏览器直推（Direct Install）** | PC 浏览器 WebSocket 上传 → **RAM 会话** → range → `127.0.0.1` | 「No Duplicate Storage Needed」指的就是这条：**不落盘副本** |

第三条与本项目「大文件吞吐 + 续传」的真空正好重叠，值得单独立项。

---

## 7. 落地记录（2026-09-30）

### 7.1 改了什么

| 文件 | 内容 |
|---|---|
| `src/pkg_stream.{h,c}` | **新增**。只监听 `127.0.0.1` 的 HTTP/1.1 range 服务；keep-alive、`206`+`Content-Range`、未登记路径一律 `404`（含 `.crc` 探测）、单例发布、非阻塞、由主循环泵送。跨平台（MinGW winsock / POSIX）⇒ **可在 PC 上确定性验收** |
| `src/pkg_installer.c` | 重写。真签名 `(&metadata, &info, &playgo)`；`Initialize/Terminate` 包裹；`uri` = `http://127.0.0.1:<port>/stream/install/<session>.pkg`；进度改由 `sceAppInstUtilGetInstallStatus()` 提供 |
| `src/worker.c` | `worker_tick()` 泵送流服务；`step_install()` 在“仍在进行”时返回 0（否则安装期间主循环 100% 空转）；失败/跳过的原因通过 `task_set_note` 透出 |
| `tests/test_pkg_stream.c` | **新增**，89 项断言，三段协议端到端（无真机） |
| `tests/run-orchestration-tests.sh` | 接入新套件；并修好了**此前已红**的编排套件（见 7.3） |

### 7.2 关键设计选择（与参考实现不同，且有理由）

- **单进程内直调**，不用 helper 进程。参考实现用独立 helper 是因为“卡住的系统调用会报销整个进程”。其他能用的自建安装器（singleDPI 系）都在本进程内直调，我们跟随后者。**代价**：真机上若该系统调用挂死，整个 payload 一起挂 —— 这是本项目唯一一处「可能被系统调用带走」的地方，明确记录而不掩饰。
- **端口优先 18841，占用则回退临时端口**，且 `uri` 永远携带**实际绑定**的端口。参考实现写死端口；我们不做假设，但也不改变已知可用的默认值。
- **取消不调用未确认的终止符号**：只停止喂流（`unpublish`），让主机自己的安装器自行失败。参考实现的取消是 `SIGKILL` 掉 helper，我们没有 helper，无法等价实现 ⇒ 记为缺口而不是猜。
- **范围请求非法时回 `200` 全量，不回 `416`**：主机对 `416` 的反应未经证实，而「忽略 Range 并给全量」是 HTTP 允许的、且永远安全。

### 7.3 顺带修好的两处既有缺陷

1. **编排主机套件当时是红的**：`api.c` 后来新增的文件操作块用了 `lstat()` 与两参 `mkdir()`，MinGW 都没有 ⇒ `run-orchestration-tests.sh` 早已编不过。已用 `#ifdef _WIN32` 映射修好（`api.c` 顶部），并给 `tests/run-orchestration-tests.sh` 补上 `tests/compat`（`<sys/statvfs.h>` 垫片）与 `lws2_32` 的 Windows-only 分支。
2. **`app_install_if_needed` 没有任何桩**：`make linux` 与主机套件都会缺符号。已在 `src/stubs_nops5.c`（诚实返回 UNSUPPORTED）与 `tests/stubs_ps5.c`（可记录）各补一个。

### 7.4 已核实（本机可验的部分）

- 单 TU 编译：`pkg_installer / pkg_stream / worker / api / main / savemgr / app_installer` 在 **prospero-clang 18 + `-Werror`** 下全部通过。
- **符号可得性**：`$SDK/target/lib/libSceAppInstUtil.so`（108 个桩符号）**导出**
  `sceAppInstUtilInitialize` / `Terminate` / `InstallByPackage` / `GetInstallStatus`
  ⇒ 直连 `-lSceAppInstUtil` 可行，**不需要**走 `kernel_dynlib_resolve`。
- 主机套件：`pkg_stream` 89 项 + 全套 4 套件全绿。

### 7.5 真机待核（**Phase 0，一条都不能跳**）

| # | 待核项 | 为什么有疑问 |
|---|---|---|
| 1 | 系统安装器**接受非标准端口的 loopback uri** | 参考实现写死 18841；我们优先用它，但回退路径未验 |
| 2 | `sceAppInstUtilInstallByPackage` 是否**在本进程内返回**（不挂死） | 参考实现为此专门开了 helper 进程 |
| 3 | `appinst_status_t` 的**末尾字段偏移** | 只有头部（`status`/`downloaded_size`/`total_size`/`error_info`）被跨源证实；尾部是按参考实现复刻的，用于给系统留出写入空间 |
| 4 | 真**取消**符号 | 未知，故未调用 |
| 5 | **局域网离线时是否仍报 `0x80B21121`** | 参考实现遇到它会回退到“直连存储”；`/data` 与 SMB **没有**这个回退，只能失败 |
| 6 | `GetInstallStatus` 的轮询频率上限 | 我们节流到 300 ms，是估计值 |
| 7 | 装完后 `unpublish` 的时机 | 早于主机读完最后一块会让安装失败 |


# 竞品源码级核实（Competitor Audit）

> 目的：把 PLAN §0 / HANDOVER 里「竞品有无 SMB/解压」的**待核实项**变成源码级确定结论。
> 方法：**读源码**（gh api 拉 blob / tree），不依赖发布说明、视频、论坛帖。
> 核实日期：2026-09-27。核实人：本会话（Senior Developer 工作流）。

---

## 0. 结论速览（先看这条）

| 能力 | VoidShell 3.0B | elf-arsenal（开源 clone） | 对我们定位的影响 |
| --- | --- | --- | --- |
| **SMB 互通** | ❌ 无（闭源；走 FTP + ShadowMount） | ✅ **有，基于 libsmb2**（作者即 libsmb2 上游 John Törnblom，GPL-3.0） | SMB 不再是「真空」，但**有开源 libsmb2 可直接 vendored + 参考封装**，自研风险下降 |
| **NFS 互通** | ❌ 无 | ❌ 无（只有 SMB + FTP） | **仍真空**（我们 libnfs 备选路线成立） |
| **ZIP 解压** | ❓ 闭源，未知 | ✅ 有，但**仅 miniz（ZIP/DEFLATE），无 ZipCrypto/AES 加密、无分卷** | 我们引擎覆盖 ZIP(含加密) + RAR + 7z(含加密头+分卷)，明显更宽 |
| **RAR 解压** | ❓ 闭源，未知 | ❌ 完全没有 rar | **真真空**（我们的差异化） |
| **7z 解压** | ❓ 闭源，未知 | ❌ 完全没有 7z | **真真空**（我们的差异化） |
| **传输主路径** | FTP（报道：FileZilla/WinSCP） | FTP（自带 ftpsrv-src，Makefile.ps5/ps4） | FTP 续传弱；我们 SMB + 续传是差异 |
| **FFPKG 落地** | ShadowMount | ShadowMountPlus（vendored 子模块） | 与我们的 FFPKG 通道（dump2ufs + ShadowMount）方向一致 |
| **金手指** | 有（mc4/shn/json，auto-inject） | 有（cheats，garlic-worker 系） | 本项目**不做**（Session 19 拍板，只做文件管理） |
| **许可** | **闭源**；有内置过期日；社区称已被作者放弃 | 开源（GPL-3.0，聚合 Sonic Loader 后继 + 多个 GPL/MIT 叶子） | 见 §3 许可护栏 |

**一句话**：之前方案里把「SMB/NFS NAS 互通」列为真空之一是**不准确的**——elf-arsenal 已有正牌 libsmb2 SMB 客户端。但 **RAR/7z 解压 + 加密 + 分卷** 仍是 elf-arsenal 与 VoidShell（闭源）都没覆盖的真真空；**NFS** 也仍是真空。我们的差异化应聚焦于：完整解压引擎（RAR/7z/加密/分卷）+ 大文件吞吐与续传 + NFS 备选。

---

## 1. VoidShell 3.0B —— 闭源，无法源码级核实

- **分发渠道**：ko-fi（`ko-fi.com/s/99aac5c463` 等）、mediafire。**不是 GitHub 开源仓库**（`gh search repos "VoidShell"` 命中的全是名字相似的无关项目：Voidshells、voidshell、VOIDSHELL…）。
- **作者**：VoidWhisper（社区署名）。
- **状态（社区反馈，gbatemp 2026-04）**：「beta、有 bug、**已被作者放弃（abandoned）**、闭源意味着无后续开发/修复、**内置过期日期（date of expiry）**」；etaHEN + VoidShell 组合在某些配置下已知会导致 kernel panic。
- **公开功能（发布说明 / 报道）**：双窗文件管理器、网络 PKG 安装器、payload 管理器、ShadowMount（FFPKG 挂载）、温度/运行时间/库统计、Sentinel Warden（自动 kstuff 安全处理）、cheats（mc4/shn/json）、app dumper、**FTP** 传输。
- **SMB/NFS**：发布说明层面**无 SMB/NFS**，走 FTP + ShadowMount。
- **核实结论**：**无法源码级核实（闭源）**。按「依赖记叶子、不记打包者 / 界面文档不出现第三方打包发行版名字」铁律，VoidShell 的闭源代码**一行不可借鉴**，且它本身是已放弃的闭源打包品，不构成我们生态的复用来源。仅作为「市场已有形态」参考，确认「另一个文件管理器 + 安装器」赛道拥挤、但闭源品不作为技术参照。

---

## 2. elf-arsenal —— 开源，已完成源码级核实

- **仓库**：`aloksaurabh/elf-arsenal`（自述「Clone 05Jun2026」，Sonic Loader 后继 rebrand）。`bsk193/elf-arsenal-mirror` 仅含 CI 镜像工作流，无源码。
- **许可**：GPL-3.0（聚合仓库，内含多个叶子依赖，各带自身许可）。
- **Web UI 端口**：`:6969`（PLAN 旧注写的 `:9021` 有误，以源码 README 为准）。
- **主要 vendored 子模块**：
  - `ftpsrv-src/` —— FTP 服务器（Makefile.ps5 / Makefile.ps4），**传输主路径是 FTP**。
  - `ShadowMountPlus-main/` —— FFPKG 挂载（与我们的 FFPKG 通道方向一致）。
  - `garlic-worker-src/` —— 存档域 worker（garlic-savemgr 系，GPL-3.0，**我们可合法复用，需署名**）。

### 2.1 SMB 客户端（✅ 实锤，基于 libsmb2）

`src/smb.c` 头部与 include：

```c
/* Copyright (C) 2025 John Törnblom   — 即 libsmb2 上游作者 */
#include <smb2/smb2.h>
#include <smb2/libsmb2.h>
#include <smb2/libsmb2-raw.h>
```

- 通过 MHD Web 服务器暴露 `smb_request_shares_args`（列共享）、`smb_request_dir_args`（列目录）、`smb_request_file_args`（取文件）—— 是**完整的 libsmb2 客户端封装**。
- 许可：GPL-3.0（John Törnblom 以 GPL-3.0 发布该封装）。libsmb2 本体是 **GPL-2.0-or-later**（与 GPL-3.0 兼容，可 vendored）。
- **对我们的意义**：SMB 互通**不是从零**。我们 Phase 1 计划同样用 libsmb2（`transfer.c` + `smb.c`），可直接 vendored 正牌 libsmb2 + 借鉴 elf-arsenal 的 MHD 集成封装技术事实（技术事实无版权；若直接取代码须 GPL-3.0 署名）。这**降低 SMB 自研风险**。

### 2.2 ZIP 解压（✅ 有，但范围窄）

`src/zipread.c`：

```c
#include "miniz.h"          // 单文件 ZIP 库 miniz
ezip_open_mem / ezip_open_file
ezip_extract_heap(...)      // mz_zip_reader_extract_to_heap
ezip_extract_iter_(...)     // mz_zip_reader_extract_iter_*
```

- 基于 **miniz**（仅支持 ZIP/DEFLATE/store）。
- **不支持**：ZIP ZipCrypto、WinZip AES-256、分卷 ZIP（`.z01`+`.zip`）、RAR、7z。
- 是**真正的完整 ZIP 解压**（能解压到 heap / 迭代流式读），但格式覆盖极窄。

### 2.3 RAR / 7z（❌ 完全没有）

全树 385 文件，grep `rar|7z|lzma|unrar` 命中为空（除构建产物外）。**elf-arsenal 不提供任何 RAR/7z 解压**。

### 2.4 NFS（❌ 无）

全树无 `nfs`/`libnfs` 引用。传输只 SMB + FTP。

**elf-arsenal 源码核实结论**：SMB（libsmb2）✅ / ZIP-miniz ✅ / RAR ❌ / 7z ❌ / NFS ❌ / FTP（主）✅ / FFPKG(ShadowMountPlus) ✅ / cheats+savemgr(garlic-worker) ✅。

---

## 3. 许可护栏（更新 PLAN §6 / HANDOVER）

- **libsmb2**：GPL-2.0-or-later，**可 vendored 为正牌依赖**（叶子依赖，记叶子）。
- **elf-arsenal 的 `src/smb.c` 封装层**：GPL-3.0，若直接取代码须保留版权头 + NOTICE 署名；**更稳妥是只借鉴技术事实（MHD×libsmb2 集成方式），自研等价 `smb.c`**，避免聚合第三方打包品的耦合。
- **garlic-worker-src**：GPL-3.0（garlic-savemgr 系），存档域**可合法复用**，需署名。
- **VoidShell**：闭源、已放弃、有内置过期日 ⇒ **不借鉴、不出现其名**。
- **铁律重申**：本项目**界面/文档不出现任何第三方打包发行版名字**（elf-arsenal / VoidShell / etaHEN / kstuff-lite 等皆不出现）；依赖记叶子（libsmb2 / miniz / garlic-worker / unrar7 / minizip-ng / LZMA SDK），不记打包者。

---

## 4. 对方案定位的修正（需同步回 PLAN）

1. PLAN §0「真空」清单里「NAS 互通（SMB/NFS 拉取）」需细分：
   - **SMB 互通**：有开源 libsmb2 参考 ⇒ 风险降级，但要做「更稳 / 带断点续传 / 吞吐优化」才构成差异。
   - **NFS 互通**：elf-arsenal 无 ⇒ 仍真空（libnfs 备选成立）。
2. **解压引擎差异化收敛到 RAR/7z（含加密头 + 分卷）**——这是 elf-arsenal 与闭源 VoidShell 都没覆盖的真真空。
3. **大文件吞吐 + 续传**：FTP 续传能力弱（elf-arsenal 主路径 FTP）⇒ 我们 SMB + 续传 + 跨重启是实质差异。

> 注：本核实不改变「只依赖 kstuff、不绑 etaHEN」的安装层原则，也不改变四能力域结构；仅校准竞争差异点。

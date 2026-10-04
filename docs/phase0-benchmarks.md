# Phase 0 测量方案与预检清单（本机半，真机到位照着跑）

> 角色：Phase 0 真机基线的「本机半」。本机现在做测量脚本 + 预检清单 + 竞品核实（已完成，见 `competitor-audit.md`）；**真机到位再跑 4 项判定实验**。
> 本机半**不阻塞任何后端 Phase**（传输/安装/吞吐/存档都能先写接口占位 + host 测试）。
> 配套脚本：`misc/bench_extract.c`（PS5 侧计时壳）、`misc/bench_fixtures.py`（本机夹具生成 + 计时原型，验证方法论）。

---

## 0. 速度口径（先读，贯穿所有计时实验）

**定义**：速度 = 总字节 ÷ 总耗时（或 ≥10s 窗口）。**250ms 瞬时值只在内部采样，不对外显示**。

**夹具形状决定结论（已两次翻转的教训）**：
- ❌ **禁止**用 `/dev/urandom` / 90% 随机数据造夹具 —— 随机数据不可压缩，给解码器喂的是「全 miss」退化路径，测出的倍率（曾出现 2.55×）是**假结论**。
- ✅ **必须按真实压缩比造夹具**：用真实可压缩样本（游戏资源文本/XML/json、媒体、已压缩过的素材包）或已知压缩比的混合数据。**真实压缩比下解码器才走正常热点路径**。
- 测「解码吞吐」与测「网络/写盘吞吐」要分开变量：同形状夹具分别在有/无网络、有/无写盘下计时，才能定位瓶颈。

**三个要量出的数（Phase 0 的核心产出）**：
1. `T_net` —— NAS→PS5 实际拉取速度（SMB，带盘前提）。
2. `T_dec` —— 解压引擎解码吞吐（RAR/7z/ZIP 同形状）。
3. `T_copy` —— 写盘上限（内置 M.2 `/mnt/ext1` vs exFAT USB `/mnt/usb0`）。

> 量完三数才谈排序：若 `T_copy` 远小于 `T_net`，瓶颈在写盘（吞吐优化无效，优先续传/异步落盘）；若 `T_dec` 是墙，再谈 RAR_SMP 多线程（条件：证明确实不在写盘）。

---

## 1. 实验① singleDPI 三件套 + 装 fPKG（判定「只靠 kstuff 就够」）

**目标**：确认在测试机 FW 12.70 上，跑现成 `singleDPI.elf` 能 ping 通三件套并成功装一个 PS4 fPKG —— 若成，「只依赖 kstuff、不绑 etaHEN」成立。

**前置（本机半已备）**：
- 下载 `singleDPI.elf`（来源：maxMilu/ps5-direct-package-installer，GPL-3.0，可合法取用，须署名）。
- 准备一个已知可装的 PS4 fPKG（CUSA 小体积，<2GB 便于计时）。

**步骤**：
1. 注入 kstuff（本机确认 `cat /proc/kstuff` 存在、autoload.txt 含 kstuff、无 `no_kstuff` 哨兵：`/data/etaHEN/` 与 `/user/data/etaHEN/` 检查）。
2. 发 `{"action":"ping"}` 三件套：预期返回 kstuff 就绪 / 安装服务在线 / 已装清单可拉。
3. 触发安装该 fPKG，轮询状态到完成。
4. PS5 主屏确认该游戏出现并可启动。

**判读**：装成 + 主屏可见 ⇒ 「只靠 kstuff 就够」**成立**（etaHEN 不再是安装前提）。

**就绪三件套探测（本机即可写的预检脚本骨架）**：见 `misc/` 后续 `preflight_dpi.sh`（待补，列出 curl 三件套端点 + kstuff 哨兵检查）。

---

## 2. 实验② AuthID 提权链自测

**目标**：确认两套 AuthID 各自能拿到对应特权，且**分开封装、互不污染**。

**前置**：本仓已定 AuthID 常量（代码为准）：
- 安装 = `0x4800000000000006`（DEBUG_AUTHID）
- 存档 = `0x4800000000000010`

**步骤**（每套独立跑）：
1. `set_ucred_authid(<X>)` → `get_ucred_caps` + `caps[7] |= 0x40` → `set_ucred_caps` → `setuid(0)`。
2. 验证该 AuthID 能完成其域的最小特权操作（安装：调 `sceAppInstUtilInstallByPackage`；存档：开 `/dev/pfsmgr` ioctl）。
3. 确认另两套 AuthID 在当前上下文**不可**越权（隔离性）。

**判读**：两套各自最小特权达成 + 隔离 ⇒ 提权链封装正确。

**关联坑**：本仓 `Makefile` **未链 `-lkernel_sys`** ⇒ 现有安装路径缺 `kernel_set_ucred_authid` 这道，很可能从未真机验证。本实验必须连带验证 `-lkernel_sys` 链接后能否真调。

---

## 3. 实验③ 同形状解压对照（单大文件 RAR 计时）

**目标**：量出 `T_dec`（解压吞吐），确认瓶颈是否在解码。

**前置**：`misc/bench_fixtures.py` 生成的**真实压缩比** RAR 夹具（见 §0 口径）；`misc/bench_extract.c` 计时壳（PS5 侧，须 SDK 编）。

**步骤**：
1. 用 `bench_fixtures.py` 造三个同形状夹具：纯可压缩文本包 / 媒体混合包 / 已压缩素材包（**禁用随机数据**）。
2. PS5 上 `bench_extract.c` 对每个夹具计时解压（总字节 ÷ 总耗时），采样 ≥10s 窗口。
3. 变量分离：同夹具分别在「只解码不落盘」「解码 + 写 `/mnt/ext1`」「解码 + 写 `/mnt/usb0`(exFAT)」计时。

**判读**：若「解码+写内置」≈「只解码」而 << 网络拉取 ⇒ 瓶颈在 `T_net`，吞吐优化优先级降；若「解码+写」>>「只解码」⇒ 写盘是墙。RAR_SMP 多线程**仅在证明确实不在写盘**时才开。

**本机原型已验证方法论**：`misc/bench_fixtures.py` 能在 MinGW 环境下生成同形状夹具并跑本机解压计时（复用已搬引擎 + tests fixtures），真机到了只换路径即可出 `T_dec`。

---

## 4. 实验④ 竞品功能核实（✅ 本机半已完成）

见 `competitor-audit.md`：**elf-arsenal 有 libsmb2 SMB + miniz ZIP、无 RAR/7z/NFS；VoidShell 闭源无法核实**。结论已回写 PLAN §0 真空清单。

---

## 5. 附加测量（写盘上限 + 拉取实测）

- **写盘上限 `T_copy`**：用 `dd` 类大块写 `/mnt/ext1/data`（内置 M.2）vs `/mnt/usb0`（exFAT 外置），分别量顺序写速。**三者写速完全不同，谈速度必须带盘前提**（内置 / exFAT USB / 网络）。
- **libsmb2 拉取实测 `T_net`**：从测试 NAS 拉一个真实大文件到 `/mnt/ext1`，量 SMB 实际速度（带盘前提）。对比 Wi-Fi 基数 30–40 MB/s（有线未测）。
- **挂载点前提备忘**：`/mnt/ext1`=内置 M.2 扩展盘，`/mnt/usb0|1`=外置 USB，`/data`=内置 PFS。谈速度必带挂载点。

---

## 6. 预检清单（真机到位前，本机逐项打勾）

- [ ] 测试机 FW 确认 = 12.70（或记录实际版本）。
- [ ] `singleDPI.elf` 已下载 + 已知可装 PS4 fPKG 就位。
- [ ] kstuff 注入路径确认（`/proc/kstuff` + autoload.txt + 哨兵检查脚本）。
- [ ] `misc/bench_extract.c` 在 SDK 机 `make` 通过（须 `-lkernel_sys` 链接验证）。
- [ ] `misc/bench_fixtures.py` 本机生成同形状夹具 OK（已可跑）。
- [ ] `T_copy` / `T_net` 测量脚本就位（dd 类 + smb 拉取计时）。
- [ ] 两套 AuthID 常量填入对应 `.h`。
- [ ] 速度口径卡片（§0）已读，禁止随机数据夹具。

---

## 7. 交付物清单（本机半）

| 文件 | 作用 | 状态 |
| --- | --- | --- |
| `docs/competitor-audit.md` | 竞品源码级核实（4 项结论） | ✅ 已完成 |
| `docs/phase0-benchmarks.md` | 本文件：测量方案 + 预检清单 + 速度口径 | ✅ 本机半 |
| `misc/bench_extract.c` | PS5 侧同形状解压计时壳（须 SDK 编） | 见 Task 87 |
| `misc/bench_fixtures.py` | 本机夹具生成 + 计时原型（验证方法论） | 见 Task 87 |
| `misc/preflight_dpi.sh` | singleDPI 三件套 + kstuff 哨兵预检骨架 | TODO（下个增量） |

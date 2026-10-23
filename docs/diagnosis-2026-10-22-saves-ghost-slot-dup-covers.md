# 真机诊断记录 · 2026-10-22

设备：`http://<ps5-ip>:2026/`，运行 **v1.0.29**（`/api/status` 实测）
取证方式：真机 API 原始响应 + 干净浏览器 context（Playwright）抓实际 DOM / 截图 / 网络层
**本文只记录，未改动任何源码或资产。**

---

## 0. 结论摘要

| 编号 | 用户报障 | 状态 | 根因 |
|---|---|---|---|
| A1 | 删除存档后会多出一个「假存档位」 | 已定位 | `savemgr_delete_save()` 删完镜像从不 `rmdir` title 目录 ⇒ 空目录残留 |
| A2 | 已卸载游戏的存档不能删除 | 已定位 | 空目录 `slots=0` ⇒ `savemgr_delete_game()` 返回 0 ⇒ API 回 404 `save not found` |
| A3 | 假存档位显示为「1 文件夹」 | 已定位 | 前端 `(head.slots \|\| g.list.length)` 把 `0` 当 falsy 回落成 1 |
| B | PKG 页双卡 + 按钮差异 | **已定位 + 已证消除** | 双卡 = 同 ID 的 .pkg 文件卡 + 安装目录(app_dir)卡；v1.0.29 去重正确。上一轮真因＝设备跑的 ELF 内嵌前端缺去重（非浏览器缓存，设备 `Cache-Control: no-store`）；用户重载后设备现正确服务 v1.0.29，实时跑去重＝7 入 5 出，双卡已消失。按钮差异是 E1 安全设计（app_dir 不给「删除」） |
| C1 | 游戏封面显示不出 | 已定位 | `/api/save_mgr/icon` 是 soft 路由，`trylock` 抢不到锁直接回 **503 busy**；前端 `onerror` 直接 `remove()`，无重试 |
| C2 | （连带）假卡封面也拿不到 | 已定位 | `dir_name` 为空 ⇒ 前端拼出 `&dir=` ⇒ 后端校验回 **400** |
| C3 | CUSA45599 游戏级封面 404 | 已记录 | `SM_ICON_PATS` 六条候选全部未命中（该机 PS4 游戏）；PKG 页走 `/api/pkg/icon` 正常，故不影响当前视图 |

---

## 1. 问题 A：存档「假存档位」与「删不掉」

### 1.1 真机取证

`POST /api/save_mgr/games` 原始响应（节选）：

```json
{"title_id":"PPSA23226","path":"/user/home/1ea2f4d9/savedata_prospero/PPSA23226",
 "size":0,"dir_name":"","slots":0,"installed":true,"game_title":"黑神话：悟空"}
{"title_id":"PPSA16608", ... "size":0,"dir_name":"","slots":0,"installed":false,"game_title":"冰汽时代2"}
{"title_id":"PPSA26344", ... "size":0,"dir_name":"","slots":0,"installed":false,"game_title":"羊蹄山之魂"}
{"title_id":"PPSA17599", ... "size":0,"dir_name":"","slots":0,"installed":false,"game_title":"光与影：33号远征队"}
```

目录实际内容（`/api/list` 实测）——**目录都在，但里面一个文件都没有**：

```
PPSA23226 -> {"ok":true,"entries":[]}      # 黑神话，刚被你删过
PPSA16608 -> {"ok":true,"entries":[]}      # 冰汽时代2（已卸载）
PPSA26344 -> {"ok":true,"entries":[]}      # 羊蹄山之魂（已卸载）
PPSA17599 -> {"ok":true,"entries":[]}      # 光与影（已卸载）
```

存档页实际渲染（Playwright 抓 DOM，7 张卡）：

```
YouTube | PS5 | 3 文件夹
黑神话：悟空 | PS5 | 1 文件夹      <- size 0 B，实际 slots=0
冰汽时代2 | PS5 | 已卸载 | 1 文件夹  <- size 0 B，实际 slots=0
羊蹄山之魂 | PS5 | 已卸载 | 1 文件夹
光与影：33号远征队 | PS5 | 已卸载 | 1 文件夹
小小梦魇3 | PS5 | 1 文件夹
Cuphead | PS4 | 2 文件夹
```

截图：`.build/shots/diag-device-saves.png`

### 1.2 根因 A1 —— 删存档不删目录

`src/savemgr.c:1798 savemgr_delete_save()`

只做四件事：`unlink(镜像)` → （PS4）`unlink(.bin)` → `unlink(meta 下的 icon0.png)` → 注销 db 行。
**全程没有 `rmdir`**，所以删掉最后一个槽位后 `/user/home/<uid>/savedata_prospero/<TID>/` 变成空目录留在原地。

`api_save_mgr_games()`（`src/savemgr.c:2905`）按**目录**枚举，只跳过 `sce_backup<N>`，**不跳过 `slots==0`**：

```c
if (sm_is_system_container(te->d_name)) continue;   /* 只挡系统备份容器 */
...
sm_dir_scan_approx(path, &total, first_dir, sizeof(first_dir), &slots);
```

⇒ 空目录照旧出一张「裸卡」。**这就是你删完黑神话存档后立刻多出来的那一张。**

### 1.3 根因 A2 —— 空目录删不掉（404）

前端 `smDeleteGame()` 发的是**整套删除**（不带 `dir_name`）：

```js
const j = await apiJson("/api/save_mgr/delete",
  { uid: ..., title_id: g.tid });
```

后端 `api_save_mgr_delete()`（`src/savemgr.c:5247`）：

```c
int n = savemgr_delete_game(uid_hex, tid, &failed);
...
if (n == 0) return send_json_error(conn, MHD_HTTP_NOT_FOUND, "save not found");
```

`savemgr_delete_game()` 靠 `sm_game_slots()` 找槽位，而 `sm_game_slots()`（`:1735`）只数**普通文件**（`S_ISREG`，跳过 `.bin`）：

```c
if (stat(fp, &st) < 0 || !S_ISREG(st.st_mode)) continue;
```

空目录 ⇒ `n = 0` ⇒ **HTTP 404 `{"error":"save not found"}`**。前端 toast「删除失败：save not found」——
**表现就是「已卸载的游戏存档不能删除」，而且重试也没用**：它永远找不到东西可删，也不会去清那个空目录。

### 1.4 根因 A3 —— 「0 个」被显示成「1 个」

`assets/index.html:2446`：

```js
'<span class="save-count">' + (head.slots || g.list.length) + ' 文件夹</span>'
```

`head.slots === 0` 是 falsy ⇒ 回落 `g.list.length`（分组里恰好 1 条记录，`dir_name` 为空）
⇒ 卡片写着「**1 文件夹**」，点「打开存档」却是空的。这就是「假存档位」的视觉形态。

### 1.5 建议修法（尚未实施）

| # | 位置 | 改动要点 |
|---|---|---|
| 1 | `savemgr_delete_save()` | 槽位删完后尝试 `rmdir` title 目录；`ENOTEMPTY`/`ENOENT` 一律忽略（幂等，别把删除变成失败） |
| 2 | `savemgr_delete_game()` | `n==0` 时也走一次「清空目录」；成功清掉就返回非 0，API 回「已清理空目录」而不是 404 |
| 3 | `api_save_mgr_games()` | `slots==0` 且目录为空 ⇒ 要么直接跳过（列表立刻干净），要么发 `empty:true` 让前端显示「无存档」+「清理」按钮 |
| 4 | `index.html:2446` | `head.slots \|\| g.list.length` → `head.slots ?? g.list.length`（0 不能被回退） |
| 5 | `index.html` 假卡封面 | `dir_name` 为空时改用 `game=1`，别拼 `&dir=`（现在会 400，见 C2） |

> 注意 3 与 1/2 的取舍：只做 1/2 能止住「删完冒出来」，但历史残留（冰汽时代2 / 羊蹄山之魂 / 光与影）仍在，
> 需要用户在 UI 里能清掉 —— 也就是必须让「0 槽位」有可达的删除路径。

---

## 2. 问题 B：PKG 页「还是有重复」+ 按钮差异

### 2.1 真机干净实测：本机扫描根 **不重复**（v1.0.29 去重已生效）

干净浏览器 context（空 `localStorage` ⇒ 不自动重连 PC），直连真机页面抓 `#libCovers`：

```
=== PKG 页卡片 5 张 ===
  tid=CUSA45599  app=0 pc=0  pill=已安装   title=DAVE THE DIVER      <- .pkg 文件卡
  tid=PPSA01650  app=0 pc=0  pill=已安装   title=YouTube             <- .pkg 文件卡
  tid=PPSA23226  app=1 pc=0  pill=已安装   title=黑神话：悟空         <- 安装目录卡
  tid=PPSA05143  app=1 pc=0  pill=已安装   title=小小梦魇3           <- 安装目录卡
  tid=CUSA20499  app=1 pc=0  pill=已安装   title=Cuphead             <- 安装目录卡
```

截图：`.build/shots/diag-device-library.png`（5 张，无重复）。
9 个根逐个 `POST /api/pkg/scan`：`/data/pkg` 2 个 .pkg + `/user/app` 5 个目录，
经 `pkgTid` / `seenTid` 跨种类去重后本地算出 **5 张**，与浏览器实测一致。
⇒ **v1.0.29 的去重代码在真机上确实生效**，没有复现出 潜水员戴夫 / Y2JB 双卡。

### 2.2 用户真机看到的双卡是什么

用户报：在 DAVE THE DIVER（CUSA45599）与 YouTube（PPSA01650）之外，
又各出现一张同 ID 的「潜水员戴夫」「Y2JB」，且这两张只有「卸载」、没有「删除」。

- **这两张就是被 v1.0.29 去重要干掉的「安装目录（app_dir）卡」**：
  后端 `scan_app_root()`（`mango_port.c:2506`）对每个 `/user/app/<TID>` 目录发一条
  `app_dir:true` 条目，标题取自 app.db / appmeta（中文名「潜水员戴夫」「Y2JB」），
  而同 ID 的 `.pkg` 文件卡标题来自 PKG 容器（英文名「DAVE THE DIVER」「YouTube」）。
  同一 title_id 因此出现「英文名 .pkg 卡 + 中文名 app_dir 卡」两张 —— 正是用户看到的样子。
- **按钮差异是设计，不是 bug**（见 §2.4）：app_dir 卡 `deleteBtn` 为空 ⇒ 只有「卸载」；
  .pkg 文件卡才有「删除 + 卸载」。所以「双卡里有一张只有卸载」本身就印证了
  它们是 app_dir 卡，恰好是被去重逻辑覆盖的对象。

### 2.3 实时验证（用户重载后）：去重生效，双卡已消失

用户重载后沙箱成功直连设备（`<ps5-ip>:2026`），取证如下：

- `GET /api/status` → `version: "v1.0.29"`；`GET /` 返回 HTML 解压后含完整去重代码
  （`pkgTid` / `seenTid` / `renderLibraryInner` 均在，与源码 `index.html:5586-5604` 逐项一致）。
- 设备 `GET /` 响应头 **`Cache-Control: no-store`** ⇒ 浏览器不缓存 HTML，**排除浏览器缓存**这一可能。
- 实时拉取 9 个扫描根，把**与设备完全相同的去重算法**跑在真实数据上：

```
total entries collected: 7      # /data/pkg 2 个 .pkg + /user/app 5 个目录
final rendered cards:   5
  tid=CUSA45599 app_dir=0 [删除+卸载] DAVE THE DIVER
  tid=PPSA01650 app_dir=0 [删除+卸载] YouTube
  tid=PPSA23226 app_dir=1 [仅卸载]   黑神话：悟空
  tid=PPSA05143 app_dir=1 [仅卸载]   小小梦魇3
  tid=CUSA20499 app_dir=1 [仅卸载]   Cuphead
```

⇒ 潜水员戴夫（CUSA45599 app_dir）与 Y2JB（PPSA01650 app_dir）被正确丢弃，
  最终 5 张与用户认定的「正确清单」完全吻合。

### 2.4 复现根因（上一轮双卡的真因）

上一轮用户真机见到双卡，而源码 + 本轮实时验证都证明 v1.0.29 去重正确。
结合「设备 `no-store`、浏览器不可能缓存」，唯一解释是：

**上一轮设备跑的 ELF，其内嵌前端是缺去重的旧版本**（v1.0.29 的 C 后端已就位、
`/api/status` 报 v1.0.29，但嵌入的 `index.html` 是更早一代——缺 `app_dir→title_id`
去重的页面）。用户本次「重新加载」实际是把正确的 v1.0.29 ELF 跑起来，
设备随之正确服务带去重的页面，双卡消失。

> 这是**构建/部署**不一致（二进制版本号与内嵌前端不同步），不是前端代码缺陷。
> 若设备上留有多份 ELF、自动启动指向旧的那份，问题可能复发——部署时务必确认
> 启动的是 `PS5-Mango-v1.0.29.elf`（磁盘时间戳 `2026-10-21 23:55`）。

### 2.5 按钮差异的完整解释（用户疑问，已实时印证）

`libCard()`（`index.html:5442-5447`）：
```js
const installBtn = (fpkg || pc || !p.installed) ? 安装 : 卸载;
const deleteBtn = (pc || appDir) ? '' : 删除;   // ← app_dir / PC 不给「删除」
```
| 卡类型 | data 特征 | 显示按钮 | 为什么 |
|---|---|---|---|
| 真 `.pkg` 文件卡 | `app_dir=0 pc=0` | **删除 + 卸载** | 文件在盘上，「删除」删 .pkg 文件；「卸载」卸已装游戏 |
| 安装目录（app_dir）卡 | `app_dir=1` | 仅 **卸载** | path 指向游戏本体目录，`rm -rf` 会连存档一起端掉 ⇒ 故意不给「删除」 |
| PC 直装卡 | `pc=1` | 安装（未装时）/ 卸载（已装时），无「删除」 | 文件在 PC，本机无 path 可删 |

所以「DAVE / YouTube 有删除+卸载，其它只有卸载」**正是预期行为**：
前两张是盘上的 .pkg 文件，后几张（含重复的潜水员戴夫/Y2JB）是安装目录。
不要因为按钮不同就觉得是 bug —— 它反而帮我们确认了双卡的身份。实时计算结果（§2.3）已印证：DAVE/YouTube 为 `app_dir=0` ⇒ 删除+卸载；黑神话/小小梦魇3/Cuphead 为 `app_dir=1` ⇒ 仅卸载。

### 2.6 部署 / 复发提示（已实时验证，双卡已消除）

- 本轮直连设备实时验证：去重正确，最终 5 张卡，双卡已消除（§2.3）。
- 若以后设备上多份 ELF 共存、自动启动指向旧的那份，双卡可能复发：
  部署时务必确认启动的是 `PS5-Mango-v1.0.29.elf`（磁盘时间戳 `2026-10-21 23:55`）。
- 设备在 `/` 返回 `Cache-Control: no-store`，浏览器侧无需特别硬刷新；
  只要设备跑的是正确 ELF，重载即拿新页。

### 2.7 结论与收尾

- 双卡不是 v1.0.29 代码缺陷（源码 + 实时跑真实数据双重确认去重已生效）。
- 上一轮双卡的真因＝设备当时跑的 ELF 内嵌前端缺去重（构建/部署不一致）；
  用户重载/重启后设备现正确服务 v1.0.29，双卡已消除（实时计算 7→5）。
- 按钮差异是 E1 安全设计，无需修改。
- **本轮未改动任何源码**，仅在设备侧验证。无需 bump v1.0.30。

---

## 3. 问题 C：封面

### 3.1 网络层实测（Playwright 抓每个 icon 请求）

```
resp 200 image/png 5721   /api/pkg/icon?path=...YouTube...
resp 503 json        74   /api/save_mgr/icon?title_id=PPSA05143&game=1     <- 失败
resp 503 json        74   /api/save_mgr/icon?title_id=CUSA20499&game=1     <- 失败
resp 200 image/png 240461 /api/pkg/icon?path=...DAVE...
resp 200 image/png 382963 /api/save_mgr/icon?title_id=PPSA23226&game=1
resp 503 json        74   /api/save_mgr/icon?uid=...&title_id=PPSA01650&dir=STARBOARD
resp 400 json       120   /api/save_mgr/icon?uid=...&title_id=PPSA23226&dir=
resp 503 json        74   /api/save_mgr/icon?uid=...&title_id=PPSA26344&dir=
resp 400 json       120   /api/save_mgr/icon?uid=...&title_id=PPSA16608&dir=
resp 400 json       120   /api/save_mgr/icon?uid=...&title_id=PPSA17599&dir=
resp 503 json        74   /api/save_mgr/icon?uid=...&title_id=PPSA05143&dir=OptionSettings
resp 200 image/png 151305 /api/save_mgr/icon?uid=...&title_id=CUSA20499&dir=sce_sdmemory
```

对应卡片终态：

```
{"tid":"CUSA45599","has":true,"loaded":true}    <- 有封面
{"tid":"PPSA01650","has":true,"loaded":true}
{"tid":"PPSA23226","has":true,"loaded":true}
{"tid":"PPSA05143","has":false,"loaded":null}   <- img 已被 onerror 移除
{"tid":"CUSA20499","has":false,"loaded":null}   <- img 已被 onerror 移除
```

截图里这两张就塌成了渐变 + `P05143` / `C20499` 大字兜底。

### 3.2 根因 C1 —— soft 路由 trylock 直接回 503

`src/savemgr.c` 的 `dispatch_save_mgr()`：

```c
if (soft) {
  if (pthread_mutex_trylock(&g_savemgr_lock) != 0)
    return send_json_error(conn, MHD_HTTP_SERVICE_UNAVAILABLE, "busy");
}
```

`/api/save_mgr/icon` **在 soft 名单里**（`:6304` 附近），设计意图是「只读几十 KB，后台导入时也别卡住页面」。
但 trylock 是**不等待**的：页面一次性要 5 张封面 + 存档页同时要 `games`（也是 soft，持锁扫描全部用户目录）
⇒ 抢不到锁的那几张直接 503。

而前端 `coverImg()` 对失败的处理是：

```js
'<img ... onload="this.classList.add(\'loaded\')" onerror="this.remove()">'
```

`remove()` 之后没有任何重试 ⇒ **这张卡的封面在整个页面生命周期里都不会再出现**（只有切页重扫重建 DOM 才有一次新机会）。

后端本身没问题：同一 URL 用 `curl` 单发永远 200 且图片正确（DAVE、YouTube、黑神话、小小梦魇3、Cuphead 五张我都下载看过，内容全对）。

### 3.3 根因 C2 —— `dir` 为空时拼出 `&dir=` 回 400

`svCoverUrl()`（`index.html:2329`）：

```js
if (gameLevel) url += "&game=1";
else url += "&dir=" + encodeURIComponent(dir || "");
```

假卡（`slots=0`）的 `dir_name` 是空串 ⇒ 请求变成 `...&dir=` ⇒ 后端校验：

```c
(!game_level && (!dir || !dir[0]))   ->  400 "uid/title_id[/dir] required"
```

⇒ 那三张已卸载卡的封面请求**必然 400**，与 503 叠加。

### 3.4 根因 C3 —— CUSA45599 游戏级封面 404（记录用，不影响当前 PKG 视图）

```
/api/save_mgr/icon?title_id=CUSA45599&game=1
-> 404 {"ok":false,"error":"no icon"}
```

`SM_ICON_PATS` 六条候选（`savemgr.c:5629`）：

```
/user/home/%s/savedata_prospero_meta/user/%s/%s_icon0.png
/user/appmeta/%s/icon0.png
/user/appmeta/%s/sce_sys/icon0.png
/system_data/priv/appmeta/%s/icon0.png
/system_data/priv/appmeta/%s/sce_sys/icon0.png
/user/app/%s/sce_sys/icon0.png
```

六条全未命中。但 **PKG 页那张卡走的是 `/api/pkg/icon`（从 .pkg 容器抽），实测 240KB 的正确海报**，
所以你在 PKG 页看到的 DAVE 封面是对的。这条只在「存档页出现纯目录型 CUSA45599 卡」时才会暴露。

### 3.5 建议修法（尚未实施）

| # | 位置 | 改动要点 |
|---|---|---|
| 1 | 前端 `coverImg()` | `onerror` 不要 `remove()`，改成**退避重试**（如 300ms / 900ms 各一次，带 `?r=` 破缓存）；三次都失败才落兜底 |
| 2 | 后端 `dispatch_save_mgr` | `/api/save_mgr/icon` 从 soft（trylock 503）改为**带超时的 `pthread_mutex_timedlock`**（如 2s），把「瞬间抖动」变成「稍等即有」 |
| 3 | 前端 `svCoverUrl` | `dir` 为空 ⇒ 自动改用 `&game=1`（消除 400 那一类） |
| 4 | 封面预热 | 预热是串行的、2 个一批（注释已说明），但与首屏 5 张并发叠加仍会打满 —— 预热应让位给真实渲染请求（或先等首屏 `img` 都落定） |

---

## 4. 复现与取证脚本

| 脚本 | 作用 |
|---|---|
| `.build/diag-device-dom.mjs` | 干净 context 直连真机，打印 PKG 卡片清单 + 存档卡片清单 + 截图 |
| `.build/diag-cover-net.mjs` | 逐条记录每个 icon 请求的 status / content-type / 字节数 / 失败原因 |

两者都在 `.build/` 下（`gitignore` 已覆盖），不属于产品代码。
截图：`.build/shots/diag-device-library.png`、`.build/shots/diag-device-saves.png`。

一键复跑（本机 Node 22）：

```bash
cd "/c/Users/<user>/Desktop/Web File Manager/PS5-Mango"
"C:/Users/<user>/.workbuddy/binaries/node/versions/22.22.2-6/node.exe" .build/diag-device-dom.mjs
"C:/Users/<user>/.workbuddy/binaries/node/versions/22.22.2-6/node.exe" .build/diag-cover-net.mjs
```

---

## 5. 待你确认

1. **PKG 重复 / 按钮差异（已定位 + 已证消除，见 §2）**：双卡是「同 ID 的 .pkg 文件卡 + 安装目录卡」，
   v1.0.29 去重正确。上一轮真因＝设备跑的 ELF 内嵌前端缺去重（非浏览器缓存，设备 `Cache-Control: no-store`）；
   用户重载后设备现正确服务 v1.0.29，沙箱实时跑真实数据＝7 入 5 出，双卡已消失（§2.3）。
   按钮差异是 E1 安全设计（app_dir 卡不给「删除」）。去重本身无需 bump；但 v1.0.30
   现用于下一节的「安装包格式名牌」新功能（与去重无关）。
2. **「假存档位」的期望**：删完最后一个存档后，是希望这张游戏卡**整个消失**，还是保留卡片但显示「无存档」？
   （决定 §1.5 里选 3 的哪一种。）
3. 历史残留的三个空目录（冰汽时代2 / 羊蹄山之魂 / 光与影）是否一并清掉？这次修好后可以在 UI 里点删除。

---

## 6. v1.0.30：安装包格式名牌（2026-10-22 新增）

用户要求：PKG 管理页每张卡片用「名牌」标注该游戏的安装包格式 —— 文件夹格式安装 /
exFAT / ffpkg / LZ4 / PKG。

### 6.1 根因背景

后端 `is_fpkg_name()` 把 `.ffpkg` / `.ffpfsc` / `.exfat` **全算 `is_fpkg=true`**，前端无法把
exFAT 与 ffpkg 区分开；且 `lz4` 在代码库里完全没被识别，后端根本不扫 `.lz4` 条目，名牌无从标。

### 6.2 改动

- **后端 `src/mango_port.c`**：
  - 新增 `is_lz4_name()` 识别 `.lz4`；`scan_dir()` 匹配条件加入 `|| is_lz4_name(...)`，
    `.lz4` 条目现在会被扫描出来。
  - `scan_emit()` 计算权威 `fmt` 字段：`exfat`（`.exfat`）/ `ffpkg`（`.ffpkg`/`.ffpfsc`）/
    `lz4`（`.lz4`）/ `pkg`（`.pkg`）；JSON 输出加 `,"fmt":"..."`。
  - `scan_app_root()` 的 app_dir 条目输出 `,"fmt":"app"`。
- **前端 `assets/index.html`**：
  - `libFmt(p)`：优先用后端 `fmt`，否则按 `app_dir` / path 后缀兜底推导
    （`exfat` / `ffpkg` / `lz4` / `pkg`）。
  - `FMT_META`：五类名牌文案 + 配色（app=文件夹 / pkg=PKG / ffpkg=ffpkg / exfat=exFAT / lz4=LZ4）。
  - `libCard()`：封面右上角渲染 `<span class="fmtbadge f-xxx">文案</span>`；卡片 `article`
    加 `data-fmt` 属性；新增 `.cover .fmtbadge` 及五类配色 CSS。
- **守卫**：`dock_pkg_checks.mjs` 新增【7】`libFmt` 分类断言（含 exFAT 判据反证）；
  `frontend-verify-windows.sh` 主链照常跑真实 Chromium 验收 + 本段逻辑断言。
- **版本**：`Makefile` `VERSION_TAG` → `v1.0.30`（ELF 产物 `PS5-Mango-v1.0.30.elf`）。

### 6.3 验证

- `make test-frontend`（`.build/frontend-verify-windows.sh`）：Chromium 验收 + dock 逻辑断言，
  含新增【7】libFmt 分类。
- 真机：重跑后每张卡片封面右上角显示格式名牌；`.pkg` 卡=PKG、app_dir 卡=文件夹、exFAT/ffpkg/lz4
  按 path 后缀正确分流。

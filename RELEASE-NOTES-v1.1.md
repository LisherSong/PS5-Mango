# PS5 Mango v1.1 发布说明

| 项 | 值 |
|---|---|
| 对比基线 | **v1.0** |
| 产物 | `PS5-Mango-v1.1.elf` |
| 大小 | 3,047,080 字节 |
| sha256 | `4add7a06623e276e320ee67b9d43f1929e9a370273b60e9253717af52da7e04b` |
| 端口 | 2026（不变） |

规模变化（相对 v1.0）：后端路由 43 → 44（新增 `GET /api/fs/raw`）；前端 `assets/index.html`
7140 → 7754 行；中英文案 509 → 518 条；`src/` 新增 6 个源文件、改动 22 个文件（+2712 / −401 行）。

---

## 一、新增

### 1. NAS 支持 WebDAV / WebDAVS（HTTPS）

v1.0 只能接 SMB / NFS。现在 PS5 可以直接当 WebDAV 客户端连群晖、飞牛、TrueNAS、Alist、Windows IIS 这类服务端。

- 新增后端模块 `src/backend_webdav.c`，挂在原有的 transfer 后端注册表后面（`src/transfer_internal.h` 定义 seam），
  因此对上层是一套接口：列目录、打开、读、取大小、关闭。
- scheme 现支持 `smb` / `nfs` / `webdav` / `webdavs` / `dav` / `davs`，最长前缀匹配，`webdavs` 天然命中 `webdav`。
- **HTTPS 用 mbedTLS 3.6.7 静态库**（`third_party/mbedtls`，已随仓库），握手时按主机名发送 SNI。
  证书策略是 **不校验**（VERIFY_NONE）——自签证书的群晖/飞牛必须能连，这是有意为之。
- 添加书签时可勾 HTTPS；**已存在的书签**也能就地切换 `webdav ⇄ webdavs`，不必删了重加。
- 地址统一按 `主机[:端口]/路径` 解析（`src/nas_addr.c`，唯一实现）；带 `:端口` 时端口输入框留空即可。

### 2. 页内播放器（v1.0 完全没有）

v1.0 想听/看一个文件，只能整包下载。现在视频和音频直接在页面里播。

- 新路由 `GET /api/fs/raw`：二进制流 + **HTTP Range**（`206 Partial Content` + `Content-Range` + `Accept-Ranges`，越界返回 `416`）。
- 因此进度条可以**拖动**，不必从头重放。
- 播放**不再创建下载任务**，右下角任务坞不会因为看个片子亮起来。
- 视频与音频共用同一个面板：音频没有画面，面板里显示一枚音频图标 + 底部自绘控制条。

### 3. NAS 书签条不再溢出

v1.0 的书签区是一个 `inline-flex` 单体：8 个书签会拼成 2400 px 的一个整体，容器只有约 1070 px，
第 5 个之后**滚不到也点不到**，`+ NAS` 被挤到第三行，单条长地址能撑到 414 px 并折成两行。

- 书签区改为可折行、单条限宽（`240px × --ui`，约 300 px），`+ NAS` 移进书签流内部。
- 长地址按像素预算缩短，**末端目录名永远保留**（`davs:…/自动备份`），完整地址留在 `title` 里。
- 两行封顶，放不下的收进「更多 N」，点开就地展开成「收起」。
- **1～4 个书签与 v1.0 完全一致**，日常用户看不出区别。

### 4. 存档：删完之后不再留空壳

删掉某游戏的最后一份存档后，空的 `sdimg_<dir>` 目录与 `_meta/<TITLE_ID>` 目录现在会自动收尾，
浏览页也会跳过 `slots == 0` 的空条目——不再出现「这个游戏一份存档都没有，卡片却还在」的幽灵卡片。

### 5. 上传/导入进度面板可以关掉了

上传进度是浏览器端的合成行（服务端并没有对应任务），v1.0 里它在上传结束后可能残留 100%，
点 ✕ 下一帧又被弹回来。现在：清理责任移进上传函数本身（`try/finally`）、点 ✕ 直接丢弃、
60 秒心跳过期兜底、合成行不再显示「取消」按钮（点了只会拿到 404）。

### 6. 文档

- 新增 `docs/ADDING-A-BACKEND.md`：给 transfer 层加一个新后端的完整步骤与坑。

---

## 二、删除

**没有删除任何文件、接口或功能**：v1.0 的 43 条 API 全部保留，书签与存档的数据格式不变，升级不需要迁移。

删掉的是四处**旧做法**，它们各自都是一个 bug 的来源：

| 删掉的旧做法 | 因为它而出现的问题 |
|---|---|
| 播放借道下载任务 | 看个视频就把任务坞点亮、进度条拖不动 |
| WebDAV 文件名取自 `displayname` 标签 | 名字里有 `&` 的文件点开就 404（XML 实体没还原） |
| 地址切分在 list / transfer 里各写一份 | 同一串地址两处解析结果不同，`:端口` 重复拼接 |
| mp3 用原生 `<audio controls>` | 与自绘控制条同屏，出现两条进度条 |

---

## 三、优化

- **粘贴/复制文件不再变成空文件夹**：列目录前先用 `PROPFIND Depth:0` 探 `resourcetype`，
  确认是集合才建目录；文件直接写。此前把文件误判为目录，落地成一个空文件夹。
- **报错信息带实际请求路径**：`webdav: not found` 这类消息现在能看出是哪条 URL 失败。
- **文件名上限 256 → 512 字节**，长中文名不再被截断。
- **存档列表**：前端显式判断 `slots != null`，不再把「0 个槽位」和「字段缺失」混为一谈。
- **验证体系**：hosttest 340 条断言、静态守卫 32 条 + 反证 24 个，全部可复现；
  出包时从 ELF 里把内嵌资源抽出来单独断言，避免「源码是新的、打进去的是旧的」。

---

## 四、升级须知

- 直接替换同名 payload ELF 即可，**书签、配置、存档都不需要迁移**，端口仍是 `2026`。
- 新增两个随仓库的第三方依赖：**mbedTLS 3.6.7**（HTTPS，静态库 + 头文件已在 `third_party/mbedtls`）
  与 **cJSON**（`third_party/cjson`）。`make` 不需要额外步骤。
- 想编一个不带 TLS 的版本：`make MBTLS=0`（之后 `webdavs://` 会明确报错，不会静默失败）。

## 五、已知限制

- **WebDAV 只读**：`PUT` / `MKCOL` / `MOVE` 未实现，往 WebDAV 书签里上传、新建目录、移动改名都还不支持。
- 3xx 重定向按失败上报（不保留方法重发）。
- TLS 不校验证书、也不做 TOFU 指纹——自签服务端能连，但无法防中间人。
- 播放器不自适应码率、无字幕轨道。
- 存档「删掉某游戏最后一份存档 ⇒ 卡片立即消失」这条分支在真机上尚未被走到（需要只带 1 个槽位的游戏）。

---

## English summary

**Added** — WebDAV / WebDAVS (HTTPS) shares on top of the existing SMB/NFS backends, with
in-place HTTP ⇄ HTTPS switching for existing bookmarks and SNI on handshake; an in-page
video/audio player backed by `GET /api/fs/raw` with HTTP Range (206/416, seekable, no
download task); a rebuilt NAS bookmark bar that wraps, caps each chip, shortens long
addresses by pixel budget (last path segment always visible) and folds the overflow into
a “more N” toggle; empty save directories are now pruned after the last slot is deleted;
the upload progress row can be dismissed and expires on its own.

**Removed** — nothing: all 43 v1.0 API routes, bookmark and save formats are unchanged, so
upgrading needs no migration. Four obsolete internal approaches were dropped (playback via a
download task, `displayname` as the file name source, duplicated address parsing, the native
`<audio controls>`).

**Improved** — pasting/copying from a share no longer creates an empty folder (PROPFIND
`Depth:0` resource-type probe), error messages carry the failing path, entry names up to 512
bytes, and the verification suite now runs 340 host assertions plus 32 static guards with 24
counter-proofs.

**Limits** — WebDAV is read-only (no `PUT`/`MKCOL`/`MOVE`), 3xx redirects are reported as
failures, TLS certificates are not verified (deliberate, for self-signed NAS devices), and the
player has no adaptive bitrate or subtitle track support.

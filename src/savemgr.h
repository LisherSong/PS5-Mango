/* savemgr.h — PS5 save manager engine (original implementation)
 *
 * Reimplements the save import / replace / export capability of a dedicated
 * save-manager tool, integrated into PS5-Mango behind its existing MHD server.
 * No code was copied from any third-party project; the on-disk PSF format and
 * the sceFs* / /dev/pfsmgr ioctl sequence are Sony platform behaviours that
 * were re-derived from public documentation and console observation.
 *
 * Design boundary:
 *   - Host-testable logic (PSF parse/generate, zip extract via minizip-ng,
 *     savedata.db register via sqlite3, path/account helpers) compiles and
 *     runs on the Linux build.
 *   - Device-only logic (sceFs mount/unmount, PFS create, pfsmgr key decrypt)
 *     is guarded by #ifndef __linux__ and simply returns -1 on the host build,
 *     so the existing file-manager / NAS / PKG features are never affected and
 *     `make linux` + hosttest stay green. On-device behaviour MUST be validated
 *     on a real PS5 console.
 */
#ifndef SAVEMGR_H
#define SAVEMGR_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- PSF param.sfo info ------------------------------------------------ */
typedef struct {
  char title_id[32];      /* e.g. PPSA01234 */
  char dir_name[64];      /* SAVEDATA_DIRECTORY */
  char main_title[256];   /* MAINTITLE */
  char sub_title[256];    /* SUBTITLE */
  char detail[1024];      /* DETAIL */
  uint64_t account_id;    /* ACCOUNT_ID (8 bytes) */
  uint32_t user_id;       /* USER_ID (4 bytes) */
  uint32_t blocks;        /* SAVEDATA_BLOCKS */
  uint32_t system_blocks; /* SYSTEM_BLOCKS */
  uint32_t user_param;    /* USER_PARAM */
  char format[8];         /* "ppr" = PS5, "obs" = PS4 */
  char game_title_id[32]; /* CATEGORY / SAVEDATA_LIST_PARAM fallback */
  uint8_t params_hmac[32];
  int params_hmac_valid;
} sfo_info_t;

/* Parse a param.sfo. Returns 0 ok, -1 bad, -2 zeroed/corrupt. */
int savemgr_parse_sfo(const char *path, sfo_info_t *out);

/* Generate a 15-entry standard PS5/PS4 param.sfo (matches console output). */
int savemgr_generate_sfo(const char *path, const sfo_info_t *info);

/* Cache the per-user PARAMS HMAC from any valid mounted param.sfo. */
void savemgr_cache_params_hmac(const char *sfo_path);

/* Rebind a param.sfo to the local account.
 *  - 解析成功 ⇒ **就地改写** ACCOUNT_ID / USER_ID / PARAMS[0x04]，其余字节原样保留。
 *    （旧实现无条件整文件重建，把原厂 SFO 里没解析的字段全部清零 ⇒ 真机
 *     「导入全部成功但进游戏提示存档损坏」。）
 *  - 解析失败（-2 全零 / -1 读不动）⇒ 才整文件重建；此时 hint 提供调用方已知的
 *    title_id / dir_name（镜像里没有可用 SFO 时的唯一来源），可传 NULL。
 * Returns 0 ok, <0 error. */
int savemgr_resign_sfo(const char *sfo_path, int is_ps4, uint64_t aid,
                       uint32_t uid, const sfo_info_t *hint);

/* Extract a zip (STORE or DEFLATE) into dest dir. Returns file count or <0.
 * 失败分档：-2 临时文件建不出来 / -3 写不进去 / -100-st 解包器报错（st 是
 * zipx_status_t，见 zip_extract.h）。原因串用 savemgr_last_zip_error() 取。 */
int savemgr_extract_zip(const char *dest, const uint8_t *data, size_t len);

/* 同上，但源是**已经在盘上的** ZIP 路径 —— 不把整包读进内存，也不写第二份
 * 临时副本。三条导入路径（import_archive / import_zip / import_encrypted）
 * 都走这里：真机上几百 MB 的整包 malloc(zsize) 就是崩溃/写爆 /data 的来源。 */
int savemgr_extract_zip_path(const char *dest, const char *zip_path);

/* 崩溃后自恢复：清掉上次异常退出留下的暂存（_nex_arc / _nex_zip_*.tmp /
 * _nex_new_* / _nex_enc_*）、best-effort 清掉残留的 /data/mount_sd 挂载，
 * 并把空闲空间与清理结果写进台账。启动时调一次即可（宿主上也调，便于测试）。 */
void savemgr_boot_reconcile(void);

/* 上一次 savemgr_extract_zip 失败的原因（"不是 zip" / "条目名不安全 @ 名字" /
 * "磁盘满" …）；没有失败过则为空串。 */
const char *savemgr_last_zip_error(void);

/* Account id for a local user (reads username.dat + registry). 0 if unknown. */
uint64_t savemgr_account_id_for_user(const char *uid_hex);
int savemgr_foreground_uid(void);

/* Register / update a save entry in the system savedata.db so the OS lists it.
 * Returns 0 ok, <0 error. */
int savemgr_register_db(const char *uid_hex, int is_ps4,
                        const sfo_info_t *info);

/* 从扫描到的目录项名推导存档目录名：`sdimg_<dir>` -> `<dir>`（无前缀则原样）。
 * 镜像是加密 PFS，不挂载读不到 param.sfo ⇒ 扫描阶段只能从文件名推。
 * 刻意不在设备分支内，好让宿主单测能驱动它（见 .build/savemgr-dirname-unit-wsl.sh）。 */
void sm_dir_name_from_entry(const char *entry, char *out, size_t outsz);

/* 系统应用库 /system_data/priv/mms/app.db 里的**真游戏名**。
 * savedata.db 的 main_title 是 param.sfo 的 TITLE（存档自己的名字，意区就是
 * "Dati salvati"），不是游戏名 —— 要显示「Astro's Playroom」只能查这里。
 * 只读打开；拿不到就留空，让调用方回落显示 title_id。宿主可直接驱动。 */
void sm_db_game_title(const char *tid, char *out, size_t outsz);

/* ---- 游戏名/封面的本地缓存（/data/Mango/covers/）
 * 名字与封面都是「游戏装着才有」的货源：游戏卸载后 app.db 行会被清、appmeta
 * 目录会被删，而存档残留还在列表里 —— 用户看到的就是一排裸 title_id 加灰
 * 占位图（2026-10-21 真机报障 PPSA16608/26344/17599）。所以每次成功查到就
 * 落盘，卸载后回放。宿主用 WFM_DATA_ROOT / WFM_APPMETA_ROOT 映射后可测。 */

/* 显示名的统一出口：app.db → appmeta param.json → titles.json 缓存。
 * installed 回传「游戏本体是否还在」（app.db 有行 或 appmeta 目录在）。 */
void sm_game_display_name(const char *tid, char *out, size_t outsz,
                          int *installed);

/* titles.json：1 查到 / 0 没有 / -1 参数问题；put 幂等（值相同不重写）。 */
int  sm_title_cache_get(const char *tid, char *out, size_t outsz);
void sm_title_cache_put(const char *tid, const char *name);

/* appmeta 的 param.json（localizedParameters.titleName，zh-CN → zh-TW →
 * defaultLanguage）。1 查到 / 0 没有。 */
int  sm_appmeta_title(const char *tid, char *out, size_t outsz);

/* 封面缓存：fetch 返回 1 时 out 是**已映射**的缓存文件路径。 */
int  sm_icon_cache_fetch(const char *tid, char *out, size_t n);
void sm_icon_cache_store(const char *tid, const char *src_mapped);

/* 系统备份容器（sce_backup*）：不是游戏，games 列表跳过。宿主可测。 */
int  sm_is_system_container(const char *tid);

/* 枚举一个游戏在本机上的全部存档槽位（savedata_prospero + savedata 里的
 * sdimg_*，返回去前缀的 dir_name；PS4 的 .bin 不是槽位）。
 * ps4_flags 可为 NULL。返回槽位数，超出 max 截断。宿主可直接驱动。 */
int sm_game_slots(const char *uid_hex, const char *tid, char dirs[][64],
                  int max, int *ps4_flags);

/* 按游戏整体删除：该 title_id 下**所有**槽位的镜像 + PS4 .bin + icon + db 行。
 * 「一个游戏 = 一个整体」是用户定的粒度。不挂载。
 * 返回成功删掉的槽位数；0 = 本机没有这个游戏的存档；<0 参数/名字非法。
 * 部分槽位失败不中断（尽力删完），失败数通过 *failed 返回。
 * pruned（可选）回告**清理掉的空目录数**：槽位全删光后 TITLE_ID 目录会变空，
 * 不 rmdir 就是一张删不掉的幽灵卡片（浏览页按目录扫）。 */
int savemgr_delete_game(const char *uid_hex, const char *title_id,
                        int *failed, int *pruned);

/* 清理某个 title 的空目录（PS5/PS4 镜像目录 + PS5 元数据目录），
 * 返回实际清掉的目录数；<0 参数非法。errors（可选）累计 rmdir 失败数。
 * 只在目录**真的空了**时成功（ENOTEMPTY 留给「还有其它槽位」）。 */
int savemgr_prune_empty_dirs(const char *uid_hex, const char *title_id,
                             int *errors);

/* Delete one save slot: image + (PS4) <dir>.bin + meta icon + the db row.
 * Does NOT mount — mounting is for reading/writing save *contents*; deleting
 * the whole slot has no reason to open it (one kernel call = one chance to
 * wedge). Returns 0 deleted, -1 bad name, -2 not found, -3 files gone but the
 * db row survived, -4 unlink failed. */
int savemgr_delete_save(const char *uid_hex, const char *title_id,
                        const char *dir_name);
int savemgr_unregister_db(const char *uid_hex, int is_ps4,
                          const char *title_id, const char *dir_name);

/* 备份自述文件里描述**一个槽位**的结构（整体备份时 slots[] 的每个元素）。
 * 「一个游戏 = 一个整体」是用户定的粒度：一次备份把该游戏所有槽位都带走，
 * manifest 因此必须逐个记下来，否则还原时不知道包里有哪些槽位。 */
typedef struct {
  char dir_name[64];
  char icon[192];   /* 包内 icon 条目名（meta/<tid>/<dir>_icon0.png），可空 */
  int is_ps4;
  uint32_t blocks;
} sm_backup_slot_t;

/* 备份自述文件（包内 manifest.json + 包旁同名 .json 侧车）。
 * ★ 为什么非有不可：存档镜像是**加密 PFS**，不挂载就读不到里面的 param.sfo，
 *   而还原通道刻意不挂载（一次内核调用 = 一次可能永不返回的风险）。没有这份
 *   自述，还原就不知道这份镜像属于哪个游戏、哪个目录、是不是 PS4、blocks 多少。
 *   侧车那份让「备份列表」不必解开 zip 就能列出来（列几十个包时差别是秒级）。
 * blocks 与 main_title 优先取自系统 savedata.db，取不到分别回落 96 与 title_id。
 * 0 ok，<0 写不出来。 */
int savemgr_write_backup_manifest(const char *path, const char *uid_hex,
                                  const char *title_id, const char *dir_name,
                                  const char *main_title, int is_ps4,
                                  uint32_t blocks, uint32_t user_param,
                                  const char *icon_entry,
                                  const char *game_title);

/* 同上，但一次写**多个槽位**（整体备份）。顶层仍保留 dir_name / is_ps4 /
 * blocks / icon 四个字段（取 slots[0]），好让只认旧格式的东西照样能读 ——
 * v1.0.24/25 打的单槽位备份与这一版的整体备份走同一个还原函数。
 * game_title 是真游戏名（来自 app.db），为空时回落 main_title / title_id。
 * nslots 必须 >= 1。0 ok，<0 写不出来。 */
int savemgr_write_backup_manifest2(const char *path, const char *uid_hex,
                                   const char *title_id,
                                   const sm_backup_slot_t *slots, int nslots,
                                   const char *main_title,
                                   uint32_t user_param,
                                   const char *game_title);

/* 解析备份自述里的 slots[]（整体备份时一个包有多个槽位）。
 * 返回槽位数；0 = 旧版单槽位包（调用方回落用顶层 dir_name）。宿主可驱动。 */
int sm_manifest_slots(const char *manifest_json, char dirs[][64], int *ps4,
                      uint32_t *blocks, char icons[][256], int max);

/* 从备份包还原一份存档 —— **全程不挂载**。
 * 口径取自外网做法并经代码核对：同一台机器、同一个账号（离线号 PSNID 全 0）
 * 把加密镜像**直接复制回原路径就能用**，不必解密也不必改签；只有换机器/换
 * 账号才需要走「导入」（重新签名）。省下的正是 create_pfs → 挂载 → 卸载这
 * 三次内核调用。
 * 覆盖现有存档前先把旧的 rename 挪到 sdimg_<dir>.nex-old-<ts>，复制失败能整体
 * 回滚 —— 不让「还原失败」变成「存档没了」。
 * 0 ok；-1 参数/名字非法；-2 备份包不存在；-3 解包失败；-4 不是本工具的备份
 * （缺 manifest 或包里没有对应镜像）；-5 备份属于另一个账号（应走导入）；
 * -6 复制失败；-7 注册系统存档库失败（文件已放回，只是列表可能看不到）。 */
int savemgr_restore_backup(const char *zip_path, const char *uid_hex);

/* ---- device-only save mount engine (returns -1 on host build) ---------- */
int savemgr_mount_image(const char *img_path, char *mount_point, size_t mp_len);
int savemgr_unmount(void);
/* is_ps4 selects the image flavor: PS5 = PPR create with a zero key; PS4 =
 * fresh sealed key (persisted as <img>.bin beside the image) + non-Ppr create.
 * See savemgr.c for the full return-code table. */
int savemgr_create_pfs(const char *img_path, uint64_t data_len, uint8_t *out_key,
                       int is_ps4);
int savemgr_is_mounted(void);
const char *savemgr_mount_point(void);
const char *savemgr_mounted_image(void);
int savemgr_mounted_is_ps4(void);

/* **卸载之前**把挂载点里的 sce_sys/icon0.png 抓到
 * /user/home/<uid>/savedata_prospero_meta/user/<tid>/<dir>_icon0.png
 * —— 系统存档列表的缩略图就是读这里，删存档时也要抹这一份。
 * ⚠️ 时机是全部要点：镜像是加密 PFS 容器，路径下面**没有** sce_sys/ 这层，
 *   卸载之后再按镜像路径去 stat 恒失败（旧实现正是放在卸载之后，于是 icon
 *   从来拷不出来）。所以每个导入路径都要在 savemgr_unmount() **之前**调它。
 * 0 已拷；<0 未挂载 / 源不存在 / PS4 存档 —— 都不算失败，调用方忽略即可。 */
int savemgr_capture_icon(const char *uid_hex, int is_ps4,
                         const sfo_info_t *info);

/* Copy a decrypted/encrypted image into the system save dir and register it.
 * finalize_path is the source image already sitting at g_mounted_image (or an
 * explicit path). Returns 0 ok, <0 error. */
int savemgr_import_finalize(const char *src_img, const char *uid_hex,
                            int is_ps4, const sfo_info_t *info);

/* ---- MHD endpoint handlers (declared for filemgr.c dispatch) ----------- */
struct MHD_Connection;
enum MHD_Result api_save_mgr_list(struct MHD_Connection *conn, const char *url,
                                  const char *body, size_t body_size);
/* Fast game-level save scan (ps5upload-style): one row per title_id/user,
 * directory sizes only, no per-slot database lookups. */
enum MHD_Result api_save_mgr_games(struct MHD_Connection *conn, const char *url,
                                   const char *body, size_t body_size);
enum MHD_Result api_save_mgr_mount(struct MHD_Connection *conn, const char *url,
                                   const char *body, size_t body_size);
enum MHD_Result api_save_mgr_unmount(struct MHD_Connection *conn, const char *url,
                                     const char *body, size_t body_size);
enum MHD_Result api_save_mgr_capture_icon(struct MHD_Connection *conn,
                                          const char *url, const char *body,
                                          size_t body_size);
enum MHD_Result api_save_mgr_files(struct MHD_Connection *conn, const char *url,
                                   const char *body, size_t body_size);
enum MHD_Result api_save_mgr_read(struct MHD_Connection *conn, const char *url,
                                  const char *body, size_t body_size);
enum MHD_Result api_save_mgr_write(struct MHD_Connection *conn, const char *url,
                                   const char *body, size_t body_size);
enum MHD_Result api_save_mgr_upload(struct MHD_Connection *conn, const char *url,
                                    const char *body, size_t body_size);
/* ZIP already streamed to the device; `path` (query param) points at it.
 * Bypasses the 4 MB buffered-body cap that killed /api/save_mgr/upload for
 * real-world save archives. */
enum MHD_Result api_save_mgr_import_zip(struct MHD_Connection *conn,
                                        const char *url, const char *body,
                                        size_t body_size);
enum MHD_Result api_save_mgr_import_encrypted(struct MHD_Connection *conn,
                                              const char *url, const char *body,
                                              size_t body_size);
enum MHD_Result api_save_mgr_import_finish(struct MHD_Connection *conn,
                                           const char *url, const char *body,
                                           size_t body_size);

/* 批量归档导入：一个 ZIP 里含多个 sdimg_* 镜像。
 *   import_archive         解包 + 逐个读信息 → 返回清单（不动本机存档）
 *   import_archive_finish  确认后逐张替换（重新签名 → 落盘 → 注册数据库）
 *   import_archive_cancel  放弃本次，卸挂载 + 清暂存
 * 三段都必须走 dispatch_save_mgr（同一把锁），因为它们共享除挂载点以外的
 * 一份进程内状态（g_arc_*）。 */
enum MHD_Result api_save_mgr_import_archive(struct MHD_Connection *conn,
                                            const char *url, const char *body,
                                            size_t body_size);
enum MHD_Result api_save_mgr_import_archive_finish(struct MHD_Connection *conn,
                                                   const char *url,
                                                   const char *body,
                                                   size_t body_size);
enum MHD_Result api_save_mgr_import_archive_cancel(struct MHD_Connection *conn,
                                                   const char *url,
                                                   const char *body,
                                                   size_t body_size);
enum MHD_Result api_save_mgr_create_pfs(struct MHD_Connection *conn,
                                        const char *url, const char *body,
                                        size_t body_size);
enum MHD_Result api_save_mgr_export(struct MHD_Connection *conn, const char *url,
                                    const char *body, size_t body_size);
/* Download the mounted save (or mount <uid>/<tid>/sdimg_<dir> on demand) as a
 * STORE zip; device-only (mount), host build returns 409 "mount failed". */
enum MHD_Result api_save_mgr_download(struct MHD_Connection *conn,
                                      const char *url, const char *body,
                                      size_t body_size);
/* Create a fresh PFS image from a plain zip (no resign). The image is kept in
 * memory state and served once by /api/save_mgr/encrypt_download. */
enum MHD_Result api_save_mgr_encrypt_upload(struct MHD_Connection *conn,
                                            const char *url, const char *body,
                                            size_t body_size);
enum MHD_Result api_save_mgr_encrypt_download(struct MHD_Connection *conn,
                                              const char *url, const char *body,
                                              size_t body_size);
/* Re-sign an encrypted image to the local account (parse sfo + regenerate). */
enum MHD_Result api_save_mgr_resign_upload(struct MHD_Connection *conn,
                                           const char *url, const char *body,
                                           size_t body_size);
enum MHD_Result api_save_mgr_resign_download(struct MHD_Connection *conn,
                                             const char *url, const char *body,
                                             size_t body_size);
/* Zip a save image (+ PS4 .bin + meta icon + manifest.json) to
 * /data/Mango/backups/<tid>/ (local) or /mnt/<usb>/PS5/SAVEDATA/GARLIC/<tid>/
 * (usb), plus a <same-name>.json sidecar next to the zip. Host-testable. */
enum MHD_Result api_save_mgr_backup(struct MHD_Connection *conn,
                                    const char *url, const char *body,
                                    size_t body_size);
/* Delete one save slot (image + PS4 .bin + icon + db row). Never mounts. */
enum MHD_Result api_save_mgr_delete(struct MHD_Connection *conn,
                                    const char *url, const char *body,
                                    size_t body_size);
/* Serve the meta icon0.png for one save slot (browse page cover art).
 * Read-only; 404 when the console has no icon for that slot. */
enum MHD_Result api_save_mgr_icon(struct MHD_Connection *conn, const char *url,
                                  const char *body, size_t body_size);
/* List the backups under /data/Mango/backups (or USB) by reading the .json
 * sidecars — no zip is opened, so listing stays instant. */
enum MHD_Result api_save_mgr_backups(struct MHD_Connection *conn,
                                     const char *url, const char *body,
                                     size_t body_size);
/* Delete one backup (the zip plus its .json sidecar). Refuses anything outside
 * the local or USB backup root — `path` comes back from the browser. */
enum MHD_Result api_save_mgr_backup_delete(struct MHD_Connection *conn,
                                           const char *url, const char *body,
                                           size_t body_size);
/* Restore one backup by copying the image back to its original path. Never
 * mounts; refuses a backup that belongs to a different user. */
enum MHD_Result api_save_mgr_restore(struct MHD_Connection *conn,
                                     const char *url, const char *body,
                                     size_t body_size);
/* Copy the mounted save tree to the USB stick verbatim (device-only). */
enum MHD_Result api_save_mgr_dump_usb(struct MHD_Connection *conn,
                                      const char *url, const char *body,
                                      size_t body_size);
/* Append a line to the in-flight import-progress log (rendered live by the
 * front end while the blocking import_zip request is still running). */
void savemgr_import_log(const char *fmt, ...);

/* 同上，并**同时**更新结构化进度条。phase 见 savemgr.c 的 SM_IMP_* 表：
 * 0 空闲 · 1 解包 · 2 逐个读取存档信息 · 3 逐个替换落盘。
 * done/total 是展示用的估计值，total 未知传 0（前端只显示当前步骤）。 */
void savemgr_import_progress(int phase, long long done, long long total,
                             const char *fmt, ...);

/* 任务结束：进度归零（前端据 phase==0 收进度条），日志保留供回看。
 * 每个提前 return 的出口都要走到，否则页面永远停在「进行中」。 */
void savemgr_import_progress_end(void);

/* 任务开始：**清空日志缓冲**并把进度归零。
 * ⚠️ 前端是在发请求之前就开窗并轮询的，而日志缓冲从不清 —— 不清的话，新窗
 * 第一帧会把上一次操作的整段日志先重放一遍（用户看到的正是「刚点了替换，
 * 怎么又在解包整包」）。每条长路由**进入时**调一次，与 end() 成对。 */
void savemgr_import_progress_begin(void);

/* Return the accumulated import-progress lines plus the progress triple as
 * {"lines":[...],"phase":N,"done":D,"total":T,"text":"..."}. */
enum MHD_Result api_save_mgr_import_log(struct MHD_Connection *conn,
                                        const char *url, const char *body,
                                        size_t body_size);

/* Single entry point for the whole /api/save_mgr/ namespace. Runs the matched
 * route under one process-wide mutex — the reference save manager serialises
 * every /api/ request the same way, and without it MHD's thread-per-connection
 * model lets two threads mount/unmount (or write into a save being unmounted)
 * at the same time, which deadlocks the PS5 save-mount in the kernel.
 * /api/save_mgr/import_log is exempt: the UI polls it while a long import
 * already holds the lock, and it only reads the log ring. */
enum MHD_Result dispatch_save_mgr(struct MHD_Connection *conn, const char *url,
                                  const char *body, size_t body_size);

#ifdef __cplusplus
}
#endif

#endif /* SAVEMGR_H */

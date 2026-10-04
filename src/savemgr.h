/* savemgr.h — PS5 save manager engine (original implementation)
 *
 * Reimplements the save import / replace / export capability of a dedicated
 * save-manager tool, integrated into ps5-nexus behind its existing MHD server.
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

/* Rebind a param.sfo to the local account (parse -> regenerate with local
 * account_id/user_id + cached HMAC). Returns 0 ok, <0 error. */
int savemgr_resign_sfo(const char *sfo_path, int is_ps4, uint64_t aid,
                       uint32_t uid);

/* Extract a zip (STORE or DEFLATE) into dest dir. Returns file count or <0. */
int savemgr_extract_zip(const char *dest, const uint8_t *data, size_t len);

/* Account id for a local user (reads username.dat + registry). 0 if unknown. */
uint64_t savemgr_account_id_for_user(const char *uid_hex);
int savemgr_foreground_uid(void);

/* Register / update a save entry in the system savedata.db so the OS lists it.
 * Returns 0 ok, <0 error. */
int savemgr_register_db(const char *uid_hex, int is_ps4,
                        const sfo_info_t *info);

/* ---- device-only save mount engine (returns -1 on host build) ---------- */
int savemgr_mount_image(const char *img_path, char *mount_point, size_t mp_len);
int savemgr_unmount(void);
int savemgr_create_pfs(const char *img_path, uint64_t data_len, uint8_t *out_key);
int savemgr_is_mounted(void);
const char *savemgr_mount_point(void);
const char *savemgr_mounted_image(void);
int savemgr_mounted_is_ps4(void);

/* Copy a decrypted/encrypted image into the system save dir and register it.
 * finalize_path is the source image already sitting at g_mounted_image (or an
 * explicit path). Returns 0 ok, <0 error. */
int savemgr_import_finalize(const char *src_img, const char *uid_hex,
                            int is_ps4, const sfo_info_t *info);

/* ---- MHD endpoint handlers (declared for filemgr.c dispatch) ----------- */
struct MHD_Connection;
enum MHD_Result api_save_mgr_list(struct MHD_Connection *conn, const char *url,
                                  const char *body, size_t body_size);
enum MHD_Result api_save_mgr_mount(struct MHD_Connection *conn, const char *url,
                                   const char *body, size_t body_size);
enum MHD_Result api_save_mgr_unmount(struct MHD_Connection *conn, const char *url,
                                     const char *body, size_t body_size);
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
enum MHD_Result api_save_mgr_create_pfs(struct MHD_Connection *conn,
                                        const char *url, const char *body,
                                        size_t body_size);
enum MHD_Result api_save_mgr_export(struct MHD_Connection *conn, const char *url,
                                    const char *body, size_t body_size);

#ifdef __cplusplus
}
#endif

#endif /* SAVEMGR_H */

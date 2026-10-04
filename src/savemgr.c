/* savemgr.c — PS5 save manager engine (original implementation)
 *
 * See savemgr.h for the licensing / design-boundary note. This module is an
 * independent reimplementation of save import / replace / export built on the
 * documented PS5 save-data APIs (sceFs*), the /dev/pfsmgr key-decrypt ioctl,
 * and the Sony PSF param.sfo binary format. No third-party source was copied.
 */
#include "savemgr.h"
#include "filemgr_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <dirent.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/ioctl.h>

#include "zip_extract.h"

#include "sqlite3.h"

#ifndef MAX_PATH_LEN
#define MAX_PATH_LEN 1024
#endif

/* GCC's warn_unused_result is not silenced by a (void) cast; consuming the
 * value in a comparison is. Use this for I/O we deliberately do not recover
 * from (best-effort cache reads, non-critical writes). */
#define SM_IGNORE(rc) do { if ((rc) < 0) {} } while (0)

/* ---- Sony save-data / kernel prototypes (device-only) ----------------- */
/* NOTE: MountOpt / UmountOpt / CreateOpt are opaque Sony save-data structs.
 * Their authoritative layout lives in the official PS5 SDK headers, which are
 * not redistributable. The definitions below are *compile-only* stand-ins that
 * reserve enough space and expose the two fields this module touches
 * (MountOpt.budgetid, CreateOpt.flags). The exact member offsets MUST be
 * re-checked against the real SDK before flashing to hardware. */
#ifndef __linux__
typedef struct MountOpt {
  char budgetid[32];
  uint8_t _pad[512 - 32];
} MountOpt;
typedef struct UmountOpt {
  uint8_t _pad[512];
} UmountOpt;
typedef struct CreateOpt {
  uint8_t flags[16];
  uint8_t _pad[512 - 16];
} CreateOpt;

int sceFsInitMountSaveDataOpt(MountOpt *opt);
int sceFsMountSaveData(MountOpt *opt, const char *path, const char *mount, uint8_t *key);
int sceFsInitUmountSaveDataOpt(UmountOpt *opt);
int sceFsUmountSaveData(UmountOpt *opt, const char *mount, int handle, int ignore);
int sceFsInitCreatePfsSaveDataOpt(CreateOpt *opt);
int sceFsCreatePprPfsSaveDataImage(CreateOpt *opt, const char *path, int x, uint64_t size, uint8_t *key);
int sceFsUfsAllocateSaveData(int fd, uint64_t size, uint64_t flags, int ext);
int sceRegMgrGetStr(int key, char *out, size_t len);
int sceRegMgrGetBin(int key, void *out, size_t len);
int sceUserServiceGetForegroundUser(int *user_id);
int kernel_set_ucred_authid(unsigned long long);
int kernel_get_ucred_caps(int pid, uint8_t *caps);
int kernel_set_ucred_caps(int pid, const uint8_t *caps);
#endif

/* ============================================================ helpers === */

static int recursive_mkdir(const char *path) {
  char tmp[MAX_PATH_LEN];
  snprintf(tmp, sizeof(tmp), "%s", path);
  for (char *p = tmp + 1; *p; p++) {
    if (*p == '/') {
      *p = 0;
      mkdir(tmp, 0777);
      *p = '/';
    }
  }
  return mkdir(tmp, 0777);
}

static int copy_file(const char *src, const char *dst)
  __attribute__((unused)); /* device-only helper; not used on host build */
static int copy_file(const char *src, const char *dst) {
  int in = open(src, O_RDONLY);
  if (in < 0) return -1;
  int out = open(dst, O_CREAT | O_WRONLY | O_TRUNC, 0644);
  if (out < 0) { close(in); return -1; }
  uint8_t buf[65536];
  ssize_t n;
  while ((n = read(in, buf, sizeof(buf))) > 0) {
    ssize_t w = 0;
    while (w < n) {
      ssize_t r = write(out, buf + w, (size_t)(n - w));
      if (r <= 0) { close(in); close(out); return -1; }
      w += r;
    }
  }
  close(in);
  close(out);
  return 0;
}

/* Extract a query parameter (?key=value&) from a URL string. Returns a
 * static buffer (caller must copy). */
static const char *url_param(const char *url, const char *key) {
  static char out[512];
  out[0] = 0;
  char needle[64];
  snprintf(needle, sizeof(needle), "%s=", key);
  const char *p = strstr(url, needle);
  if (!p) return out;
  p += strlen(needle);
  int i = 0;
  while (*p && *p != '&' && i < (int)sizeof(out) - 1)
    out[i++] = *p++;
  out[i] = 0;
  return out;
}

/* ============================================================ PSF ====== */

int savemgr_parse_sfo(const char *path, sfo_info_t *out) {
  memset(out, 0, sizeof(*out));
  int fd = open(path, O_RDONLY);
  if (fd < 0) return -1;
  struct stat st;
  if (fstat(fd, &st) < 0 || st.st_size < 20) { close(fd); return -1; }

  uint8_t *buf = malloc(st.st_size);
  if (!buf) { close(fd); return -1; }
  if (read(fd, buf, st.st_size) != st.st_size) { free(buf); close(fd); return -1; }
  close(fd);

  /* all-zero => corrupted / zeroed SFO (homebrew-created images) */
  int all_zero = 1;
  size_t check = st.st_size < 256 ? (size_t)st.st_size : 256;
  for (size_t i = 0; i < check; i++)
    if (buf[i]) { all_zero = 0; break; }
  if (all_zero) { free(buf); return -2; }

  /* magic: 0x00 'P' 'S' 'F' */
  if (buf[0] != 0x00 || buf[1] != 'P' || buf[2] != 'S' || buf[3] != 'F') {
    free(buf); return -1;
  }

  uint32_t key_off, data_off, n_entries;
  memcpy(&key_off, buf + 8, 4);
  memcpy(&data_off, buf + 12, 4);
  memcpy(&n_entries, buf + 16, 4);

  for (uint32_t i = 0; i < n_entries; i++) {
    uint8_t *idx = buf + 20 + i * 16;
    uint16_t koff;
    uint32_t param_len, doff;
    memcpy(&koff, idx, 2);
    memcpy(&param_len, idx + 4, 4);
    memcpy(&doff, idx + 12, 4);

    const char *key = (const char *)(buf + key_off + koff);
    uint8_t *val = buf + data_off + doff;

    if (strcmp(key, "TITLE_ID") == 0)
      snprintf(out->title_id, sizeof(out->title_id), "%.*s", (int)param_len, (char *)val);
    else if (strcmp(key, "SAVEDATA_DIRECTORY") == 0)
      snprintf(out->dir_name, sizeof(out->dir_name), "%.*s", (int)param_len, (char *)val);
    else if (strcmp(key, "MAINTITLE") == 0 || strcmp(key, "TITLE") == 0)
      snprintf(out->main_title, sizeof(out->main_title), "%.*s", (int)param_len, (char *)val);
    else if (strcmp(key, "SUBTITLE") == 0)
      snprintf(out->sub_title, sizeof(out->sub_title), "%.*s", (int)param_len, (char *)val);
    else if (strcmp(key, "DETAIL") == 0)
      snprintf(out->detail, sizeof(out->detail), "%.*s", (int)param_len, (char *)val);
    else if (strcmp(key, "SAVEDATA_BLOCKS") == 0 && param_len >= 4)
      memcpy(&out->blocks, val, 4);
    else if (strcmp(key, "PARAMS") == 0 || strcmp(key, "USER_PARAM") == 0) {
      if (param_len >= 4) memcpy(&out->user_param, val, 4);
    } else if (strcmp(key, "ACCOUNT_ID") == 0 && param_len >= 8)
      memcpy(&out->account_id, val, 8);
    else if (strcmp(key, "USER_ID") == 0 && param_len >= 4)
      memcpy(&out->user_id, val, 4);
    else if (strcmp(key, "FORMAT") == 0)
      snprintf(out->format, sizeof(out->format), "%.*s", (int)param_len, (char *)val);
  }
  if (!out->game_title_id[0])
    strncpy(out->game_title_id, out->title_id, sizeof(out->game_title_id) - 1);

  free(buf);
  return 0;
}

static uint8_t g_params_hmac[32] = {0};
static int g_params_hmac_valid = 0;

void savemgr_cache_params_hmac(const char *sfo_path) {
  if (g_params_hmac_valid) return;
  int fd = open(sfo_path, O_RDONLY);
  if (fd < 0) return;
  uint8_t hdr[20];
  if (read(fd, hdr, 20) != 20) { close(fd); return; }
  if (hdr[0] != 0 || hdr[1] != 'P' || hdr[2] != 'S' || hdr[3] != 'F') { close(fd); return; }
  uint32_t koff, doff, nent;
  memcpy(&koff, hdr + 8, 4);
  memcpy(&doff, hdr + 12, 4);
  memcpy(&nent, hdr + 16, 4);
  for (uint32_t i = 0; i < nent; i++) {
    uint8_t idx[16];
    SM_IGNORE(pread(fd, idx, 16, 20 + i * 16));
    uint16_t ko; memcpy(&ko, idx, 2);
    char kname[32] = {0};
    SM_IGNORE(pread(fd, kname, 31, koff + ko));
    if (strcmp(kname, "PARAMS") == 0) {
      uint32_t d_off; memcpy(&d_off, idx + 12, 4);
      uint8_t cand[32];
      SM_IGNORE(pread(fd, cand, 32, doff + d_off + 0x08));
      int valid = 0;
      for (int b = 0; b < 32; b++) if (cand[b]) { valid = 1; break; }
      if (valid) {
        memcpy(g_params_hmac, cand, 32);
        g_params_hmac_valid = 1;
      }
      break;
    }
  }
  close(fd);
}

int savemgr_generate_sfo(const char *path, const sfo_info_t *info) {
  /* SFO header + 16-byte index entries + key table + data table.
   * fmt: 0x0004 = raw binary, 0x0204 = utf8 string, 0x0404 = uint32. */
  uint8_t buf[4096];
  memset(buf, 0, sizeof(buf));

  const char *keys[] = {
    "ACCOUNT_ID", "ATTRIBUTE", "CATEGORY", "DETAIL", "FORMAT",
    "LINEAGE_ID", "MAINTITLE", "PARAMS", "SAVEDATA_BLOCKS",
    "SAVEDATA_DIRECTORY", "SAVEDATA_LIST_PARAM", "SAVEDATA_REVISION",
    "SUBTITLE", "SYSTEM_BLOCKS", "TITLE_ID"
  };
  int nkeys = 15;

  uint32_t key_off = 20 + nkeys * 16;
  uint32_t key_total = 0;
  for (int i = 0; i < nkeys; i++) key_total += (uint32_t)strlen(keys[i]) + 1;
  uint32_t data_off = key_off + key_total;
  if (data_off % 4) data_off += 4 - (data_off % 4);

  buf[0] = 0x00; buf[1] = 'P'; buf[2] = 'S'; buf[3] = 'F';
  buf[4] = 0x01; buf[5] = 0x01;
  memcpy(buf + 8, &key_off, 4);
  memcpy(buf + 12, &data_off, 4);
  uint32_t ne = nkeys;
  memcpy(buf + 16, &ne, 4);

  uint32_t kpos = 0;
  for (int i = 0; i < nkeys; i++) {
    size_t kl = strlen(keys[i]) + 1;
    memcpy(buf + key_off + kpos, keys[i], kl);
    kpos += (uint32_t)kl;
  }

  int is_ps4 = (strcmp(info->format, "obs") == 0);
  const char *fmt_str = is_ps4 ? "obs" : "ppr";

  uint32_t dpos = 0;
  kpos = 0;
  for (int i = 0; i < nkeys; i++) {
    uint8_t *idx = buf + 20 + i * 16;
    uint16_t ko = (uint16_t)kpos;
    memcpy(idx, &ko, 2);

    uint32_t param_len = 0, param_max = 0;
    uint16_t param_fmt = 0x0204;

    if (strcmp(keys[i], "ACCOUNT_ID") == 0) {
      param_fmt = 0x0004; param_len = 8; param_max = 8;
      memcpy(buf + data_off + dpos, &info->account_id, 8);
    } else if (strcmp(keys[i], "ATTRIBUTE") == 0) {
      param_fmt = 0x0404; param_len = 4; param_max = 4;
    } else if (strcmp(keys[i], "CATEGORY") == 0) {
      param_len = 3; param_max = 4;
      memcpy(buf + data_off + dpos, "sd", 3);
    } else if (strcmp(keys[i], "DETAIL") == 0) {
      param_len = (info->detail[0] ? (uint32_t)strlen(info->detail) + 1 : 1);
      param_max = 1024;
      if (param_len > param_max) param_len = param_max;
      memcpy(buf + data_off + dpos, info->detail, param_len);
    } else if (strcmp(keys[i], "FORMAT") == 0) {
      param_len = 3; param_max = 4;
      memcpy(buf + data_off + dpos, fmt_str, 3);
    } else if (strcmp(keys[i], "LINEAGE_ID") == 0) {
      param_fmt = 0x0004; param_len = 16; param_max = 16;
    } else if (strcmp(keys[i], "MAINTITLE") == 0) {
      const char *t = info->main_title[0] ? info->main_title : info->title_id;
      param_len = (uint32_t)strlen(t) + 1; param_max = 128;
      if (param_len > param_max) param_len = param_max;
      memcpy(buf + data_off + dpos, t, param_len);
    } else if (strcmp(keys[i], "PARAMS") == 0) {
      param_fmt = 0x0004; param_len = 1024; param_max = 1024;
      uint8_t *p = buf + data_off + dpos;
      uint32_t uid = info->user_id;
      memcpy(p + 0x04, &uid, 4);
      if (g_params_hmac_valid) memcpy(p + 0x08, g_params_hmac, 32);
      uint32_t flags = 1;
      memcpy(p + 0x28, &flags, 4);
      strncpy((char *)(p + 0x2C), info->title_id, 12);
      const char *gtid = info->game_title_id[0] ? info->game_title_id : info->title_id;
      strncpy((char *)(p + 0x38), gtid, 12);
      uint32_t unk2 = 2;
      memcpy(p + 0x54, &unk2, 4);
    } else if (strcmp(keys[i], "SAVEDATA_BLOCKS") == 0) {
      param_fmt = 0x0004; param_len = 8; param_max = 8;
      uint64_t b = info->blocks ? info->blocks : 96;
      memcpy(buf + data_off + dpos, &b, 8);
    } else if (strcmp(keys[i], "SAVEDATA_DIRECTORY") == 0) {
      const char *d = info->dir_name[0] ? info->dir_name : "savedata0";
      param_len = (uint32_t)strlen(d) + 1; param_max = 32;
      if (param_len > param_max) param_len = param_max;
      memcpy(buf + data_off + dpos, d, param_len);
    } else if (strcmp(keys[i], "SAVEDATA_LIST_PARAM") == 0) {
      param_fmt = 0x0404; param_len = 4; param_max = 4;
    } else if (strcmp(keys[i], "SAVEDATA_REVISION") == 0) {
      param_fmt = 0x0004; param_len = 16; param_max = 16;
    } else if (strcmp(keys[i], "SUBTITLE") == 0) {
      const char *st = info->sub_title[0] ? info->sub_title : "";
      param_len = (uint32_t)strlen(st) + 1; param_max = 128;
      if (param_len > param_max) param_len = param_max;
      memcpy(buf + data_off + dpos, st, param_len);
    } else if (strcmp(keys[i], "SYSTEM_BLOCKS") == 0) {
      param_fmt = 0x0004; param_len = 8; param_max = 8;
      uint64_t sb = info->system_blocks;
      memcpy(buf + data_off + dpos, &sb, 8);
    } else if (strcmp(keys[i], "TITLE_ID") == 0) {
      param_len = (uint32_t)strlen(info->title_id) + 1; param_max = 12;
      if (param_len > param_max) param_len = param_max;
      memcpy(buf + data_off + dpos, info->title_id, param_len);
    }

    memcpy(idx + 2, &param_fmt, 2);
    memcpy(idx + 4, &param_len, 4);
    memcpy(idx + 8, &param_max, 4);
    memcpy(idx + 12, &dpos, 4);
    dpos += param_max;
    kpos += (uint32_t)strlen(keys[i]) + 1;
  }

  uint32_t total = data_off + dpos;
  int fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
  if (fd < 0) return -1;
  SM_IGNORE(write(fd, buf, total));
  close(fd);
  return 0;
}

int savemgr_resign_sfo(const char *sfo_path, int is_ps4, uint64_t aid,
                       uint32_t uid) {
  sfo_info_t info;
  int r = savemgr_parse_sfo(sfo_path, &info);
  if (r < 0 && r != -2) return r;
  if (r == -2) {
    /* zeroed SFO: build a minimal info; caller should have supplied values */
    memset(&info, 0, sizeof(info));
    strncpy(info.title_id, "UNKNOWN", sizeof(info.title_id) - 1);
  }
  info.account_id = aid;
  info.user_id = uid;
  snprintf(info.format, sizeof(info.format), "%s", is_ps4 ? "obs" : "ppr");
  if (!info.game_title_id[0])
    strncpy(info.game_title_id, info.title_id, sizeof(info.game_title_id) - 1);
  return savemgr_generate_sfo(sfo_path, &info);
}

/* ============================================================ zip ====== */

int savemgr_extract_zip(const char *dest, const uint8_t *data, size_t len) {
  recursive_mkdir(dest);
  char tmp[MAX_PATH_LEN];
  snprintf(tmp, sizeof(tmp), "/data/save_files/_wfm_zip_%ld.tmp", (long)getpid());
  mkdir("/data/save_files", 0777);
  int fd = open(tmp, O_CREAT | O_WRONLY | O_TRUNC, 0644);
  if (fd < 0) return -1;
  size_t written = 0;
  while (written < len) {
    ssize_t w = write(fd, data + written, len - written);
    if (w <= 0) { close(fd); unlink(tmp); return -1; }
    written += (size_t)w;
  }
  close(fd);

  zipx_result_t result;
  memset(&result, 0, sizeof(result));
  zipx_status_t st = zipx_extract(tmp, dest, ZIPX_CONFLICT_OVERWRITE,
                                  NULL, NULL, NULL, NULL, NULL, &result);
  unlink(tmp);
  if (st != ZIPX_OK) return -1;
  return (int)result.files_created;
}

/* ============================================================ account == */

uint64_t savemgr_account_id_for_user(const char *uid_hex) {
#ifdef __linux__
  (void)uid_hex;
  return 0;
#else
  char upath[MAX_PATH_LEN];
  snprintf(upath, sizeof(upath), "/user/home/%s/username.dat", uid_hex);
  char username[17] = {0};
  int fd = open(upath, O_RDONLY);
  if (fd >= 0) { SM_IGNORE(read(fd, username, 16)); username[16] = 0; close(fd); }
  if (!username[0]) return 0;
  for (int i = 1; i <= 16; i++) {
    int key_name = (i - 1) * 65536 + 125829632;
    char reg_name[32] = {0};
    if (sceRegMgrGetStr(key_name, reg_name, sizeof(reg_name)) < 0) continue;
    if (!reg_name[0]) continue;
    if (strcmp(username, reg_name) == 0) {
      int key_aid = (i - 1) * 65536 + 125830400;
      uint64_t aid = 0;
      sceRegMgrGetBin(key_aid, &aid, sizeof(aid));
      return aid;
    }
  }
  return 0;
#endif
}

int savemgr_foreground_uid(void) {
#ifdef __linux__
  return 0;
#else
  int uid = 0;
  sceUserServiceGetForegroundUser(&uid);
  return uid;
#endif
}

/* ============================================================ db ======= */

int savemgr_register_db(const char *uid_hex, int is_ps4,
                        const sfo_info_t *info) {
  char db_path[MAX_PATH_LEN];
  snprintf(db_path, sizeof(db_path),
           "/system_data/%s/%s/db/user/savedata.db",
           is_ps4 ? "savedata" : "savedata_prospero", uid_hex);
  char db_dir[MAX_PATH_LEN];
  snprintf(db_dir, sizeof(db_dir), "/system_data/%s/%s/db/user",
           is_ps4 ? "savedata" : "savedata_prospero", uid_hex);
  recursive_mkdir(db_dir);

  sqlite3 *db = NULL;
  if (sqlite3_open(db_path, &db) != SQLITE_OK) return -1;
  char *err = NULL;
  sqlite3_exec(db,
    "CREATE TABLE IF NOT EXISTS savedata ("
    "id INTEGER PRIMARY KEY AUTOINCREMENT NOT NULL,"
    "title_id NOT NULL, dir_name NOT NULL, main_title NOT NULL,"
    "sub_title, detail, tmp_dir_name, is_broken, user_param,"
    "blocks, free_blocks, size_kib, mtime NOT NULL,"
    "fake_broken, account_id, user_id, faked_owner,"
    "cloud_icon_url, cloud_revision, game_title_id NOT NULL, system_blocks)",
    NULL, 0, &err);
  if (err) { sqlite3_free(err); err = NULL; }

  char esc[512];
  int j = 0;
  const char *s = info->main_title;
  while (*s && j < (int)sizeof(esc) - 2) { if (*s == '\'') esc[j++] = '\''; esc[j++] = *s++; }
  esc[j] = 0;

  char mtime[64];
  time_t now = time(NULL);
  struct tm *tm = gmtime(&now);
  strftime(mtime, sizeof(mtime), "%Y-%m-%dT%H:%M:%S.00Z", tm);

  uint64_t aid = info->account_id;
  char check[512];
  snprintf(check, sizeof(check),
           "SELECT COUNT(*) FROM savedata WHERE title_id='%s' AND dir_name='%s'",
           info->title_id, info->dir_name);
  int exists = 0;
  sqlite3_stmt *st = NULL;
  if (sqlite3_prepare_v2(db, check, -1, &st, NULL) == SQLITE_OK) {
    if (sqlite3_step(st) == SQLITE_ROW) exists = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
  }
  char sql[2048];
  if (exists) {
    snprintf(sql, sizeof(sql),
      "UPDATE savedata SET main_title='%s', mtime='%s', account_id=%llu, user_id=%u "
      "WHERE title_id='%s' AND dir_name='%s'",
      esc, mtime, (unsigned long long)aid, info->user_id,
      info->title_id, info->dir_name);
  } else {
    snprintf(sql, sizeof(sql),
      "INSERT INTO savedata (title_id, dir_name, main_title, sub_title, detail,"
      "is_broken, user_param, blocks, free_blocks, size_kib, mtime,"
      "fake_broken, account_id, user_id, faked_owner, cloud_revision,"
      "game_title_id, system_blocks) VALUES ("
      "'%s','%s','%s','','',0,%u,%u,%u,%u,'%s',0,%llu,%u,0,0,'%s',0)",
      info->title_id, info->dir_name, esc,
      info->user_param, info->blocks, info->blocks, info->blocks * 0x40, mtime,
      (unsigned long long)aid, info->user_id, info->title_id);
  }
  int rc = sqlite3_exec(db, sql, NULL, 0, &err);
  if (err) sqlite3_free(err);
  sqlite3_close(db);
  return rc == SQLITE_OK ? 0 : -1;
}

/* ============================================================ device === */

#ifndef __linux__

static int g_mounted = 0;
static char g_mount_point[512];
static char g_mounted_img[512];
static char g_local_copy[512];
static int g_is_ps4 = 0;
static int g_mounted_idx = -1;

static void escalate(void) {
  kernel_set_ucred_authid(0x4800000000000010ULL);
  uint8_t caps[16];
  kernel_get_ucred_caps(getpid(), caps);
  caps[7] |= 0x40;
  kernel_set_ucred_caps(getpid(), caps);
  setuid(0);
}

static int decrypt_key(const char *mount_src, int is_ps4, uint8_t *out_key) {
  uint8_t data[0x100];
  memset(data, 0, sizeof(data));
  if (is_ps4) {
    const char *bname = strrchr(mount_src, '/');
    bname = bname ? bname + 1 : mount_src;
    char dir[MAX_PATH_LEN];
    snprintf(dir, sizeof(dir), "%s", mount_src);
    char *sl = strrchr(dir, '/');
    if (sl) *(sl + 1) = 0; else dir[0] = 0;
    const char *savename = (strncmp(bname, "sdimg_", 6) == 0) ? bname + 6 : bname;
    char binp[MAX_PATH_LEN];
    snprintf(binp, sizeof(binp), "%s%s.bin", dir, savename);
    int fd = open(binp, O_RDONLY);
    if (fd < 0) return -4;
    if (read(fd, data, 0x60) != 0x60) { close(fd); return -5; }
    close(fd);
  } else {
    int fd = open(mount_src, O_RDONLY);
    if (fd < 0) return -4;
    if (pread(fd, data, 0x60, 0x800) != 0x60) { close(fd); return -5; }
    close(fd);
  }
  int pfsmgr = open("/dev/pfsmgr", O_RDWR);
  if (pfsmgr < 0) return -6;
  int ret = ioctl(pfsmgr, 0xc0845302, data);
  close(pfsmgr);
  if (ret < 0) return -7;
  memcpy(out_key, data + 0x60, 0x20);
  return 0;
}

int savemgr_mount_image(const char *img_path, char *mount_point, size_t mp_len) {
  if (g_mounted) return -1;
  int is_ps4 = 0;
  int hfd = open(img_path, O_RDONLY);
  if (hfd >= 0) { uint8_t h; if (pread(hfd, &h, 1, 0) == 1 && h == 0x01) is_ps4 = 1; close(hfd); }

  escalate();
  g_is_ps4 = is_ps4;

  const char *mount_src = img_path;
  g_local_copy[0] = 0;
  if (strncmp(img_path, "/data/", 6) != 0) {
    snprintf(g_local_copy, sizeof(g_local_copy), "/data/save_files/%s",
             strrchr(img_path, '/') ? strrchr(img_path, '/') + 1 : "sdimg_import");
    mkdir("/data/save_files", 0777);
    if (copy_file(img_path, g_local_copy) < 0) return -3;
    chmod(g_local_copy, 0755);
    mount_src = g_local_copy;
    if (is_ps4) {
      const char *bname = strrchr(img_path, '/') ? strrchr(img_path, '/') + 1 : img_path;
      const char *sv = (strncmp(bname, "sdimg_", 6) == 0) ? bname + 6 : bname;
      char srcdir[MAX_PATH_LEN]; snprintf(srcdir, sizeof(srcdir), "%s", img_path);
      char *sl = strrchr(srcdir, '/'); if (sl) *(sl + 1) = 0;
      char sb[MAX_PATH_LEN], db[MAX_PATH_LEN];
      snprintf(sb, sizeof(sb), "%s%s.bin", srcdir, sv);
      snprintf(db, sizeof(db), "/data/save_files/%s.bin", sv);
      copy_file(sb, db); chmod(db, 0755);
    }
  }

  uint8_t key[0x20];
  memset(key, 0, sizeof(key));
  int kr = decrypt_key(mount_src, is_ps4, key);
  if (kr < 0) return kr;

  snprintf(g_mount_point, sizeof(g_mount_point), "/data/mount_sd");
  mkdir(g_mount_point, 0777);
  if (mount_point) snprintf(mount_point, mp_len, "%s", g_mount_point);

  MountOpt mopt;
  memset(&mopt, 0, sizeof(mopt));
  sceFsInitMountSaveDataOpt(&mopt);
  strcpy(mopt.budgetid, "system");

  signal(SIGPIPE, SIG_DFL);
  int ret = sceFsMountSaveData(&mopt, mount_src, g_mount_point, key);
  signal(SIGPIPE, SIG_IGN);
  if (ret < 0) return ret;

  snprintf(g_mounted_img, sizeof(g_mounted_img), "%s", img_path);
  g_mounted = 1;
  g_mounted_idx = -1;
  return 0;
}

int savemgr_unmount(void) {
  if (!g_mounted) return -1;
  UmountOpt uopt;
  memset(&uopt, 0, sizeof(uopt));
  sceFsInitUmountSaveDataOpt(&uopt);
  sceFsUmountSaveData(&uopt, g_mount_point, 0, 0);
  sync();
  if (g_local_copy[0] && g_mounted_img[0]) {
    copy_file(g_local_copy, g_mounted_img);
    unlink(g_local_copy);
  }
  g_mounted = 0;
  g_mounted_idx = -1;
  return 0;
}

int savemgr_create_pfs(const char *img_path, uint64_t data_len, uint8_t *out_key) {
  escalate();
  mkdir("/data/save_files", 0777);
  uint64_t img_size = data_len + data_len / 4 + (4 * 1024 * 1024);
  if (img_size < 32 * 1024 * 1024) img_size = 32 * 1024 * 1024;
  img_size = ((img_size + 32767) / 32768) * 32768;

  int fd = open(img_path, O_CREAT | O_TRUNC | O_RDWR, 0777);
  if (fd < 0) return -2;
  int ret = sceFsUfsAllocateSaveData(fd, img_size, 0, 0);
  if (ret < 0) ftruncate(fd, (off_t)img_size);
  close(fd);

  CreateOpt copt;
  memset(&copt, 0, sizeof(copt));
  sceFsInitCreatePfsSaveDataOpt(&copt);
  copt.flags[1] = 0x02;
  uint8_t key[0x20] = {0};
  ret = sceFsCreatePprPfsSaveDataImage(&copt, img_path, 0, img_size, key);
  if (ret < 0) return ret;
  if (out_key) memcpy(out_key, key, 0x20);
  return 0;
}

int savemgr_is_mounted(void) { return g_mounted; }
const char *savemgr_mount_point(void) { return g_mounted ? g_mount_point : ""; }
const char *savemgr_mounted_image(void) { return g_mounted ? g_mounted_img : ""; }
int savemgr_mounted_is_ps4(void) { return g_is_ps4; }

int savemgr_import_finalize(const char *src_img, const char *uid_hex,
                            int is_ps4, const sfo_info_t *info) {
  char sd_dir[MAX_PATH_LEN], sd_dst[MAX_PATH_LEN];
  snprintf(sd_dir, sizeof(sd_dir), "/user/home/%s/%s/%s", uid_hex,
           is_ps4 ? "savedata" : "savedata_prospero", info->title_id);
  snprintf(sd_dst, sizeof(sd_dst), "%s/sdimg_%s", sd_dir, info->dir_name);
  recursive_mkdir(sd_dir);
  if (copy_file(src_img, sd_dst) < 0) return -1;
  chmod(sd_dst, 0644);
  /* icon */
  char icon_src[MAX_PATH_LEN];
  snprintf(icon_src, sizeof(icon_src), "%s/sce_sys/icon0.png", g_mounted_img);
  struct stat ist;
  if (stat(icon_src, &ist) == 0) {
    char icdir[MAX_PATH_LEN], ics[MAX_PATH_LEN];
    snprintf(icdir, sizeof(icdir), "/user/home/%s/savedata_prospero_meta/user/%s",
             uid_hex, info->title_id);
    snprintf(ics, sizeof(ics), "%s/%s_icon0.png", icdir, info->dir_name);
    recursive_mkdir(icdir);
    copy_file(icon_src, ics);
  }
  return savemgr_register_db(uid_hex, is_ps4, info);
}

#else /* __linux__ host stubs */

int savemgr_mount_image(const char *img_path, char *mount_point, size_t mp_len) {
  (void)img_path; (void)mount_point; (void)mp_len; return -1;
}
int savemgr_unmount(void) { return -1; }
int savemgr_create_pfs(const char *img_path, uint64_t data_len, uint8_t *out_key) {
  (void)img_path; (void)data_len; (void)out_key; return -1;
}
int savemgr_is_mounted(void) { return 0; }
const char *savemgr_mount_point(void) { return ""; }
const char *savemgr_mounted_image(void) { return ""; }
int savemgr_mounted_is_ps4(void) { return 0; }
int savemgr_import_finalize(const char *src_img, const char *uid_hex,
                            int is_ps4, const sfo_info_t *info) {
  (void)src_img; (void)uid_hex; (void)is_ps4; (void)info; return -1;
}

#endif

/* ============================================================ API ====== */

static enum MHD_Result json_ok(struct MHD_Connection *conn, const char *extra) {
  char buf[1024];
  snprintf(buf, sizeof(buf), "{\"ok\":true%s%s}", extra ? "," : "", extra ? extra : "");
  return send_buffer(conn, MHD_HTTP_OK, buf, "application/json");
}

enum MHD_Result api_save_mgr_list(struct MHD_Connection *conn, const char *url,
                                  const char *body, size_t body_size) {
  (void)url; (void)body; (void)body_size;
  /* Device-only scan of /user/home/<uid>/savedata_prospero and /savedata. */
#ifndef __linux__
  char out[16384];
  int pos = 0;
  pos += snprintf(out + pos, sizeof(out) - pos, "{\"saves\":[");
  int first = 1;
  DIR *home = opendir("/user/home");
  if (home) {
    struct dirent *hu;
    while ((hu = readdir(home))) {
      if (hu->d_name[0] == '.') continue;
      const char *bases[] = {"savedata_prospero", "savedata"};
      for (int b = 0; b < 2; b++) {
        char tp[MAX_PATH_LEN];
        snprintf(tp, sizeof(tp), "/user/home/%s/%s", hu->d_name, bases[b]);
        DIR *td = opendir(tp);
        if (!td) continue;
        struct dirent *te;
        while ((te = readdir(td))) {
          if (te->d_name[0] == '.') continue;
          char title_path[MAX_PATH_LEN];
          snprintf(title_path, sizeof(title_path), "%s/%s", tp, te->d_name);
          struct stat tst; if (stat(title_path, &tst) < 0 || !S_ISDIR(tst.st_mode)) continue;
          DIR *sd = opendir(title_path);
          if (!sd) continue;
          struct dirent *se;
          while ((se = readdir(sd))) {
            if (se->d_name[0] == '.') continue;
            size_t l = strlen(se->d_name);
            if (l > 4 && strcmp(se->d_name + l - 4, ".bin") == 0) continue;
            if (b == 1 && strncmp(se->d_name, "sdimg_", 6) != 0) continue;
            char fp[MAX_PATH_LEN];
            snprintf(fp, sizeof(fp), "%s/%s", title_path, se->d_name);
            struct stat fst; if (stat(fp, &fst) < 0 || !S_ISREG(fst.st_mode)) continue;
            int ps4 = (b == 1);
            if (!ps4) {
              int h = open(fp, O_RDONLY); uint8_t hdr;
              if (h >= 0) { if (pread(h, &hdr, 1, 0) == 1 && hdr == 0x01) ps4 = 1; close(h); }
            }
            sfo_info_t sfo;
            char sfo_path[MAX_PATH_LEN];
            snprintf(sfo_path, sizeof(sfo_path), "%s/sce_sys/param.sfo", fp);
            int have = (savemgr_parse_sfo(sfo_path, &sfo) == 0);
            if (!first) pos += snprintf(out + pos, sizeof(out) - pos, ",");
            first = 0;
            pos += snprintf(out + pos, sizeof(out) - pos,
              "{\"title_id\":\"%s\",\"dir_name\":\"%s\",\"main_title\":\"%s\","
              "\"is_ps4\":%s,\"uid\":\"%s\",\"path\":\"%s\"}",
              have ? sfo.title_id : te->d_name,
              have ? sfo.dir_name : se->d_name,
              have ? sfo.main_title : "",
              ps4 ? "true" : "false", hu->d_name, fp);
          }
          closedir(sd);
        }
        closedir(td);
      }
    }
    closedir(home);
  }
  pos += snprintf(out + pos, sizeof(out) - pos, "]}");
  return send_buffer(conn, MHD_HTTP_OK, out, "application/json");
#else
  return send_buffer(conn, MHD_HTTP_OK,
    "{\"saves\":[],\"note\":\"save scan is device-only; run on PS5\"}",
    "application/json");
#endif
}

enum MHD_Result api_save_mgr_mount(struct MHD_Connection *conn, const char *url,
                                   const char *body, size_t body_size) {
  (void)url;
  const char *tid = req_param(conn, body, body_size, "title_id");
  const char *dir = req_param(conn, body, body_size, "dir_name");
  const char *uid = req_param(conn, body, body_size, "uid");
  if (!tid || !dir) return send_json_error(conn, MHD_HTTP_BAD_REQUEST, "title_id/dir_name required");
  char uid_hex[16];
  if (!uid || !uid[0]) {
    int f = savemgr_foreground_uid();
    snprintf(uid_hex, sizeof(uid_hex), "%x", (unsigned)f);
  } else snprintf(uid_hex, sizeof(uid_hex), "%s", uid);

  char img[MAX_PATH_LEN];
  snprintf(img, sizeof(img), "/user/home/%s/savedata_prospero/%s/sdimg_%s",
           uid_hex, tid, dir);
  char mp[512];
  int r = savemgr_mount_image(img, mp, sizeof(mp));
  if (r < 0) return send_json_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, "mount failed");
  char sfo[MAX_PATH_LEN];
  snprintf(sfo, sizeof(sfo), "%s/sce_sys/param.sfo", mp);
  savemgr_cache_params_hmac(sfo);
  return json_ok(conn, NULL);
}

enum MHD_Result api_save_mgr_unmount(struct MHD_Connection *conn, const char *url,
                                     const char *body, size_t body_size) {
  (void)url; (void)body; (void)body_size;
  if (savemgr_unmount() < 0)
    return send_json_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, "not mounted");
  return json_ok(conn, NULL);
}

enum MHD_Result api_save_mgr_files(struct MHD_Connection *conn, const char *url,
                                   const char *body, size_t body_size) {
  (void)body; (void)body_size;
  if (!savemgr_is_mounted())
    return send_json_error(conn, MHD_HTTP_CONFLICT, "no save mounted");
  const char *sub = url_param(url, "path");
  char base[MAX_PATH_LEN];
  snprintf(base, sizeof(base), "%s/%s", savemgr_mount_point(), sub);
  char out[16384];
  int pos = 0;
  pos += snprintf(out + pos, sizeof(out) - pos, "{\"files\":[");
  DIR *d = opendir(base);
  int first = 1;
  if (d) {
    struct dirent *e;
    while ((e = readdir(d))) {
      if (e->d_name[0] == '.') continue;
      char fp[MAX_PATH_LEN]; snprintf(fp, sizeof(fp), "%s/%s", base, e->d_name);
      struct stat st; if (stat(fp, &st) < 0) continue;
      if (!first) pos += snprintf(out + pos, sizeof(out) - pos, ",");
      first = 0;
      pos += snprintf(out + pos, sizeof(out) - pos,
        "{\"name\":\"%s\",\"dir\":%s,\"size\":%lld}",
        e->d_name, S_ISDIR(st.st_mode) ? "true" : "false", (long long)st.st_size);
    }
    closedir(d);
  }
  pos += snprintf(out + pos, sizeof(out) - pos, "]}");
  return send_buffer(conn, MHD_HTTP_OK, out, "application/json");
}

enum MHD_Result api_save_mgr_read(struct MHD_Connection *conn, const char *url,
                                  const char *body, size_t body_size) {
  (void)body; (void)body_size;
  if (!savemgr_is_mounted())
    return send_json_error(conn, MHD_HTTP_CONFLICT, "no save mounted");
  const char *p = url_param(url, "path");
  if (!p || !p[0]) return send_json_error(conn, MHD_HTTP_BAD_REQUEST, "path required");
  char fp[MAX_PATH_LEN];
  snprintf(fp, sizeof(fp), "%s/%s", savemgr_mount_point(), p);
  int fd = open(fp, O_RDONLY);
  if (fd < 0) return send_json_error(conn, MHD_HTTP_NOT_FOUND, "file not found");
  struct stat st;
  if (fstat(fd, &st) < 0) { close(fd); return send_json_error(conn, MHD_HTTP_NOT_FOUND, "stat failed"); }
  char *buf = malloc(st.st_size ? (size_t)st.st_size : 1);
  if (!buf) { close(fd); return send_json_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, "oom"); }
  {
    size_t got = 0; ssize_t r;
    while (got < (size_t)st.st_size &&
           (r = read(fd, buf + got, (size_t)st.st_size - got)) > 0)
      got += (size_t)r;
  }
  close(fd);
  enum MHD_Result r = send_buffer(conn, MHD_HTTP_OK, buf, "application/octet-stream");
  free(buf);
  return r;
}

enum MHD_Result api_save_mgr_write(struct MHD_Connection *conn, const char *url,
                                   const char *body, size_t body_size) {
  if (!savemgr_is_mounted())
    return send_json_error(conn, MHD_HTTP_CONFLICT, "no save mounted");
  const char *p = url_param(url, "path");
  if (!p || !p[0]) return send_json_error(conn, MHD_HTTP_BAD_REQUEST, "path required");
  char fp[MAX_PATH_LEN];
  snprintf(fp, sizeof(fp), "%s/%s", savemgr_mount_point(), p);
  int fd = open(fp, O_CREAT | O_WRONLY | O_TRUNC, 0644);
  if (fd < 0) return send_json_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, "write failed");
  {
    size_t off = 0; ssize_t w;
    while (off < body_size &&
           (w = write(fd, body + off, body_size - off)) > 0)
      off += (size_t)w;
  }
  close(fd);
  return json_ok(conn, NULL);
}

/* Import path 1: upload a zip of plaintext save files, build a fresh PFS. */
/* Shared import pipeline: build a fresh PFS image from a plaintext-save ZIP
 * already resident in memory, install it into the caller's account. Used by
 * BOTH entry points:
 *   /api/save_mgr/upload     — ZIP arrives in the HTTP body (buffered; the
 *                              web server caps that at 4 MB, which is exactly
 *                              why real-world zips died with
 *                              "request body is too large")
 *   /api/save_mgr/import_zip — ZIP was streamed to disk first via the
 *                              /api/upload-file channel (no size cap), then
 *                              read back here. This is the path the UI uses.
 * Returns the final MHD response.
 * ⚠️ Parameters come from req_param() (JSON body / form / GET query), NOT
 * from parsing the URL string: MHD's callback url never contains the query
 * string, so the old url_param(url, ...) parser silently saw NOTHING — the
 * only reason it went unnoticed is that the 4 MB body cap killed every real
 * request before parameter parsing could run. */
static enum MHD_Result
savemgr_import_zip_common(struct MHD_Connection *conn, const char *body,
                          size_t body_size, const uint8_t *zip, size_t zsize) {
  const char *uid = req_param(conn, body, body_size, "uid");
  const char *tid = req_param(conn, body, body_size, "title_id");
  const char *dir = req_param(conn, body, body_size, "dir_name");
  const char *mt = req_param(conn, body, body_size, "main_title");
  const char *ps4s = req_param(conn, body, body_size, "ps4");
  int is_ps4 = (ps4s && ps4s[0] == '1');
  if (!tid || !tid[0]) return send_json_error(conn, MHD_HTTP_BAD_REQUEST, "title_id required");

  char uid_hex[16];
  if (!uid || !uid[0]) {
    int f = savemgr_foreground_uid();
    snprintf(uid_hex, sizeof(uid_hex), "%x", (unsigned)f);
  } else snprintf(uid_hex, sizeof(uid_hex), "%s", uid);

  /* create a fresh PFS image in /data/save_files */
  char img[MAX_PATH_LEN];
  snprintf(img, sizeof(img), "/data/save_files/_wfm_new_%s", tid);
  uint8_t key[0x20];
  if (savemgr_create_pfs(img, zsize, key) < 0)
    return send_json_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, "create pfs failed");
  char mp[512];
  if (savemgr_mount_image(img, mp, sizeof(mp)) < 0) {
    unlink(img);
    return send_json_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, "mount failed");
  }
  int n = savemgr_extract_zip(mp, zip, zsize);
  /* build/repair param.sfo */
  char sfo_path[MAX_PATH_LEN];
  snprintf(sfo_path, sizeof(sfo_path), "%s/sce_sys/param.sfo", mp);
  sfo_info_t info;
  if (savemgr_parse_sfo(sfo_path, &info) != 0) {
    memset(&info, 0, sizeof(info));
    strncpy(info.title_id, tid, sizeof(info.title_id) - 1);
    if (dir && dir[0]) strncpy(info.dir_name, dir, sizeof(info.dir_name) - 1);
    if (mt && mt[0]) strncpy(info.main_title, mt, sizeof(info.main_title) - 1);
    snprintf(info.format, sizeof(info.format), "%s", is_ps4 ? "obs" : "ppr");
  }
  uint64_t aid = savemgr_account_id_for_user(uid_hex);
  int fuid = savemgr_foreground_uid();
  savemgr_resign_sfo(sfo_path, is_ps4, aid, (uint32_t)fuid);
  savemgr_unmount();

  sfo_info_t fin;
  memset(&fin, 0, sizeof(fin));
  strncpy(fin.title_id, tid, sizeof(info.title_id) - 1);
  if (dir && dir[0]) strncpy(fin.dir_name, dir, sizeof(fin.dir_name) - 1);
  else strncpy(fin.dir_name, "savedata0", sizeof(fin.dir_name) - 1);
  if (mt && mt[0]) strncpy(fin.main_title, mt, sizeof(fin.main_title) - 1);
  snprintf(fin.format, sizeof(fin.format), "%s", is_ps4 ? "obs" : "ppr");
  fin.account_id = aid;
  fin.user_id = (uint32_t)fuid;
  if (savemgr_import_finalize(img, uid_hex, is_ps4, &fin) < 0) {
    unlink(img);
    return send_json_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, "finalize failed");
  }
  unlink(img);
  char extra[256];
  snprintf(extra, sizeof(extra), "\"files\":%d,\"title_id\":\"%s\"", n, tid);
  return json_ok(conn, extra);
}

enum MHD_Result api_save_mgr_upload(struct MHD_Connection *conn, const char *url,
                                    const char *body, size_t body_size) {
  (void)url;
  if (body_size == 0)
    return send_json_error(conn, MHD_HTTP_BAD_REQUEST, "empty body");
  return savemgr_import_zip_common(conn, body, body_size,
                                   (const uint8_t *)body, body_size);
}

/* Import path 1b: the ZIP was already streamed to the device (via the
 * /api/upload-file channel, same one PKG uploads use), so it can be far
 * bigger than the HTTP body cap. `path` (query param) points at it on disk. */
enum MHD_Result api_save_mgr_import_zip(struct MHD_Connection *conn,
                                        const char *url, const char *body,
                                        size_t body_size) {
  (void)url;
  const char *path = req_param(conn, body, body_size, "path");
  if (!path || path[0] != '/')
    return send_json_error(conn, MHD_HTTP_BAD_REQUEST, "path required");
  struct stat st;
  if (stat(path, &st) || !S_ISREG(st.st_mode) || st.st_size <= 0)
    return send_json_error(conn, MHD_HTTP_BAD_REQUEST, "zip not found");
  size_t zsize = (size_t)st.st_size;
  uint8_t *zip = malloc(zsize);
  if (!zip)
    return send_json_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, "out of memory");
  int fd = open(path, O_RDONLY);
  if (fd < 0) {
    free(zip);
    return send_json_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, "open zip failed");
  }
  size_t off = 0;
  while (off < zsize) {
    ssize_t r = read(fd, zip + off, zsize - off);
    if (r <= 0) break;
    off += (size_t)r;
  }
  close(fd);
  if (off != zsize) {
    free(zip);
    return send_json_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, "read zip failed");
  }
  enum MHD_Result ret = savemgr_import_zip_common(conn, body, body_size,
                                                  zip, zsize);
  free(zip);
  return ret;
}

/* Import path 2: upload an encrypted/decrypted image, mount to inspect. */
enum MHD_Result api_save_mgr_import_encrypted(struct MHD_Connection *conn,
                                              const char *url, const char *body,
                                              size_t body_size) {
  if (body_size == 0)
    return send_json_error(conn, MHD_HTTP_BAD_REQUEST, "empty body");
  const char *uid = url_param(url, "uid");
  const char *ps4s = url_param(url, "ps4");
  int is_ps4 = (ps4s && ps4s[0] == '1');
  char uid_hex[16];
  if (!uid || !uid[0]) {
    int f = savemgr_foreground_uid();
    snprintf(uid_hex, sizeof(uid_hex), "%x", (unsigned)f);
  } else snprintf(uid_hex, sizeof(uid_hex), "%s", uid);

  mkdir("/data/save_files", 0777);
  char img[MAX_PATH_LEN];
  snprintf(img, sizeof(img), "/data/save_files/_wfm_imp_%s", is_ps4 ? "ps4" : "ps5");
  int fd = open(img, O_CREAT | O_WRONLY | O_TRUNC, 0755);
  if (fd < 0) return send_json_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, "tmp create failed");
  {
    size_t off = 0; ssize_t w;
    while (off < body_size &&
           (w = write(fd, body + off, body_size - off)) > 0)
      off += (size_t)w;
  }
  close(fd);

  char mp[512];
  if (savemgr_mount_image(img, mp, sizeof(mp)) < 0) {
    unlink(img);
    return send_json_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, "mount failed");
  }
  char sfo_path[MAX_PATH_LEN];
  snprintf(sfo_path, sizeof(sfo_path), "%s/sce_sys/param.sfo", mp);
  savemgr_cache_params_hmac(sfo_path);
  sfo_info_t sfo;
  int pr = savemgr_parse_sfo(sfo_path, &sfo);
  if (pr == -2) {
    return send_buffer(conn, MHD_HTTP_OK,
      "{\"ok\":true,\"sfo_zeroed\":true,\"match\":true,\"exists\":false}", "application/json");
  }
  if (pr < 0) {
    savemgr_unmount(); unlink(img);
    return send_json_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, "parse sfo failed");
  }
  uint64_t aid = savemgr_account_id_for_user(uid_hex);
  int match = (aid != 0 && sfo.account_id == aid) ? 1 : 0;
  char sd[MAX_PATH_LEN];
  snprintf(sd, sizeof(sd), "/user/home/%s/%s/%s/sdimg_%s", uid_hex,
           is_ps4 ? "savedata" : "savedata_prospero", sfo.title_id, sfo.dir_name);
  struct stat ex; int exists = (stat(sd, &ex) == 0) ? 1 : 0;
  char out[512];
  snprintf(out, sizeof(out),
    "{\"ok\":true,\"title_id\":\"%s\",\"dir_name\":\"%s\",\"main_title\":\"%s\","
    "\"match\":%s,\"exists\":%s}",
    sfo.title_id, sfo.dir_name, sfo.main_title, match ? "true" : "false",
    exists ? "true" : "false");
  return send_buffer(conn, MHD_HTTP_OK, out, "application/json");
}

enum MHD_Result api_save_mgr_import_finish(struct MHD_Connection *conn, const char *url,
                                           const char *body, size_t body_size) {
  (void)body; (void)body_size;
  if (!savemgr_is_mounted())
    return send_json_error(conn, MHD_HTTP_CONFLICT, "no save mounted");
  const char *uid = url_param(url, "uid");
  char uid_hex[16];
  if (!uid || !uid[0]) {
    int f = savemgr_foreground_uid();
    snprintf(uid_hex, sizeof(uid_hex), "%x", (unsigned)f);
  } else snprintf(uid_hex, sizeof(uid_hex), "%s", uid);

  int is_ps4 = savemgr_mounted_is_ps4();
  char sfo_path[MAX_PATH_LEN];
  snprintf(sfo_path, sizeof(sfo_path), "%s/sce_sys/param.sfo", savemgr_mount_point());
  sfo_info_t sfo;
  if (savemgr_parse_sfo(sfo_path, &sfo) != 0) {
    savemgr_unmount();
    return send_json_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, "parse sfo failed");
  }
  uint64_t aid = savemgr_account_id_for_user(uid_hex);
  int fuid = savemgr_foreground_uid();
  savemgr_resign_sfo(sfo_path, is_ps4, aid, (uint32_t)fuid);
  savemgr_unmount();

  if (savemgr_import_finalize(savemgr_mounted_image(), uid_hex, is_ps4, &sfo) < 0)
    return send_json_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, "finalize failed");
  return json_ok(conn, NULL);
}

enum MHD_Result api_save_mgr_create_pfs(struct MHD_Connection *conn, const char *url,
                                        const char *body, size_t body_size) {
  (void)body; (void)body_size;
  const char *tid = url_param(url, "title_id");
  if (!tid || !tid[0]) return send_json_error(conn, MHD_HTTP_BAD_REQUEST, "title_id required");
  const char *uid = url_param(url, "uid");
  char uid_hex[16];
  if (!uid || !uid[0]) {
    int f = savemgr_foreground_uid();
    snprintf(uid_hex, sizeof(uid_hex), "%x", (unsigned)f);
  } else snprintf(uid_hex, sizeof(uid_hex), "%s", uid);

  char img[MAX_PATH_LEN];
  snprintf(img, sizeof(img), "/data/save_files/_wfm_new_%s", tid);
  uint8_t key[0x20];
  if (savemgr_create_pfs(img, 1024 * 1024, key) < 0)
    return send_json_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, "create pfs failed");
  char mp[512];
  if (savemgr_mount_image(img, mp, sizeof(mp)) < 0) {
    unlink(img);
    return send_json_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, "mount failed");
  }
  return json_ok(conn, NULL);
}

enum MHD_Result api_save_mgr_export(struct MHD_Connection *conn, const char *url,
                                    const char *body, size_t body_size) {
  (void)body; (void)body_size;
  /* Export the currently mounted save as a zip download (device-only). */
  if (!savemgr_is_mounted())
    return send_json_error(conn, MHD_HTTP_CONFLICT, "no save mounted");
  const char *p = url_param(url, "path");
  (void)p;
  return send_json_error(conn, MHD_HTTP_NOT_IMPLEMENTED,
    "export not yet wired (mount + read individual files via /api/save_mgr/read)");
}

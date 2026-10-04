/* Transfer abstraction: local FS / SMB(libsmb2) / NFS(libnfs) all expose the
 * same "readable stream + listable directory" interface, so NAS browse / pull /
 * verify / atomic land / resume reuse ONE code path.
 *
 * Adding a backend means implementing the four functions below for that scheme
 * — no other module changes. SMB and NFS are compiled in only when the
 * libraries are vendored (NEXUS_HAVE_LIBSMB2 / NEXUS_HAVE_LIBNFS); without
 * them transfer_open() returns NULL for that scheme so the caller fails
 * cleanly instead of silently pretending the backend works.
 *
 * The local backend is the reference implementation and is what the host test
 * matrix exercises. The NAS backends connect lazily on first use (mirroring
 * the lazy local fd): transfer_open() only validates the URL shape and fills
 * transfer_nas_t, so URL errors fail at open time while real connection
 * errors surface on the first list/read/size call.
 *
 * transfer_url_parse() is deliberately macro-free: it is pure string work, so
 * it is compiled and host-tested on every build even when the NAS libraries
 * are absent. Only the connection/IO glue sits behind the HAVE_ guards, and
 * that part is verified on the SDK machine. */
#include "transfer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>

#if defined(NEXUS_HAVE_LIBSMB2)
/* smb2.h (raw protocol defs: SMB2_GUID_SIZE, smb2_lease_key, ...) must come
 * first -- libsmb2.h references those names. */
#include <smb2/smb2.h>
#include <smb2/libsmb2.h>
#endif
#if defined(NEXUS_HAVE_LIBNFS)
#include <nfsc/libnfs.h>
#endif

typedef enum { SCHEME_LOCAL = 0, SCHEME_SMB, SCHEME_NFS } transfer_scheme_t;

struct transfer_src {
  transfer_scheme_t scheme;
  char   path[NEXUS_PATH_MAX];
  int    fd;        /* local: lazy, opened on first transfer_read */
  int    opened;
  char   err[256];  /* last backend error, human-readable (smb2/nfs_get_error) */
#if defined(NEXUS_HAVE_LIBSMB2) || defined(NEXUS_HAVE_LIBNFS)
  transfer_nas_t nas;   /* filled at transfer_open when scheme != local */
#endif
#if defined(NEXUS_HAVE_LIBSMB2)
  struct smb2_context *smb2;    /* lazy: connected on first use */
  struct smb2fh       *smb2_fh; /* lazy: file opened on first read */
#endif
#if defined(NEXUS_HAVE_LIBNFS)
  struct nfs_context  *nfs;     /* lazy: mounted on first use */
  struct nfsfh        *nfs_fh;  /* lazy: file opened on first read */
#endif
};

/* Record why the NAS backend failed. The API layer forwards this verbatim to
 * the UI, so "NAS 联不上" becomes "mountd refused export" / "auth failed" /
 * "no route to host" instead of a bare "list failed". */
static void
set_err(transfer_src_t *s, const char *msg) {
  if(!s) return;
  snprintf(s->err, sizeof(s->err), "%s", (msg && *msg) ? msg : "unknown error");
}

const char *
transfer_last_error(transfer_src_t *s) {
  return (s && s->err[0]) ? s->err : NULL;
}

static int
parse_scheme(const char *scheme) {
  if(!scheme || !*scheme) return SCHEME_LOCAL;
  if(!strncmp(scheme, "local", 5)) return SCHEME_LOCAL;
  if(!strncmp(scheme, "smb", 3))   return SCHEME_SMB;
  if(!strncmp(scheme, "nfs", 3))   return SCHEME_NFS;
  return SCHEME_LOCAL; /* unknown -> treat as local */
}

static int
scheme_is_known(const char *scheme) {
  if(!scheme || !*scheme) return 1; /* empty/NULL -> default local */
  if(!strncmp(scheme, "local", 5)) return 1;
  if(!strncmp(scheme, "smb", 3))   return 1;
  if(!strncmp(scheme, "nfs", 3))   return 1;
  return 0;
}

int
transfer_scheme_supported(const char *scheme) {
  int sc = parse_scheme(scheme);

  /* parse_scheme folds unknown schemes into SCHEME_LOCAL (transfer_open's
   * "unrecognised prefix = local path" contract), but "supported" must not:
   * an unsupported scheme is a different failure from a bad local path. */
  if(!scheme_is_known(scheme)) return 0;
  if(sc == SCHEME_LOCAL) return 1;
#if defined(NEXUS_HAVE_LIBSMB2)
  if(sc == SCHEME_SMB) return 1;
#endif
#if defined(NEXUS_HAVE_LIBNFS)
  if(sc == SCHEME_NFS) return 1;
#endif
  return 0; /* known-but-unbuilt backend */
}

/* '/' and '\' are interchangeable as path separators here.
 *
 * Windows users both type and paste backslash-separated SMB paths, and the NAS
 * address field is exactly where that lands: "192.168.1.3\PS5_Games". Before
 * this the server-name scan ran straight past the backslash to end-of-string,
 * so the whole thing went to getaddrinfo, which answered
 *   Invalid address:192.168.1.3\PS5_Games Can not resolve into IPv4/v6.
 * -- an error naming neither the offending character nor the field at fault.
 *
 * ⚠️ Credentials are peeled off BEFORE any of this and are deliberately NOT
 * translated: a '\' is a legal password character, not a separator.
 * ⚠️ smb_path() and nfs_list() call this too, so fixing it here covers every
 * caller at once -- that is the point of keeping the rule in one place. */
static int
path_sep(char c) { return c == '/' || c == '\\'; }

int
transfer_url_parse(const char *url, transfer_nas_t *out) {
  const char *server, *share, *p;
  size_t      sl, shl;

  if(!url || !*url || !out) return 0;
  memset(out, 0, sizeof(*out));
  p = url;

  /* Strip an optional scheme:// prefix (smb://, nfs://) and a leading //. */
  if(!strncmp(p, "smb://", 6) || !strncmp(p, "nfs://", 6)) p += 6;
  if(!strncmp(p, "//", 2)) p += 2;

  /* Optional "user[:pass]@" credentials prefix. The server scan below stops at
   * the first ':' which would otherwise grab "user" as the hostname, so the
   * credentials must be peeled off before it. */
  {
    const char *at = strchr(p, '@');
    if(at) {
      const char *sep = memchr(p, ':', (size_t)(at - p));
      size_t ul = sep ? (size_t)(sep - p) : (size_t)(at - p);
      if(ul > sizeof(out->user) - 1) ul = sizeof(out->user) - 1;
      memcpy(out->user, p, ul); out->user[ul] = 0;
      if(sep) {
        size_t pl = (size_t)(at - (sep + 1));
        if(pl > sizeof(out->pass) - 1) pl = sizeof(out->pass) - 1;
        memcpy(out->pass, sep + 1, pl); out->pass[pl] = 0;
      }
      p = at + 1;
    }
  }

  server = p;
  while(*p && !path_sep(*p) && *p != ':') p++;
  sl = (size_t)(p - server);
  if(!sl) return 0;

  /* host[:port]/export/path -- a colon after the server is an optional port
   * (digits only; "host:x" with a non-digit is still rejected). */
  if(*p == ':') {
    int v = 0, digits = 0;
    p++;
    while(*p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); p++; digits++; }
    out->port = v;
    /* 0 位数字本身不算错：`host:/export` 是合法的「不写端口」写法（冒号只是分隔符）。
       只挡两种真·坏输入 —— 冒号后面既不是数字也不是分隔符（"host:x"），以及冒号后
       什么都没有（"host:"）。⚠️ 后者以前是被「share 必须非空」顺带挡住的，现在缺
       share 合法了，必须自己挡，否则 "host:" 会被当成「只填服务器」悄悄接受。 */
    if(*p && !path_sep(*p)) return 0;
    if(!digits && !*p) return 0;
  }
  if(path_sep(*p)) p++;             /* single separator before the share */

  share = p;
  while(*p && !path_sep(*p)) p++;
  shl = (size_t)(p - share);
  /* 缺 share 不再是错误：标成 server_only，SMB 后端改连 IPC$ 去枚举共享名。
     这里以前是 `return 0`，上层把它翻成一句 "cannot open source (bad path)" ——
     用户只填 NAS 的 IP（或末尾多打了个 "/"）时撞的正是这条，且看不出该改什么。 */
  if(!shl) out->server_only = 1;
  if(path_sep(*p)) p++;             /* remainder (may be empty) is the path */

  if(sl  >= sizeof(out->server)) sl  = sizeof(out->server)  - 1;
  if(shl >= sizeof(out->share))  shl = sizeof(out->share)   - 1;
  memcpy(out->server, server, sl); out->server[sl] = 0;
  memcpy(out->share,  share,  shl); out->share[shl]  = 0;
  /* The remainder keeps whatever the user typed, so translate it on the way in:
     every consumer downstream (libsmb2 create name, libnfs export path) wants
     '/', and a stray '\' that reaches SMB2 is at best a literal filename
     character -- silently opening the wrong thing, which is worse than failing. */
  {
    size_t i = 0;
    for(; p[i] && i < sizeof(out->path) - 1; i++)
      out->path[i] = path_sep(p[i]) ? '/' : p[i];
    out->path[i] = 0;
  }
  return 1;
}

transfer_src_t *
transfer_open(const char *scheme, const char *path) {
  transfer_src_t *s;
  int             sc = parse_scheme(scheme);

  /* Backends not compiled in fail loudly at open time. */
#if !defined(NEXUS_HAVE_LIBSMB2)
  if(sc == SCHEME_SMB) return NULL;
#endif
#if !defined(NEXUS_HAVE_LIBNFS)
  if(sc == SCHEME_NFS) return NULL;
#endif
  if(!path || !*path) return NULL;

  s = calloc(1, sizeof(*s));
  if(!s) return NULL;
  s->scheme = sc;
  s->fd = -1;
#if defined(NEXUS_HAVE_LIBSMB2) || defined(NEXUS_HAVE_LIBNFS)
  if(sc == SCHEME_SMB || sc == SCHEME_NFS) {
    if(!transfer_url_parse(path, &s->nas)) { free(s); return NULL; }
  }
#endif
  strncpy(s->path, path, NEXUS_PATH_MAX - 1);
  s->path[NEXUS_PATH_MAX - 1] = 0;
  return s;
}

/* Point an already-open source at another path, reusing the connection.
 * See transfer.h for why; the short version is that the SMB/NFS contexts live
 * inside this struct, so transfer_open() per file would reconnect per file. */
int
transfer_repoint(transfer_src_t *s, const char *path) {
  if(!s || !path || !*path) return 0;

  if(s->scheme == SCHEME_LOCAL) {
    /* No connection to reuse, but the local fd was opened lazily from the OLD
     * path and would otherwise serve the wrong file. */
    if(s->fd >= 0) { close(s->fd); s->fd = -1; }
    s->opened = 0;
  } else {
#if defined(NEXUS_HAVE_LIBSMB2) || defined(NEXUS_HAVE_LIBNFS)
    transfer_nas_t tmp;

    if(!transfer_url_parse(path, &tmp)) return 0;
    /* Same server / share / credentials / port, or the connection must not be
     * reused: a tree connect is bound to ONE share, so pointing a connection
     * at another share would read from the wrong one. */
    if(strcmp(s->nas.server, tmp.server) || strcmp(s->nas.share, tmp.share) ||
       strcmp(s->nas.user, tmp.user) || strcmp(s->nas.pass, tmp.pass) ||
       s->nas.port != tmp.port || s->nas.server_only != tmp.server_only ||
       s->nas.server_only) return 0;
    s->nas = tmp;
#else
    return 0;   /* backend not compiled in; transfer_open already refused it */
#endif
#if defined(NEXUS_HAVE_LIBSMB2)
    if(s->smb2_fh) { smb2_close(s->smb2, s->smb2_fh); s->smb2_fh = NULL; }
#endif
#if defined(NEXUS_HAVE_LIBNFS)
    if(s->nfs_fh) { nfs_close(s->nfs, s->nfs_fh); s->nfs_fh = NULL; }
#endif
  }

  strncpy(s->path, path, NEXUS_PATH_MAX - 1);
  s->path[NEXUS_PATH_MAX - 1] = 0;
  s->err[0] = 0;
  return 1;
}

/* Shared list-append used by all three backends. */
static nexus_err_t
list_push(transfer_entry_t **list, int *n, int *cap,
          const char *name, uint64_t size, uint64_t mtime, uint32_t mode, int is_dir) {
  size_t nl;

  if(*n == *cap) {
    int newcap = *cap ? *cap * 2 : 32;
    transfer_entry_t *tmp = realloc(*list, (size_t)newcap * sizeof(**list));
    if(!tmp) return NEXUS_ERR_NOMEM;
    *list = tmp; *cap = newcap;
  }
  memset(&(*list)[*n], 0, sizeof((*list)[*n]));
  nl = strlen(name);
  if(nl > sizeof((*list)[*n].name) - 1) nl = sizeof((*list)[*n].name) - 1;
  memcpy((*list)[*n].name, name, nl);
  (*list)[*n].name[nl] = 0;
  (*list)[*n].size = size;
  (*list)[*n].mtime = mtime;
  (*list)[*n].mode = mode;
  (*list)[*n].is_dir = is_dir ? 1 : 0;
  (*n)++;
  return NEXUS_OK;
}

#if defined(NEXUS_HAVE_LIBSMB2)
/* Connect lazily; the first successful call owns the context. user == NULL
 * defaults to the current user (libsmb2 sync semantics), which is the
 * anonymous path most home NAS boxes accept without credentials. There is no
 * credential field in the fetch contract yet. */
static nexus_err_t
smb_ensure_connected(transfer_src_t *s) {
  char target[320];

  if(s->smb2) return NEXUS_OK;
  s->smb2 = smb2_init_context();
  if(!s->smb2) return NEXUS_ERR_NOMEM;
  /* Empty strings = anonymous; the URL "user:pass@host/share" prefix fills
   * these when the NAS bookmark carries credentials. */
  if(s->nas.user[0]) smb2_set_user(s->smb2, s->nas.user);
  if(s->nas.pass[0]) smb2_set_password(s->smb2, s->nas.pass);
  /* libsmb2 accepts "<host>[:<port>]" in the server string and falls back to
   * 445 — that is the ONLY way to give it a non-default port (there is no
   * smb2_set_port in this vendored tree), so splice the bookmark's port in
   * here. Without this the UI's 端口 field was silently ignored for SMB while
   * NFS honoured it, which is the kind of asymmetry that reads as "SMB 不通". */
  if(s->nas.port > 0)
    snprintf(target, sizeof(target), "%s:%d", s->nas.server, s->nas.port);
  else
    snprintf(target, sizeof(target), "%s", s->nas.server);
  /* 只填了服务器 ⇒ 连 IPC$：它的根目录在 Windows / 多数 NAS 上会把共享名当目录项
     列出来 —— 用户从此能自己发现共享名，而不是收到一句 bad path 就卡住。 */
  if(smb2_connect_share(s->smb2, target,
                        s->nas.share[0] ? s->nas.share : "IPC$",
                        s->nas.user[0] ? s->nas.user : NULL) < 0) {
    set_err(s, smb2_get_error(s->smb2));   /* e.g. NT_STATUS_LOGON_FAILURE */
    smb2_destroy_context(s->smb2);
    s->smb2 = NULL;
    return NEXUS_ERR_NOTFOUND;
  }
  return NEXUS_OK;
}

/* Path inside the connected share: the explicit dir, else the URL remainder,
 * else the empty name (= the share root).
 * The API layer forwards the frontend's full "host/share/rest" URL as `dir`
 * (that is what the UI path bar holds), so parse it back into share-relative
 * form before opening — otherwise "nas.lan/games/sub" is taken literally and
 * opendir fails (this is exactly why NAS browse appeared dead on the device).
 *
 * 🪤 SMB2 names the share root with an EMPTY create name, and libsmb2 rewrites
 * every '/' into '\' verbatim (see lib/smb2-cmd-create.c). A leading slash
 * therefore arrives at the server as a leading backslash and the answer is
 * STATUS_INVALID_PARAMETER — i.e. "/" does NOT mean "the share root" here, it
 * means "this request is malformed". That single character is why listing a
 * share root always failed (502 nas_list_failed, "NAS 连接失败：未知原因")
 * while descending into "subdir" worked: the remainder had no leading slash.
 * Strip leading slashes; interior separators are fine, the library converts
 * them. Proved by .build/libsmb2-uaf-probe.c step A/B on a real smbd. */
static void
smb_path(transfer_src_t *s, const char *dir, char *buf, size_t bufsz) {
  const char *p = dir ? dir : (s->nas.path[0] ? s->nas.path : "");
  if(dir) {
    transfer_nas_t tmp;
    if(transfer_url_parse(dir, &tmp))
      p = tmp.path;
  }
  /* Both separators, and translate interior ones: the `dir == NULL` fallback
     and the "URL parse failed" fallback can both still carry backslashes, and a
     leading '\' at the share root is the same STATUS_INVALID_PARAMETER as a
     leading '/' (see the note above). */
  while(path_sep(*p)) p++;
  {
    size_t i = 0;
    for(; p[i] && i < bufsz - 1; i++) buf[i] = path_sep(p[i]) ? '/' : p[i];
    buf[i] = 0;
  }
}

static nexus_err_t
smb_list(transfer_src_t *s, const char *dir,
         transfer_entry_t **out, int *count) {
  struct smb2dir    *d;
  struct smb2dirent *ent;
  transfer_entry_t  *list = NULL;
  int               n = 0, cap = 0;
  nexus_err_t       rc;
  char              target[NEXUS_PATH_MAX];

  rc = smb_ensure_connected(s);
  if(rc != NEXUS_OK) return rc;
  smb_path(s, dir, target, sizeof(target));
  d = smb2_opendir(s->smb2, target);
  if(!d) {
    /* libsmb2 already recorded a precise reason ("Opendir failed with
     * (0xc0000034) STATUS_OBJECT_NAME_NOT_FOUND"). Forward it: the API layer
     * puts it in error_arg, so a failed browse is diagnosable from the UI
     * instead of the blank "未知原因" it used to show. */
    set_err(s, smb2_get_error(s->smb2));
    return NEXUS_ERR_NOTFOUND;
  }
  while((ent = smb2_readdir(s->smb2, d))) {
    if(!strcmp(ent->name, ".") || !strcmp(ent->name, "..")) continue;
    rc = list_push(&list, &n, &cap, ent->name,
                   (uint64_t)ent->st.smb2_size,
                   (uint64_t)ent->st.smb2_mtime,
                   0,  /* SMB2 stat has no Unix mode bits */
                   ent->st.smb2_type == SMB2_TYPE_DIRECTORY);
    if(rc != NEXUS_OK) { free(list); smb2_closedir(s->smb2, d); return rc; }
  }
  smb2_closedir(s->smb2, d);
  *out = list;
  *count = n;
  return NEXUS_OK;
}

static nexus_err_t
smb_size(transfer_src_t *s, uint64_t *size) {
  struct smb2_stat_64 st;
  char                target[NEXUS_PATH_MAX];
  nexus_err_t         rc;

  rc = smb_ensure_connected(s);
  if(rc != NEXUS_OK) return rc;
  smb_path(s, NULL, target, sizeof(target));
  if(smb2_stat(s->smb2, target, &st) < 0) return NEXUS_ERR_NOTFOUND;
  *size = (uint64_t)st.smb2_size;
  return NEXUS_OK;
}

static nexus_err_t
smb_read(transfer_src_t *s, uint64_t offset, void *buf, size_t len, size_t *got) {
  int   n;
  char  target[NEXUS_PATH_MAX];
  nexus_err_t rc;

  rc = smb_ensure_connected(s);
  if(rc != NEXUS_OK) return rc;
  if(!len) { *got = 0; return NEXUS_OK; }
  if(!s->smb2_fh) {
    smb_path(s, NULL, target, sizeof(target));
    s->smb2_fh = smb2_open(s->smb2, target, O_RDONLY);
    if(!s->smb2_fh) return NEXUS_ERR_NOTFOUND;
  }
  /* smb2_pread takes a uint32_t count; our chunks are far below that. */
  n = smb2_pread(s->smb2, s->smb2_fh, (uint8_t *)buf, (uint32_t)len, offset);
  if(n < 0) return NEXUS_ERR_GENERIC;
  *got = (size_t)n;   /* 0 is clean EOF, not an error */
  return NEXUS_OK;
}

static void
smb_close(transfer_src_t *s) {
  if(s->smb2_fh) smb2_close(s->smb2, s->smb2_fh);
  if(s->smb2) smb2_destroy_context(s->smb2);
}
#endif /* NEXUS_HAVE_LIBSMB2 */

#if defined(NEXUS_HAVE_LIBNFS)
/* nfsio_* prefix: nfs_read/nfs_close are already public libnfs API names. */
static nexus_err_t
nfsio_ensure_connected(transfer_src_t *s) {
  char export_name[NEXUS_PATH_MAX];

  if(s->nfs) return NEXUS_OK;
  /* NFS export names are absolute paths; the URL form host:/export/path
   * parses to a share without the leading slash, so re-add it for mountd. */
  if(s->nas.share[0] == '/') {
    strncpy(export_name, s->nas.share, sizeof(export_name) - 1);
    export_name[sizeof(export_name) - 1] = 0;
  } else {
    snprintf(export_name, sizeof(export_name), "/%s", s->nas.share);
  }

  /* libnfs defaults to NFSv3. Many modern NAS export ONLY v4, so try v3 first
   * and fall back to NFSv4.0. A fresh context per attempt: a failed mount can
   * leave the context unusable. NFS_V4 == 4 is defined in libnfs's internal
   * headers (nfs4/libnfs-raw-nfs4.h), NOT the public libnfs.h, so use 4. */
  for(int attempt = 0; attempt < 2; attempt++) {
    s->nfs = nfs_init_context();
    if(!s->nfs) return NEXUS_ERR_NOMEM;
    /* A custom port MUST be set before mount: nfs_mount connects immediately,
     * so setting it afterwards (the old order) silently had no effect. */
    if(s->nas.port > 0) nfs_set_nfsport(s->nfs, s->nas.port);
    if(attempt == 1) nfs_set_version(s->nfs, 4 /* NFSv4.0 */);
    if(nfs_mount(s->nfs, s->nas.server, export_name) == 0) return NEXUS_OK;
    set_err(s, nfs_get_error(s->nfs));   /* capture before destroying the ctx */
    nfs_destroy_context(s->nfs);
    s->nfs = NULL;
  }
  return NEXUS_ERR_NOTFOUND;
}

static nexus_err_t
nfsio_list(transfer_src_t *s, const char *dir,
         transfer_entry_t **out, int *count) {
  struct nfsdir    *d = NULL;
  struct nfsdirent *ent;
  transfer_entry_t *list = NULL;
  int              n = 0, cap = 0;
  nexus_err_t      rc;
  const char       *target = dir ? dir : s->nas.path;

  rc = nfsio_ensure_connected(s);
  if(rc != NEXUS_OK) return rc;
  /* Same host/share/rest handling as smb_path: the frontend passes the full
   * URL, so strip the server+share before nfs_opendir (share-relative). */
  if(dir) {
    transfer_nas_t tmp;
    if(transfer_url_parse(dir, &tmp))
      target = tmp.path[0] ? tmp.path : "/";
  }
  if(nfs_opendir(s->nfs, target, &d) != 0) return NEXUS_ERR_NOTFOUND;
  while((ent = nfs_readdir(s->nfs, d))) {
    int is_dir;
    if(!strcmp(ent->name, ".") || !strcmp(ent->name, "..")) continue;
    /* libnfs fills type when the server answers READDIRPLUS; fall back to the
     * mode bits for old servers (NF3DIR == 2 in the NFSv3 protocol). */
    is_dir = ((ent->type == 2) || S_ISDIR(ent->mode)) ? 1 : 0;
    rc = list_push(&list, &n, &cap, ent->name, (uint64_t)ent->size,
                   (uint64_t)ent->mtime.tv_sec, (uint32_t)(ent->mode & 07777), is_dir);
    if(rc != NEXUS_OK) { free(list); nfs_closedir(s->nfs, d); return rc; }
  }
  nfs_closedir(s->nfs, d);
  *out = list;
  *count = n;
  return NEXUS_OK;
}

static nexus_err_t
nfsio_size(transfer_src_t *s, uint64_t *size) {
  /* libnfs itself splits nfs_stat's struct on _WIN32 (struct __stat64); mirror
   * that here so the glue compiles on a Win32 host too. The PS5 payload uses
   * the POSIX branch. */
#if defined(_WIN32)
  struct __stat64 st;
#else
  struct stat st;
#endif
  nexus_err_t rc;

  rc = nfsio_ensure_connected(s);
  if(rc != NEXUS_OK) return rc;
  if(nfs_stat(s->nfs, s->nas.path, &st) != 0) return NEXUS_ERR_NOTFOUND;
  *size = (uint64_t)st.st_size;
  return NEXUS_OK;
}

static nexus_err_t
nfsio_read(transfer_src_t *s, uint64_t offset, void *buf, size_t len, size_t *got) {
  int         n;
  nexus_err_t rc;

  rc = nfsio_ensure_connected(s);
  if(rc != NEXUS_OK) return rc;
  if(!len) { *got = 0; return NEXUS_OK; }
  if(!s->nfs_fh) {
    if(nfs_open(s->nfs, s->nas.path, O_RDONLY, &s->nfs_fh) != 0)
      return NEXUS_ERR_NOTFOUND;
  }
  n = nfs_pread(s->nfs, s->nfs_fh, buf, len, offset);
  if(n < 0) return NEXUS_ERR_GENERIC;
  *got = (size_t)n;   /* 0 is clean EOF, not an error */
  return NEXUS_OK;
}

static void
nfsio_close(transfer_src_t *s) {
  if(s->nfs_fh) nfs_close(s->nfs, s->nfs_fh);
  if(s->nfs) nfs_destroy_context(s->nfs);
}
#endif /* NEXUS_HAVE_LIBNFS */

nexus_err_t
transfer_list(transfer_src_t *s, const char *dir,
              transfer_entry_t **out, int *count) {
  DIR            *d;
  struct dirent  *ent;
  transfer_entry_t *list = NULL;
  int             n = 0, cap = 0;
  const char     *target;
  char            base[NEXUS_PATH_MAX];

  if(!s || !out || !count) return NEXUS_ERR_GENERIC;
  target = dir ? dir : s->path;
  if(s->scheme == SCHEME_LOCAL) {
    nexus_err_t rc;

    strncpy(base, target, sizeof(base) - 1);
    base[sizeof(base) - 1] = 0;
    d = opendir(base);
    if(!d) return NEXUS_ERR_NOTFOUND;
    while((ent = readdir(d))) {
      char full[NEXUS_PATH_MAX + 300];
      struct stat st;

      if(!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;
      snprintf(full, sizeof(full), "%s/%s", base, ent->d_name);
      if(stat(full, &st)) continue;
      rc = list_push(&list, &n, &cap, ent->d_name,
                     (uint64_t)st.st_size, (uint64_t)st.st_mtime,
                     (uint32_t)(st.st_mode & 07777), S_ISDIR(st.st_mode));
      if(rc != NEXUS_OK) { free(list); closedir(d); return rc; }
    }
    closedir(d);
    *out = list;
    *count = n;
    return NEXUS_OK;
  }
#if defined(NEXUS_HAVE_LIBSMB2)
  if(s->scheme == SCHEME_SMB) return smb_list(s, dir, out, count);
#endif
#if defined(NEXUS_HAVE_LIBNFS)
  if(s->scheme == SCHEME_NFS) return nfsio_list(s, dir, out, count);
#endif
  return NEXUS_ERR_UNSUPPORTED;
}

nexus_err_t
transfer_size(transfer_src_t *s, uint64_t *size) {
  struct stat st;

  if(!s || !size) return NEXUS_ERR_GENERIC;
  *size = 0;
  if(s->scheme == SCHEME_LOCAL) {
    if(stat(s->path, &st)) return NEXUS_ERR_NOTFOUND;
    *size = (uint64_t)st.st_size;
    return NEXUS_OK;
  }
#if defined(NEXUS_HAVE_LIBSMB2)
  if(s->scheme == SCHEME_SMB) return smb_size(s, size);
#endif
#if defined(NEXUS_HAVE_LIBNFS)
  if(s->scheme == SCHEME_NFS) return nfsio_size(s, size);
#endif
  return NEXUS_ERR_UNSUPPORTED;
}

/* Recommended per-call read size; see transfer.h for why this exists.
 * 1 MB is the floor because it is libsmb2's own default max_read_size when a
 * server does not advertise one, and because a copy loop that read in smaller
 * pieces than the link allows is exactly the slowdown users report. The 4 MB
 * ceiling only bounds a caller's buffer against a server with an unreasonably
 * large advertisement. */
#define TRANSFER_CHUNK_FLOOR (1u * 1024u * 1024u)
#define TRANSFER_CHUNK_CAP   (4u * 1024u * 1024u)

size_t
transfer_read_chunk(transfer_src_t *s) {
  size_t chunk = TRANSFER_CHUNK_FLOOR;

  if(!s) return TRANSFER_CHUNK_FLOOR;
#if defined(NEXUS_HAVE_LIBSMB2)
  if(s->scheme == SCHEME_SMB && s->smb2) {
    uint32_t mx = smb2_get_max_read_size(s->smb2);
    if(mx >= 4096 && (size_t)mx < chunk) chunk = (size_t)mx;
  }
#endif
  /* NFS: libnfs exposes no negotiated read ceiling; the floor (1 MB) is also
   * the NFSv3 protocol's own per-READ maximum, so it is the right value. */
  if(chunk > TRANSFER_CHUNK_CAP) chunk = TRANSFER_CHUNK_CAP;
  return chunk;
}

nexus_err_t
transfer_read(transfer_src_t *s, uint64_t offset,
              void *buf, size_t len, size_t *got) {
  ssize_t n;

  if(!s || !buf || !got) return NEXUS_ERR_GENERIC;
  *got = 0;
  if(s->scheme == SCHEME_LOCAL) {
    if(!len) return NEXUS_OK;
    if(!s->opened) {
      s->fd = open(s->path, O_RDONLY
#ifdef O_BINARY
                            | O_BINARY
#endif
                   );
      s->opened = 1;
    }
    if(s->fd < 0) return NEXUS_ERR_NOTFOUND;
    /* lseek+read rather than pread: pread needs _XOPEN_SOURCE on MinGW and the
     * payload is single-threaded, so there is no concurrent-offset concern. */
    if(lseek(s->fd, (off_t)offset, SEEK_SET) < 0) return NEXUS_ERR_GENERIC;
    n = read(s->fd, buf, len);
    if(n < 0) {
      if(errno == EINTR) return NEXUS_OK;
      return NEXUS_ERR_GENERIC;
    }
    *got = (size_t)n;
    return NEXUS_OK; /* n == 0 is clean EOF, not an error */
  }
#if defined(NEXUS_HAVE_LIBSMB2)
  if(s->scheme == SCHEME_SMB) return smb_read(s, offset, buf, len, got);
#endif
#if defined(NEXUS_HAVE_LIBNFS)
  if(s->scheme == SCHEME_NFS) return nfsio_read(s, offset, buf, len, got);
#endif
  return NEXUS_ERR_UNSUPPORTED;
}

void
transfer_close(transfer_src_t *s) {
  if(!s) return;
  if(s->scheme == SCHEME_LOCAL) {
    if(s->fd >= 0) close(s->fd);
  }
#if defined(NEXUS_HAVE_LIBSMB2)
  if(s->scheme == SCHEME_SMB) smb_close(s);
#endif
#if defined(NEXUS_HAVE_LIBNFS)
  if(s->scheme == SCHEME_NFS) nfsio_close(s);
#endif
  free(s);
}

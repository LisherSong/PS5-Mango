#ifndef TRANSFER_H
#define TRANSFER_H
/* Transfer abstraction: local FS / SMB(libsmb2) / NFS(libnfs) all expose the
 * same "readable stream + listable directory" interface. NAS browse / pull /
 * verify / atomic land / resume therefore reuse ONE code path.
 * New backends are added here only — no other module changes. */
#include "nexus_common.h"

#ifndef NEXUS_PATH_MAX
#define NEXUS_PATH_MAX 1024
#endif

typedef struct transfer_src transfer_src_t; /* opaque backend */

typedef struct {
    char  name[256];
    uint64_t size;
    uint64_t mtime;   /* seconds since epoch; 0 when unknown */
    uint32_t mode;    /* st_mode permission bits; 0 when unknown */
    int   is_dir;
} transfer_entry_t;

/* Decoded NAS location. server is the hostname/IP, share is the smb share name
 * or nfs export name (verbatim from the URL), path is the remainder relative
 * to that share/export root. user/pass are the optional credentials from a
 * "user:pass@host" prefix (empty strings = anonymous).
 * server_only is set when the address carried NO share at all ("192.168.1.3"
 * or "192.168.1.3/"). That is legal for SMB — the backend then connects to
 * IPC$ and lists the share names as directory entries, which is what a user
 * who only knows the NAS's IP actually needs. NFS has no equivalent (an export
 * is mandatory), so it will fail at mount time with a real reason. */
typedef struct {
    char server[256];
    char share[256];
    char path[NEXUS_PATH_MAX];
    char user[128];
    char pass[128];
    int  port;   /* 0 = default for the scheme (445 SMB / 2049 NFS) */
    int  server_only;
} transfer_nas_t;

/* scheme: "local:" | "smb:" | "nfs:" (default local). */
transfer_src_t *transfer_open(const char *scheme, const char *path);
/* Point an ALREADY-OPEN source at another path so the connection is reused.
 *
 * Why it exists: the backend contexts live inside transfer_src_t, so
 * transfer_open() per file pays a fresh TCP connect + tree connect for EVERY
 * file. Copying a save folder of a few hundred small files over SMB is the
 * difference between one round trip per file and three.
 *
 * The per-file handle is dropped here on purpose: smb_read/nfsio_read open it
 * lazily from the URL's path, so keeping the old handle would silently serve
 * the PREVIOUS file's bytes.
 *
 * Returns 0 (caller must fall back to transfer_open + transfer_close) when the
 * new URL names a different server / share / credentials / port — reusing the
 * connection there would send the request to the WRONG share, which is worse
 * than being slow. Returns 1 when the repoint took effect. */
int transfer_repoint(transfer_src_t *s, const char *path);
/* 1 if the scheme could be opened at all on this build (i.e. it is known AND
 * its backend library was compiled in). Lets the API layer tell "smb backend
 * not built" apart from "bad path" instead of one generic error. */
int transfer_scheme_supported(const char *scheme);
/* Parse a NAS URL into server/share/path. Pure string work: compiled and
 * host-tested even when the NAS backends are not built. Accepted forms:
 *   smb://host/share/path   nfs://host/export/path   //host/share/path
 *   host/share/path         host:/export/path        host   host:port
 * A missing share is NOT an error any more: it sets `server_only` and the SMB
 * backend enumerates shares from IPC$ instead. (It used to return 0 here, and
 * the caller turned that into an unactionable "cannot open source (bad path)"
 * — the exact message users hit when they typed only the NAS's IP.)
 * Returns 1 on success, 0 on malformed input (empty server, "host:x" with a
 * non-numeric port). The trailing path may be empty (the share root). */
int transfer_url_parse(const char *url, transfer_nas_t *out);
nexus_err_t transfer_list(transfer_src_t *s, const char *dir,
                          transfer_entry_t **out, int *count);
nexus_err_t transfer_read(transfer_src_t *s, uint64_t offset,
                          void *buf, size_t len, size_t *got);
/* Recommended per-call read size for this source, so callers can size their
 * copy loop to the backend's negotiation instead of a safe-for-everyone guess.
 *
 * Why it matters: the SMB sync API is one request/one reply per call, so a
 * copy's ceiling is roughly chunk/RTT. 256 KB per round trip reads as ~7 MB/s
 * on a real gigabit link — the exact "25 GB 传了十分钟才 18%" report. The SMB2
 * negotiation already carries a much larger max_read_size (≥1 MB against
 * Samba/Windows); reading in that size quarters the round-trip count.
 *
 * Call it AFTER a first successful list/read/size on the source (the NAS
 * backends connect lazily; before that there is no negotiation to ask).
 * Never returns 0 or a value above 4 MB, so the caller's malloc stays bounded
 * even if a server advertises something absurd. */
size_t transfer_read_chunk(transfer_src_t *s);
/* Total stream size, so the UI can draw a determinate progress bar instead of
 * an indeterminate one. Only meaningful for a file-like source. */
nexus_err_t transfer_size(transfer_src_t *s, uint64_t *size);
void transfer_close(transfer_src_t *s);
/* Human-readable reason the last list/read/size failed (e.g. libsmb2's
 * "NT_STATUS_LOGON_FAILURE", libnfs's "mount refused"). NULL if none. The
 * API layer forwards it to the UI so a failed NAS connect is diagnosable. */
const char *transfer_last_error(transfer_src_t *s);
#endif

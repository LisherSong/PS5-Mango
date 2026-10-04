#pragma once

#include <limits.h>
#include <pthread.h>
#include <stddef.h>
#include <time.h>

#include <microhttpd.h>

#define ETA_SAMPLE_SLOTS 64

typedef enum task_op {
  TASK_COPY,
  TASK_MOVE,
  TASK_DELETE,
  TASK_CHMOD,
  TASK_DOWNLOAD,
  TASK_UPLOAD,
  TASK_EXTRACT,
  TASK_PKG_INSTALL,
} task_op_t;

typedef enum task_state {
  TASK_QUEUED,
  TASK_RUNNING,
  TASK_DONE,
  TASK_FAILED,
  TASK_CANCELED,
} task_state_t;

typedef struct task_eta_sample {
  unsigned long long done;
  struct timespec time;
} task_eta_sample_t;

typedef struct file_task {
  unsigned long id;
  task_op_t op;
  task_state_t state;
  char src[PATH_MAX];
  char dst[PATH_MAX];
  char current[PATH_MAX];
  char error[160];
  char error_code[64];
  char error_arg[PATH_MAX + 96];
  char **srcs;
  size_t src_count;
  char *password;
  char **extract_destinations;
  size_t file_count;
  size_t dir_count;
  size_t upload_completed;
  unsigned int chmod_mode;
  int recursive;
  int extract_overwrite;
  unsigned long long total;
  unsigned long long done;
  unsigned long long speed;
  unsigned long long eta;
  unsigned long long speed_sample_done;
  struct timespec speed_sample_time;
  task_eta_sample_t eta_samples[ETA_SAMPLE_SLOTS];
  unsigned int eta_sample_next;
  unsigned int eta_sample_count;
  int cancel_requested;
  int reported; /* Terminal state has been included in /api/tasks. */
  unsigned int active_streams;
  time_t created_at;
  time_t transfer_started_at;
  time_t updated_at;
  pthread_t thread;
  struct file_task *next;
} file_task_t;

extern pthread_mutex_t g_tasks_lock;
extern file_task_t *g_tasks;
extern unsigned long g_next_task_id;
extern unsigned short g_listen_port;

/* Task threads run deep code on purpose: recursive tree walks (save restore,
 * NAS pulls) and native installer calls (sceAppInstUtilInstallByPackage) whose
 * internal frames we cannot see. The payload's default pthread stack is small
 * (FreeBSD heritage), and an overflow does not return an error — the whole
 * process just vanishes, exactly the "clicked install, process gone" death.
 * See the NEXUS_SAVE_MAX_DEPTH note in nexus_port.c for the earlier encounter.
 * One explicit size for every task-executing thread. 2 MiB: the native
 * installer call chain (sceAppInstUtilInstallByPackage) has frames we cannot
 * see, and stack pages are committed on demand, so headroom is free. */
static inline int
task_thread_create(pthread_t *t, void *(*fn)(void *), void *arg) {
  pthread_attr_t a;
  pthread_attr_init(&a);
  pthread_attr_setstacksize(&a, 2048u * 1024u);
  return pthread_create(t, &a, fn, arg);
}

const char *task_op_name(task_op_t op);
const char *task_state_name(task_state_t state);
int task_is_active(const file_task_t *task);
int has_active_task_locked(void);
int has_active_task(void);
void free_task(file_task_t *task);
void remove_finished_tasks_locked(void);
int task_cancel_requested(file_task_t *task);
/* Run the proven task_worker TASK_DELETE pipeline (count_path_bytes +
 * remove_path) on an explicit path list. Used by the save domain so save
 * deletion and the file manager's delete button share one code path.
 * Returns 0 on success, -1 on failure (errno ECANCELED = canceled). */
int filemgr_delete_run(file_task_t *task, char **paths, size_t count);
file_task_t *find_task_locked(unsigned long id);
void task_update(file_task_t *task, task_state_t state, const char *current,
                 unsigned long long add_done, const char *error);
/* Absolute-progress variant. Engines and the console installer report a
 * running total ("3.2 GB of 7.1 GB", "42 %"), not a delta, so converting to
 * task_update()'s additive form would mean the caller keeps a shadow counter
 * that can drift. total == 0 means "not known yet" and leaves it alone. */
void task_set_progress(file_task_t *task, unsigned long long done,
                       unsigned long long total, const char *current);
/* Fixed denominator (the count-based tasks' denominator: number of entries).
 * Shared with the save-delete worker, which counts entries the same way. */
void task_set_total(file_task_t *task, unsigned long long total);
void record_task_completion_locked(file_task_t *task, time_t completed_at);

/* Unified request parameter reader: JSON body -> form body -> GET query, so a
 * handler accepts either the NEXUS front-end's apiJson (application/json) or
 * apiPost (application/x-www-form-urlencoded) encoding. Returns a heap string
 * the caller must free, or NULL when the key is absent. */
char *req_param(struct MHD_Connection *conn, const char *body, size_t body_size,
                const char *key);

enum MHD_Result send_json_ok(struct MHD_Connection *conn);
enum MHD_Result send_json_error(struct MHD_Connection *conn,
                                unsigned int status, const char *msg);
enum MHD_Result send_json_error_detail(struct MHD_Connection *conn,
                                       unsigned int status, const char *msg,
                                       const char *code, const char *arg);
enum MHD_Result send_buffer(struct MHD_Connection *conn, unsigned int status,
                            char *data, const char *mime);

int ensure_parent_dirs(const char *base, const char *rel);
int chmod_path_mode(const char *path, unsigned int mode);
int chmod_path_0777(const char *path);
int fchmod_0777(int fd);
int ignore_chmod_error(int err);
int mode_access(const char *path, int mode);
/* 1 = "<prefix>/<name>" is a real mount (or the path is not a device root at
 * all, i.e. always allowed); 0 = a device root that is provably NOT mounted.
 * Used by api_list so an empty pre-created mount point cannot advertise a
 * drive that is not attached. */
int fs_device_root_mounted(const char *path);
int check_target_writable(const char *target, char ***checked_dirs,
                          size_t *checked_dir_count,
                          char *error, size_t error_size,
                          char *code, size_t code_size,
                          char *arg, size_t arg_size);
int check_target_space(const char *target, unsigned long long required,
                       char *error, size_t error_size,
                       char *code, size_t code_size,
                       char *arg, size_t arg_size);
int target_available_space(const char *target, unsigned long long *available);
int count_task_path_bytes(file_task_t *task, const char *path,
                          const char *display, unsigned long long *total,
                          size_t *file_count, size_t *dir_count);

enum MHD_Result api_upload_prepare(struct MHD_Connection *conn,
                                   const char *body, size_t body_size);
enum MHD_Result api_upload_finish(struct MHD_Connection *conn);
enum MHD_Result api_download_prepare(struct MHD_Connection *conn,
                                     const char *body, size_t body_size);
enum MHD_Result api_download(struct MHD_Connection *conn);
enum MHD_Result api_list(struct MHD_Connection *conn, const char *body,
                         size_t body_size);
enum MHD_Result api_space(struct MHD_Connection *conn, const char *body,
                          size_t body_size);
enum MHD_Result api_tasks(struct MHD_Connection *conn, const char *body,
                          size_t body_size);
enum MHD_Result api_text(struct MHD_Connection *conn, const char *body,
                         size_t body_size);
enum MHD_Result api_text_create(struct MHD_Connection *conn,
                                const char *body, size_t body_size);
enum MHD_Result api_text_save(struct MHD_Connection *conn, const char *body,
                              size_t body_size);
enum MHD_Result api_delete(struct MHD_Connection *conn, const char *body,
                           size_t body_size);
enum MHD_Result api_rename(struct MHD_Connection *conn, const char *body,
                           size_t body_size);
enum MHD_Result api_mkdir(struct MHD_Connection *conn, const char *body,
                          size_t body_size);
enum MHD_Result api_chmod(struct MHD_Connection *conn, const char *body,
                          size_t body_size);
enum MHD_Result api_copy(struct MHD_Connection *conn, const char *body,
                         size_t body_size);
enum MHD_Result api_move(struct MHD_Connection *conn, const char *body,
                         size_t body_size);
enum MHD_Result api_extract(struct MHD_Connection *conn, const char *body,
                            size_t body_size);
enum MHD_Result api_cancel(struct MHD_Connection *conn, const char *body,
                           size_t body_size);
enum MHD_Result api_exit(struct MHD_Connection *conn, const char *body,
                         size_t body_size);
/* NEXUS-only endpoints (ported in phase 2). Declared here so the dispatch table
 * compiles; real implementations replace the stubs in savemgr.c / pkg_lib.c. */
enum MHD_Result api_app_register(struct MHD_Connection *conn,
                                 const char *body, size_t body_size);
enum MHD_Result api_status(struct MHD_Connection *conn, const char *body,
                           size_t body_size);
enum MHD_Result api_fetch(struct MHD_Connection *conn, const char *body,
                          size_t body_size);
enum MHD_Result api_nas_copy(struct MHD_Connection *conn, const char *body,
                             size_t body_size);
enum MHD_Result api_save_scan(struct MHD_Connection *conn, const char *body,
                              size_t body_size);
enum MHD_Result api_save_list(struct MHD_Connection *conn, const char *body,
                              size_t body_size);
enum MHD_Result api_save_backup(struct MHD_Connection *conn, const char *body,
                                size_t body_size);
enum MHD_Result api_save_restore(struct MHD_Connection *conn, const char *body,
                                 size_t body_size);
enum MHD_Result api_save_delete(struct MHD_Connection *conn, const char *body,
                                size_t body_size);
enum MHD_Result api_save_snapdelete(struct MHD_Connection *conn, const char *body,
                                    size_t body_size);
enum MHD_Result api_pkg_scan(struct MHD_Connection *conn, const char *body,
                             size_t body_size);
enum MHD_Result api_pkg_enqueue(struct MHD_Connection *conn, const char *body,
                                size_t body_size);
enum MHD_Result api_pkg_install_url(struct MHD_Connection *conn, const char *body,
                                    size_t body_size);
enum MHD_Result api_install_poll(struct MHD_Connection *conn, const char *body,
                                 size_t body_size);

/* Queue a single .pkg through the shared PKG worker (nexus_port.c uses this so
 * /api/pkg/enqueue and /api/install-pkg share one queue). Returns 0 and sets
 * *out_id on success, PKG_INSTALL_UNSUPPORTED off-PS5, or -1 on error. */
int filemgr_queue_pkg(const char *path, unsigned long *out_id);

#ifndef NEXUS_COMMON_H
#define NEXUS_COMMON_H
/* Leaf types for the NEXUS transfer layer (src/transfer.c).
 *
 * This is the merged tree's slimmed-down copy of ps5-nexus/src/nexus_common.h:
 * only what transfer.{c,h} actually reads.
 *
 * ⚠️ The task-kind / task-state enums are deliberately NOT copied here. This
 * tree already owns those names (task_op_t in filemgr_internal.h, task_state_t
 * in the same header), and pulling in a second definition of task_state_t would
 * be a redefinition error the moment a TU includes both. Keeping this file to
 * the error enum also keeps the dependency one-directional:
 *   filemgr_internal.h  ->  (task engine)      [no transfer dependency]
 *   nexus_common.h      ->  transfer.h         [no task-engine dependency]
 * Only src/transfer.c includes this header; the HTTP layer talks to it through
 * transfer.h's opaque handle. */
#include <stddef.h>
#include <stdint.h>

typedef enum {
  NEXUS_OK = 0,
  NEXUS_ERR_GENERIC = -1,
  NEXUS_ERR_NOMEM = -2,
  NEXUS_ERR_NOTFOUND = -3,
  NEXUS_ERR_PERM = -4,         /* privilege / AuthID failure */
  NEXUS_ERR_UNSUPPORTED = -5,  /* format/feature not compiled in */
  NEXUS_ERR_CANCELLED = -6,
  NEXUS_ERR_PARTIAL = -7,      /* resume possible (interrupted transfer) */
  NEXUS_ERR_PASSWORD = -8,     /* archive is encrypted: prompt and retry */
  NEXUS_ERR_CONFLICT = -9      /* target exists and policy is "fail" */
} nexus_err_t;

#endif /* NEXUS_COMMON_H */

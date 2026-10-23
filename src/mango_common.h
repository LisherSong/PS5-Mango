#ifndef MANGO_COMMON_H
#define MANGO_COMMON_H
/* Leaf types for the MANGO transfer layer (src/transfer.c).
 *
 * This is the merged tree's slimmed-down copy of PS5-Mango/src/mango_common.h:
 * only what transfer.{c,h} actually reads.
 *
 * ⚠️ The task-kind / task-state enums are deliberately NOT copied here. This
 * tree already owns those names (task_op_t in filemgr_internal.h, task_state_t
 * in the same header), and pulling in a second definition of task_state_t would
 * be a redefinition error the moment a TU includes both. Keeping this file to
 * the error enum also keeps the dependency one-directional:
 *   filemgr_internal.h  ->  (task engine)      [no transfer dependency]
 *   mango_common.h      ->  transfer.h         [no task-engine dependency]
 * Only src/transfer.c includes this header; the HTTP layer talks to it through
 * transfer.h's opaque handle. */
#include <stddef.h>
#include <stdint.h>

typedef enum {
  MANGO_OK = 0,
  MANGO_ERR_GENERIC = -1,
  MANGO_ERR_NOMEM = -2,
  MANGO_ERR_NOTFOUND = -3,
  MANGO_ERR_PERM = -4,         /* privilege / AuthID failure */
  MANGO_ERR_UNSUPPORTED = -5,  /* format/feature not compiled in */
  MANGO_ERR_CANCELLED = -6,
  MANGO_ERR_PARTIAL = -7,      /* resume possible (interrupted transfer) */
  MANGO_ERR_PASSWORD = -8,     /* archive is encrypted: prompt and retry */
  MANGO_ERR_CONFLICT = -9      /* target exists and policy is "fail" */
} mango_err_t;

#endif /* MANGO_COMMON_H */

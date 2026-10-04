#include "filemgr_internal.h"

#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>

#include "json_util.h"
#include "path_util.h"

enum MHD_Result
api_space(struct MHD_Connection *conn, const char *body, size_t body_size) {
  char *current = req_param(conn, body, body_size, "path");
  struct statvfs vfs;
  unsigned long block_size;
  unsigned long long free_bytes;
  unsigned long long total_bytes;
  strbuf_t b = {0};

  if(!current || !*current) {
    free(current);
    current = strdup("/");
  }
  if(!current) {
    return send_json_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, "out of memory");
  }
  if(statvfs(current, &vfs)) {
    free(current);
    return send_json_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR,
                           "cannot read target space");
  }
  block_size = vfs.f_frsize ? vfs.f_frsize : vfs.f_bsize;
  free_bytes = (unsigned long long)vfs.f_bavail * (unsigned long long)block_size;
  total_bytes = (unsigned long long)vfs.f_blocks * (unsigned long long)block_size;
  strbuf_printf(&b, "{\"ok\":true,\"free\":%llu,\"total\":%llu,\"path\":",
                free_bytes, total_bytes);
  json_escape(&b, current);
  strbuf_append(&b, "}");
  free(current);
  return send_buffer(conn, MHD_HTTP_OK, b.data, "application/json");
}

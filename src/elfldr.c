#include "elfldr.h"

/* Lifted verbatim from the removed archive_helper.c: the elfldr protocol is a
 * plain TCP stream on the loopback, unrelated to the helper socket. */

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#define WFM_ELFLDR_PORT 9021
#define WFM_MAX_ELF_SIZE (128LL * 1024LL * 1024LL)

#ifndef __linux__
static int
send_all(int fd, const unsigned char *buffer, size_t size) {
  while(size) {
    ssize_t sent = send(fd, buffer, size, 0);
    if(sent < 0 && errno == EINTR) continue;
    if(sent <= 0) return -1;
    buffer += sent;
    size -= (size_t)sent;
  }
  return 0;
}
#endif

int
elfldr_send(const char *path) {
#ifdef __linux__
  (void)path;
  errno = ENOTSUP;
  return -1;
#else
  struct sockaddr_in address;
  struct timeval timeout = {10, 0};
  struct stat st;
  unsigned char buffer[16384];
  int file_fd = -1;
  int socket_fd = -1;
  int result = -1;

  if(!path) {
    errno = EINVAL;
    return -1;
  }
  if(stat(path, &st)) return -1;
  if(!S_ISREG(st.st_mode) || st.st_size < 4 ||
     st.st_size > WFM_MAX_ELF_SIZE) {
    errno = ENOEXEC;
    return -1;
  }
  {
    int file_fd2 = open(path, O_RDONLY);
    if(file_fd2 < 0) return -1;
    file_fd = file_fd2;
  }
  if(read(file_fd, buffer, 4) != 4 || memcmp(buffer, "\x7f" "ELF", 4)) {
    errno = ENOEXEC;
    goto done;
  }
  if(lseek(file_fd, 0, SEEK_SET) < 0) goto done;
  if((socket_fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) goto done;
  setsockopt(socket_fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
  memset(&address, 0, sizeof(address));
  address.sin_family = AF_INET;
  address.sin_port = htons(WFM_ELFLDR_PORT);
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if(connect(socket_fd, (struct sockaddr *)&address, sizeof(address))) goto done;

  while(1) {
    ssize_t count = read(file_fd, buffer, sizeof(buffer));
    if(count < 0 && errno == EINTR) continue;
    if(count < 0) goto done;
    if(!count) break;
    if(send_all(socket_fd, buffer, (size_t)count)) goto done;
  }
  shutdown(socket_fd, SHUT_WR);
  result = 0;

done:
  if(socket_fd >= 0) close(socket_fd);
  if(file_fd >= 0) close(file_fd);
  return result;
#endif
}

#ifdef __APPLE__
#define _DARWIN_C_SOURCE
#else
#define _GNU_SOURCE
#endif
#include "child_posix.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/stat.h>
#include <unistd.h>

bool fsx_content_path(const char *path) {
  if (*path++ != '/')
    return false;
  if (!*path)
    return true;
  while (*path) {
    size_t n = strcspn(path, "/");
    if (!n || (n == 1 && path[0] == '.') ||
        (n == 2 && path[0] == '.' && path[1] == '.'))
      return false;
    path += n;
    if (*path && !*++path)
      return false;
  }
  return true;
}
FsStatus fsx_content_open_root(FsContentQuery *q, int *out) {
  char *path = fsx_copy(q->engine, q->root, strlen(q->root));
  if (!path)
    return FS_NOMEM;
  int fd = open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  FsStatus status = fd < 0 ? fsx_content_os_status(errno) : FS_OK;
  char *at = path + 1;
  while (status == FS_OK && *at) {
    char *end = strchr(at, '/');
    if (end)
      *end = 0;
    int next = openat(fd, at, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    int error = errno;
    close(fd);
    fd = next;
    if (fd < 0)
      status = fsx_content_os_status(error);
    if (!end)
      break;
    at = end + 1;
  }
  fsx_free(path);
  if (status == FS_OK)
    *out = fd;
  return status;
}
FsStatus fsx_content_check_root(FsContentQuery *q) {
  int fd;
  FsStatus status = fsx_content_open_root(q, &fd);
  if (status != FS_OK)
    return status;
  struct stat info;
  if (fstat(fd, &info))
    status = fsx_content_os_status(errno);
  else if ((uint64_t)info.st_dev != q->root_device ||
           (uint64_t)info.st_ino != q->root_inode)
    status = FS_STALE;
  close(fd);
  return status;
}
static FsStatus path_frame(FsContentQuery *q, char *path) {
  size_t root_length = strlen(q->root);
  if (!*path || !fsx_content_path(path) ||
      (root_length > 1 &&
       (strncmp(path, q->root, root_length) || path[root_length] != '/' ||
        !path[root_length + 1])) ||
      (root_length == 1 && !path[1]))
    return FS_CORRUPT;
  FsEntry entry = {0};
  entry.path = path;
  return fsx_content_visit(q, &entry);
}
FsStatus fsx_content_discover(FsContentQuery *q) {
  char diagnostic_text[192] = {0};
  FsStatus status = fsx_content_open_root(q, &q->root_fd);
  if (status != FS_OK)
    return status;
  struct stat root;
  if (fstat(q->root_fd, &root)) {
    status = fsx_content_os_status(errno);
    goto root_cleanup;
  }
  q->root_device = (uint64_t)root.st_dev;
  q->root_inode = (uint64_t)root.st_ino;
  char *frame = fsx_alloc(q->engine, 65536);
  if (!frame) {
    status = FS_NOMEM;
    goto root_cleanup;
  }
  char *argv[] = {q->bfs,  "-j1",   "-P", "-f",      q->root,
                  "-xdev", "-type", "f",  "-print0", NULL};
  FsChild child;
  status = fsx_child_spawn(q, q->bfs, argv, true, &child);
  if (status != FS_OK)
    goto frame_cleanup;
  close(child.input);
  child.input = -1;
  if (fcntl(child.output, F_SETFL, O_NONBLOCK) < 0 ||
      fcntl(child.error, F_SETFL, O_NONBLOCK) < 0)
    status = FS_IO;
  size_t used = 0, diagnostic = 0;
  unsigned char bytes[8192];
  while (status == FS_OK && (child.output >= 0 || child.error >= 0)) {
    status = fsx_content_stop(q);
    if (status != FS_OK)
      break;
    struct pollfd fds[2] = {{child.output, POLLIN, 0},
                            {child.error, POLLIN, 0}};
    if (poll(fds, 2, 25) < 0) {
      if (errno == EINTR)
        continue;
      status = FS_IO;
      break;
    }
    /* Drain diagnostics before processing paths, which may apply backpressure.
     */
    for (int i = 1; i >= 0 && status == FS_OK; i--) {
      int *stream = i ? &child.error : &child.output;
      if (*stream < 0 || !fds[i].revents)
        continue;
      ssize_t n = read(*stream, bytes, sizeof(bytes));
      if (!n) {
        close(*stream);
        *stream = -1;
      } else if (n < 0) {
        if (errno != EAGAIN && errno != EINTR)
          status = FS_IO;
      } else if (i) {
        size_t copy = sizeof(diagnostic_text) - 1;
        if (diagnostic < copy) {
          copy -= diagnostic;
          if (copy > (size_t)n)
            copy = (size_t)n;
          memcpy(diagnostic_text + diagnostic, bytes, copy);
          diagnostic_text[diagnostic + copy] = 0;
        }
        diagnostic += (size_t)n;
        /* Any traversal diagnostic makes discovery fail explicitly. */
        status = FS_IO;
      } else {
        cmeta_mutex_lock(&q->mutex);
        q->info.discovery_bytes += (uint64_t)n;
        if (q->info.discovery_bytes > q->request.max_discovery_bytes)
          status = FS_LIMIT;
        cmeta_mutex_unlock(&q->mutex);
        for (ssize_t at = 0; status == FS_OK && at < n; at++) {
          if (!bytes[at]) {
            frame[used] = 0;
            status = path_frame(q, frame);
            used = 0;
          } else if (used == 65535)
            status = FS_LIMIT;
          else
            frame[used++] = (char)bytes[at];
        }
      }
    }
  }
  if (status == FS_OK && used)
    status = FS_CORRUPT;
  int exit_code;
  status = fsx_child_finish(q, &child, status, &exit_code);
  if (status == FS_OK && exit_code != 0)
    status = FS_IO;
  if (status == FS_OK)
    status = fsx_content_check_root(q);
frame_cleanup:
  fsx_free(frame);
root_cleanup:
  cmeta_mutex_lock(&q->mutex);
  if (diagnostic_text[0] && !q->info.diagnostic[0])
    memcpy(q->info.diagnostic, diagnostic_text, sizeof(diagnostic_text));
  cmeta_mutex_unlock(&q->mutex);
  return status;
}

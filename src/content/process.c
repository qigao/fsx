#ifdef __APPLE__
#define _DARWIN_C_SOURCE
#else
#define _GNU_SOURCE
#endif
#include "content.h"
#ifdef FSX_POSIX_CONTENT
#include "../platform_stat.h"
#include "child_posix.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <sys/stat.h>

#include <unistd.h>

FsStatus fsx_content_os_status(int error) {
  return error == EACCES || error == EPERM                       ? FS_DENIED
         : error == ENOENT || error == ENOTDIR || error == ELOOP ? FS_STALE
         : error == ENOMEM                                       ? FS_NOMEM
                                                                 : FS_IO;
}
/* Open each component relative to a directory fd, without following links. */
static FsStatus open_file(FsContentQuery *q, const FsEntry *entry, int *out,
                          struct stat *info) {
  char *path = fsx_copy(q->engine, entry->path, strlen(entry->path));
  if (!path)
    return FS_NOMEM;
  int parent = q->root ? fcntl(q->root_fd, F_DUPFD_CLOEXEC, 3)
                       : open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (parent < 0) {
    fsx_free(path);
    return fsx_content_os_status(errno);
  }
  char *p = path + (q->root && strcmp(q->root, "/") ? strlen(q->root) + 1 : 1);
  FsStatus status = FS_OK;
  for (;;) {
    char *slash = strchr(p, '/');
    if (slash)
      *slash = 0;
    int next = openat(parent, p,
                      O_RDONLY | O_NOFOLLOW | O_CLOEXEC |
                          (slash ? O_DIRECTORY : O_NONBLOCK));
    int error = errno;
    close(parent);
    if (next < 0) {
      status = fsx_content_os_status(error);
      break;
    }
    parent = next;
    if (!slash) {
      if (fstat(parent, info))
        status = fsx_content_os_status(errno);
      else if (!S_ISREG(info->st_mode))
        status = FS_UNSUPPORTED;
      else if (q->root ? (uint64_t)info->st_dev != q->root_device
                       : ((uint64_t)info->st_ino != entry->key.file_id_lo ||
                          (uint64_t)info->st_dev != entry->key.file_id_hi))
        status = FS_STALE;
      if (status == FS_OK)
        *out = parent;
      else
        close(parent);
      break;
    }
    if (q->root) {
      struct stat directory;
      if (fstat(parent, &directory))
        status = fsx_content_os_status(errno);
      else if ((uint64_t)directory.st_dev != q->root_device)
        status = FS_UNSUPPORTED;
      if (status != FS_OK) {
        close(parent);
        break;
      }
    }
    p = slash + 1;
  }
  fsx_free(path);
  return status;
}
static bool same_version(const struct stat *a, const struct stat *b) {
  return a->st_dev == b->st_dev && a->st_ino == b->st_ino &&
         a->st_size == b->st_size &&
         fsx_stat_mtime(a).tv_sec == fsx_stat_mtime(b).tv_sec &&
         fsx_stat_mtime(a).tv_nsec == fsx_stat_mtime(b).tv_nsec &&
         fsx_stat_ctime(a).tv_sec == fsx_stat_ctime(b).tv_sec &&
         fsx_stat_ctime(a).tv_nsec == fsx_stat_ctime(b).tv_nsec;
}
static FsStatus execute(FsContentQuery *q, int file, FsContentBatch *b) {
  char *argv[] = {q->tool,
                  "-J1",
                  q->request.mode == FS_CONTENT_LITERAL ? "-F" : "-P",
                  "--color=never",
                  "--line-buffered",
                  "--label=fsx-input",
                  "--format={\"line\":%n,\"offset\":%b,\"text\":%J}%~",
                  "-e",
                  q->pattern,
                  "--",
                  "-",
                  NULL};
  FsChild child;
  FsStatus status = fsx_child_spawn(q, q->tool, argv, false, &child);
  if (status != FS_OK)
    return status;
  int rc = 0;
#ifdef __APPLE__
  int masked = fcntl(child.input, F_SETNOSIGPIPE, 1);
#else
  sigset_t blocked, previous;
  sigemptyset(&blocked);
  sigaddset(&blocked, SIGPIPE);
  int masked = pthread_sigmask(SIG_BLOCK, &blocked, &previous);
#endif
  for (unsigned i = 0; i < 3; i++) {
    int fd = i == 0 ? child.input : i == 1 ? child.output : child.error;
    if (fcntl(fd, F_SETFL, O_NONBLOCK) < 0)
      rc = errno;
  }
  unsigned char *input = fsx_alloc(q->engine, 65536);
  unsigned char output[8192];
  size_t used = 0, sent = 0, diagnostic = 0;
  uint64_t read_bytes = 0;
  bool first = true;
  FsTextState utf8 = {0};
  status = masked || rc ? FS_IO : !input ? FS_NOMEM : FS_OK;
  while (status == FS_OK &&
         (child.input >= 0 || child.output >= 0 || child.error >= 0)) {
    status = fsx_content_stop(q);
    if (status != FS_OK)
      break;
    if (child.input >= 0 && sent == used) {
      ssize_t n = read(file, input, 65536);
      if (n < 0) {
        if (errno == EINTR)
          continue;
        status = fsx_content_os_status(errno);
        break;
      }
      if (!n) {
        if (utf8.remaining) {
          status = FS_UNSUPPORTED;
          break;
        }
        close(child.input);
        child.input = -1;
      } else {
        read_bytes += (uint64_t)n;
        if (read_bytes > q->request.max_file_bytes) {
          status = FS_LIMIT;
          break;
        }
        used = (size_t)n;
        sent = 0;
        if (first && used >= 3 && !memcmp(input, "\xef\xbb\xbf", 3))
          sent = 3;
        first = false;
        if (!fsx_content_text(&utf8, input + sent, used - sent)) {
          status = FS_UNSUPPORTED;
          break;
        }
      }
    }
    struct pollfd fds[3] = {{child.input, POLLOUT, 0},
                            {child.output, POLLIN, 0},
                            {child.error, POLLIN, 0}};
    if (poll(fds, 3, 25) < 0) {
      if (errno == EINTR)
        continue;
      status = FS_IO;
      break;
    }
    if (fds[0].revents && child.input >= 0 && sent < used) {
      ssize_t n = write(child.input, input + sent, used - sent);
      if (n > 0)
        sent += (size_t)n;
      else if (n < 0 && errno != EAGAIN && errno != EINTR) {
        status = FS_IO;
      }
    }
    for (unsigned i = 1; i < 3 && status == FS_OK; i++) {
      int *stream = i == 1 ? &child.output : &child.error;
      if (!fds[i].revents || *stream < 0)
        continue;
      ssize_t n = read(*stream, output, sizeof(output));
      if (!n) {
        close(*stream);
        *stream = -1;
      } else if (n < 0) {
        if (errno != EAGAIN && errno != EINTR)
          status = FS_IO;
      } else if (i == 1) {
        if ((size_t)n > q->request.max_output_bytes - b->used)
          status = FS_LIMIT;
        else {
          memcpy(b->wire + b->used, output, (size_t)n);
          b->used += (size_t)n;
        }
      } else {
        size_t copy = sizeof(b->diagnostic) - 1;
        if (diagnostic < copy) {
          copy -= diagnostic;
          if (copy > (size_t)n)
            copy = (size_t)n;
          memcpy(b->diagnostic + diagnostic, output, copy);
          b->diagnostic[diagnostic + copy] = 0;
        }
        diagnostic += (size_t)n;
        if (diagnostic > 65536)
          status = FS_LIMIT;
      }
    }
  }
  fsx_free(input);
  int exit_code;
  status = fsx_child_finish(q, &child, status, &exit_code);
#ifndef __APPLE__
  if (!masked) {
    if (!sigismember(&previous, SIGPIPE)) {
      sigset_t pending;
      sigpending(&pending);
      if (sigismember(&pending, SIGPIPE)) {
        struct timespec zero = {0};
        sigtimedwait(&blocked, NULL, &zero);
      }
    }
    pthread_sigmask(SIG_SETMASK, &previous, NULL);
  }
#endif
  if (status == FS_OK &&
      (exit_code < 0 || exit_code > 1 || diagnostic ||
       (exit_code == 1 && b->used) || (exit_code == 0 && !b->used)))
    status = FS_IO;
  return status;
}
FsStatus fsx_content_file(FsContentQuery *q, const FsEntry *entry,
                          FsContentBatch *b) {
  if (entry->flags & (FS_ENTRY_DIRECTORY | FS_ENTRY_LINK))
    return FS_UNSUPPORTED;
  int fd = -1;
  struct stat before, after, association;
  FsStatus status = open_file(q, entry, &fd, &before);
  if (status != FS_OK)
    return status;
  if (q->root) {
    b->info.key.file_id_lo = (uint64_t)before.st_ino;
    b->info.key.file_id_hi = (uint64_t)before.st_dev;
  }
  b->info.read_start_ns = cmeta_hrtime();
  status =
      before.st_size < 0 || (uint64_t)before.st_size > q->request.max_file_bytes
          ? FS_LIMIT
          : execute(q, fd, b);
  if (status == FS_OK && (fstat(fd, &after) || !same_version(&before, &after)))
    status = FS_STALE;
  close(fd);
  if (status == FS_OK) {
    status = open_file(q, entry, &fd, &association);
    if (status == FS_OK) {
      if (!same_version(&before, &association))
        status = FS_STALE;
      close(fd);
    }
  }
  if (status == FS_OK && q->root)
    status = fsx_content_check_root(q);
  b->info.read_end_ns = cmeta_hrtime();
  return status;
}
#else
FsStatus fsx_content_file(FsContentQuery *q, const FsEntry *entry,
                          FsContentBatch *b) {
  (void)q;
  (void)entry;
  (void)b;
  return FS_UNSUPPORTED;
}
#endif

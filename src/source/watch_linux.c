#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#include "internal.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/eventfd.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <unistd.h>

struct FsWatch {
  FsEngine *engine;
  FsWatch *next;
  char *root;
  void *buffer;
  uint32_t buffer_bytes, max_directories, directories;
  int root_fd, notifications, cancelled;
  struct stat identity;
  FsWatchInfo info;
  bool dirty, scan_changed;
};

static FsStatus native_status(int error) {
  switch (error) {
  case EACCES:
  case EPERM:
    return FS_DENIED;
  case ENOMEM:
    return FS_NOMEM;
  case ENOSPC:
  case EMFILE:
  case ENFILE:
    return FS_LIMIT;
  case ENOENT:
  case ENOTDIR:
  case ELOOP:
    return FS_STALE;
  case ENOSYS:
    return FS_UNSUPPORTED;
  default:
    return FS_IO;
  }
}
static bool same_identity(const struct stat *a, const struct stat *b) {
  return a->st_dev == b->st_dev && a->st_ino == b->st_ino;
}
static void increment(uint64_t *value) {
  if (*value != UINT64_MAX)
    ++*value;
}
static void dirty(FsWatch *w, bool value) {
  FsEngine *e = w->engine;
  cmeta_mutex_lock(&e->mutex);
  if (w->dirty != value) {
    if (value)
      e->dirty_watches++;
    else
      e->dirty_watches--;
    w->dirty = value;
  }
  fsx_health_refresh(e);
  cmeta_mutex_unlock(&e->mutex);
  w->info.needs_rescan = value;
  if (w->info.state != FS_WATCH_FAILED && w->info.state != FS_WATCH_STOPPED)
    w->info.state = value ? FS_WATCH_DIRTY : FS_WATCH_IDLE;
}
static FsStatus fail(FsWatch *w, FsStatus status, int error) {
  w->info.status = status;
  w->info.native_error = (uint32_t)error;
  w->info.state = status == FS_CANCELLED ? FS_WATCH_STOPPED : FS_WATCH_FAILED;
  dirty(w, true);
  return status;
}
static FsStatus root_identity(FsWatch *w) {
  struct stat current;
  if (lstat(w->root, &current)) {
    int error = errno;
    return fail(w, native_status(error), error);
  }
  if (!S_ISDIR(current.st_mode) || !same_identity(&current, &w->identity))
    return fail(w, FS_STALE, ESTALE);
  return FS_OK;
}
static FsStatus database_outside(FsWatch *w) {
  if (!w->engine->reader_path)
    return FS_OK;
  char *path = realpath(w->engine->reader_path, NULL);
  if (!path)
    return native_status(errno);
  size_t prefix = strlen(w->root);
  bool beneath =
      !strncmp(path, w->root, prefix) &&
      (w->root[prefix - 1] == '/' || path[prefix] == '/' || path[prefix] == 0);
  free(path);
  return beneath ? FS_INVALID : FS_OK;
}
/* The scanner owns a verified directory fd. Follow this specific procfs magic
 * link, never a reconstructed namespace path. No IN_DONT_FOLLOW: that would
 * monitor the procfs link instead of the already-open directory. */
static FsStatus register_directory(FsWatch *w, int notifications, int fd) {
  if (w->directories == w->max_directories) {
    w->info.native_error = ENOSPC;
    return FS_LIMIT;
  }
  char path[64];
  snprintf(path, sizeof(path), "/proc/self/fd/%d", fd);
  int registered =
      inotify_add_watch(notifications, path,
                        IN_ONLYDIR | IN_CREATE | IN_DELETE | IN_MOVED_FROM |
                            IN_MOVED_TO | IN_ATTRIB | IN_MODIFY |
                            IN_CLOSE_WRITE | IN_DELETE_SELF | IN_MOVE_SELF);
  if (registered < 0) {
    int error = errno;
    w->info.native_error = (uint32_t)error;
    return error == ENOENT ? FS_UNSUPPORTED : native_status(error);
  }
  w->directories++;
  return FS_OK;
}
static FsStatus rebuild_monitor(FsWatch *w) {
  int replacement = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
  if (replacement < 0) {
    int error = errno;
    w->info.native_error = (uint32_t)error;
    return native_status(error);
  }
  w->directories = 0;
  FsStatus status = register_directory(w, replacement, w->root_fd);
  if (status != FS_OK) {
    close(replacement);
    return status;
  }
  if (w->notifications >= 0)
    close(w->notifications);
  w->notifications = replacement;
  return FS_OK;
}
static void dispose(FsWatch *w) {
  if (w->notifications >= 0)
    close(w->notifications);
  if (w->root_fd >= 0)
    close(w->root_fd);
  if (w->cancelled >= 0)
    close(w->cancelled);
  fsx_free(w->buffer);
  fsx_free(w->root);
  fsx_free(w);
}
FsStatus fsx_watch_open(FsEngine *e, const char *root,
                        const FsWatchConfig *config, FsWatch **out) {
  if (!out)
    return FS_INVALID;
  *out = NULL;
  if (!e || !root || !root[0] || strlen(root) > 65535 ||
      !fsx_utf8(root, strlen(root)) || !config ||
      config->struct_size != sizeof(*config) ||
      config->api_version != FSX_API_VERSION || config->buffer_bytes < 1024 ||
      config->buffer_bytes > 65536 || config->buffer_bytes % 4 ||
      !config->max_directories || config->max_directories > 1048576)
    return FS_INVALID;
  FsWatch *w = fsx_alloc(e, sizeof(*w));
  if (!w)
    return FS_NOMEM;
  memset(w, 0, sizeof(*w));
  w->engine = e;
  w->root_fd = w->notifications = w->cancelled = -1;
  w->buffer_bytes = config->buffer_bytes;
  w->max_directories = config->max_directories;
  w->info.state = FS_WATCH_DIRTY;
  FsStatus status = fsx_native_root(e, root, &w->root);
  if (status != FS_OK)
    goto failed;
  w->buffer = fsx_alloc(e, w->buffer_bytes);
  if (!w->buffer) {
    status = FS_NOMEM;
    goto failed;
  }
  w->root_fd = open(w->root, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (w->root_fd < 0 || fstat(w->root_fd, &w->identity)) {
    status = native_status(errno);
    goto failed;
  }
  status = database_outside(w);
  if (status != FS_OK)
    goto failed;
  w->cancelled = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  if (w->cancelled < 0) {
    status = native_status(errno);
    goto failed;
  }
  status = rebuild_monitor(w);
  if (status != FS_OK)
    goto failed;
  cmeta_mutex_lock(&e->mutex);
  status = atomic_load(&e->stopping) || e->handles >= e->config.max_handles
               ? FS_BUSY
               : FS_OK;
  for (FsWatch *other = e->watches; status == FS_OK && other;
       other = other->next)
    if (same_identity(&w->identity, &other->identity))
      status = FS_BUSY;
  if (status == FS_OK) {
    w->next = e->watches;
    e->watches = w;
    w->dirty = true;
    w->info.needs_rescan = true;
    e->handles++;
    e->watch_count++;
    e->dirty_watches++;
    fsx_health_refresh(e);
  }
  cmeta_mutex_unlock(&e->mutex);
  if (status == FS_OK) {
    *out = w;
    return FS_OK;
  }
failed:
  dispose(w);
  return status;
}
static FsStatus pump(FsWatch *w, bool *quiet) {
  *quiet = false;
  if (atomic_load(&w->engine->stopping))
    return fail(w, FS_CANCELLED, ECANCELED);
  if (w->info.state == FS_WATCH_FAILED || w->info.state == FS_WATCH_STOPPED)
    return w->info.status;
  FsStatus status = root_identity(w);
  if (status != FS_OK)
    return status;
  for (unsigned batch = 0; batch < 16; batch++) {
    ssize_t bytes = read(w->notifications, w->buffer, w->buffer_bytes);
    if (bytes < 0) {
      int error = errno;
      if (error == EAGAIN) {
        *quiet = true;
        return FS_OK;
      }
      if (error == EINTR)
        continue;
      return fail(w, native_status(error), error);
    }
    w->scan_changed = true;
    dirty(w, true);
    increment(&w->info.change_batches);
    for (size_t offset = 0; offset < (size_t)bytes;) {
      const struct inotify_event *event =
          (const void *)((char *)w->buffer + offset);
      if (event->mask & IN_Q_OVERFLOW)
        increment(&w->info.overflows);
      if (event->mask & IN_UNMOUNT)
        return fail(w, FS_STALE, ESTALE);
      offset += sizeof(*event) + event->len;
    }
  }
  return FS_OK;
}
FsStatus fsx_watch_wait(FsWatch *w, uint64_t deadline, FsWatchInfo *out) {
  if (!w || !out)
    return FS_INVALID;
  FsStatus status;
  for (;;) {
    bool quiet;
    status = pump(w, &quiet);
    if (status != FS_OK || w->dirty)
      break;
    if (!deadline) {
      status = FS_WAIT;
      break;
    }
    uint64_t now = cmeta_hrtime();
    if (deadline != UINT64_MAX && now >= deadline) {
      status = FS_TIMEOUT;
      break;
    }
    int millis = 250;
    if (deadline != UINT64_MAX) {
      uint64_t remaining = deadline - now;
      uint64_t rounded = remaining / 1000000 + (remaining % 1000000 != 0);
      if (rounded < (uint64_t)millis)
        millis = (int)rounded;
    }
    struct pollfd descriptors[] = {{w->notifications, POLLIN, 0},
                                   {w->cancelled, POLLIN, 0}};
    if (poll(descriptors, 2, millis) < 0 && errno != EINTR) {
      int error = errno;
      status = fail(w, native_status(error), error);
      break;
    }
  }
  *out = w->info;
  return status;
}
static FsStatus checkpoint(void *context) {
  bool quiet;
  return pump(context, &quiet);
}
static FsStatus enter_directory(void *context, int descriptor, unsigned depth) {
  FsWatch *w = context;
  FsStatus status = checkpoint(w);
  if (status != FS_OK)
    return status;
  if (!depth) {
    struct stat current;
    if (fstat(descriptor, &current)) {
      int error = errno;
      return fail(w, native_status(error), error);
    }
    return same_identity(&current, &w->identity) ? FS_OK
                                                 : fail(w, FS_STALE, ESTALE);
  }
  return register_directory(w, w->notifications, descriptor);
}
FsStatus fsx_watch_scan(FsWatch *w, FsScanInfo *out) {
  if (!w || !out)
    return FS_INVALID;
  *out = (FsScanInfo){0};
  bool quiet;
  FsStatus status = pump(w, &quiet);
  if (status != FS_OK)
    return status;
  dirty(w, true);
  w->scan_changed = false;
  status = rebuild_monitor(w);
  if (status == FS_OK) {
    w->info.state = FS_WATCH_SCANNING;
    FsScanHooks hooks = {enter_directory, checkpoint, w};
    status = fsx_engine_scan_watched(w->engine, w->root, out, &hooks);
  }
  if (w->info.state == FS_WATCH_FAILED || w->info.state == FS_WATCH_STOPPED)
    return status;
  w->info.status = status;
  if (status != FS_OK) {
    dirty(w, true);
    return status;
  }
  w->info.snapshot_epoch = out->snapshot_epoch;
  w->info.source_id = out->source_id;
  w->info.coverage_complete = out->coverage_complete;
  increment(&w->info.rescans);
  status = pump(w, &quiet);
  if (status != FS_OK)
    return status;
  dirty(w, w->scan_changed || !quiet || !out->coverage_complete);
  return FS_OK;
}
void fsx_watch_cancel_all(FsEngine *e) {
  for (FsWatch *w = e->watches; w; w = w->next) {
    uint64_t signal = 1;
    (void)write(w->cancelled, &signal, sizeof(signal));
  }
}
FsStatus fsx_watch_close(FsWatch **owner) {
  if (!owner || !*owner)
    return FS_INVALID;
  FsWatch *w = *owner;
  FsEngine *e = w->engine;
  cmeta_mutex_lock(&e->mutex);
  FsWatch **slot = &e->watches;
  while (*slot && *slot != w)
    slot = &(*slot)->next;
  *slot = w->next;
  e->watch_count--;
  if (w->dirty)
    e->dirty_watches--;
  fsx_health_invalidate(e, w->root);
  cmeta_mutex_unlock(&e->mutex);
  dispose(w);
  *owner = NULL;
  cmeta_mutex_lock(&e->mutex);
  e->handles--;
  cmeta_cond_broadcast(&e->changed);
  cmeta_mutex_unlock(&e->mutex);
  return FS_OK;
}

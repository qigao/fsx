#define _DARWIN_C_SOURCE
#include "internal.h"
#include <CoreServices/CoreServices.h>
#include <dispatch/dispatch.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

struct FsWatch {
  FsEngine *engine;
  FsWatch *next;
  char *root;
  int root_fd;
  struct stat identity;
  FSEventStreamRef stream;
  dispatch_queue_t queue;
  dispatch_semaphore_t ready, released;
  cmeta_mutex_t bridge;
  FSEventStreamEventFlags *events, overflow_flags;
  uint32_t capacity, used;
  FsWatchInfo info;
  bool started, notified, overflow, dirty, scan_changed;
};

static FsStatus native_status(int error) {
  switch (error) {
  case EACCES:
  case EPERM:
    return FS_DENIED;
  case ENOMEM:
    return FS_NOMEM;
  case EMFILE:
  case ENFILE:
    return FS_LIMIT;
  case ENOENT:
  case ENOTDIR:
  case ELOOP:
    return FS_STALE;
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
  char *slash = strrchr(path, '/');
  slash[slash == path ? 1 : 0] = 0;
  int fd = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  int error = errno;
  free(path);
  if (fd < 0)
    return native_status(error);
  FsStatus status = FS_OK;
  struct stat current;
  if (fstat(fd, &current))
    status = native_status(errno);
  while (status == FS_OK) {
    if (same_identity(&current, &w->identity)) {
      status = FS_INVALID;
      break;
    }
    int parent = openat(fd, "..", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (parent < 0) {
      status = native_status(errno);
      break;
    }
    struct stat ancestor;
    int rc = fstat(parent, &ancestor);
    error = errno;
    close(fd);
    fd = parent;
    if (rc) {
      status = native_status(error);
      break;
    }
    if (same_identity(&current, &ancestor))
      break;
    current = ancestor;
  }
  close(fd);
  return status;
}
/* The native queue only transports flags. It never touches engine health,
 * SQLite, snapshots, paths or the host-owned watch state machine. */
static void receive(ConstFSEventStreamRef stream, void *context, size_t count,
                    void *paths, const FSEventStreamEventFlags flags[],
                    const FSEventStreamEventId ids[]) {
  (void)stream;
  (void)paths;
  (void)ids;
  FsWatch *w = context;
  cmeta_mutex_lock(&w->bridge);
  for (size_t i = 0; i < count; i++) {
    if (flags[i] == kFSEventStreamEventFlagHistoryDone)
      continue;
    if (w->used < w->capacity)
      w->events[w->used++] = flags[i];
    else {
      w->overflow = true;
      w->overflow_flags |= flags[i];
    }
  }
  if ((w->used || w->overflow) && !w->notified) {
    w->notified = true;
    dispatch_semaphore_signal(w->ready);
  }
  cmeta_mutex_unlock(&w->bridge);
}
static const void *retain_context(const void *context) { return context; }
static void release_context(const void *context) {
  FsWatch *w = (FsWatch *)context;
  dispatch_semaphore_signal(w->released);
}
static void stop_stream(void *context) {
  FsWatch *w = context;
  if (w->started)
    FSEventStreamStop(w->stream);
  FSEventStreamInvalidate(w->stream);
  FSEventStreamRelease(w->stream);
}
static void dispose(FsWatch *w) {
  if (w->stream) {
    dispatch_sync_f(w->queue, w, stop_stream);
    /* The context release acknowledges the stream's final retain, including
     * native work still holding it. Callback storage stays alive until then. */
    dispatch_semaphore_wait(w->released, DISPATCH_TIME_FOREVER);
  }
  if (w->queue)
    dispatch_release(w->queue);
  if (w->ready)
    dispatch_release(w->ready);
  if (w->released)
    dispatch_release(w->released);
  if (w->bridge)
    cmeta_mutex_destroy(&w->bridge);
  if (w->root_fd >= 0)
    close(w->root_fd);
  fsx_free(w->events);
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
  w->root_fd = -1;
  w->capacity = config->buffer_bytes / sizeof(*w->events);
  w->info.state = FS_WATCH_DIRTY;
  FsStatus status = fsx_native_root(e, root, &w->root);
  if (status != FS_OK)
    goto failed;
  w->events = fsx_alloc(e, config->buffer_bytes);
  if (!w->events) {
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
  cmeta_mutex_init(&w->bridge);
  w->queue = dispatch_queue_create("fsx.fsevents", DISPATCH_QUEUE_SERIAL);
  w->ready = dispatch_semaphore_create(0);
  w->released = dispatch_semaphore_create(0);
  if (!w->bridge || !w->queue || !w->ready || !w->released) {
    status = FS_NOMEM;
    goto failed;
  }
  CFStringRef path = CFStringCreateWithCString(kCFAllocatorDefault, w->root,
                                               kCFStringEncodingUTF8);
  if (!path) {
    status = FS_NOMEM;
    goto failed;
  }
  CFArrayRef paths = CFArrayCreate(kCFAllocatorDefault, (const void **)&path, 1,
                                   &kCFTypeArrayCallBacks);
  CFRelease(path);
  if (!paths) {
    status = FS_NOMEM;
    goto failed;
  }
  FSEventStreamContext context = {0, w, retain_context, release_context, NULL};
  w->stream = FSEventStreamCreate(kCFAllocatorDefault, receive, &context, paths,
                                  kFSEventStreamEventIdSinceNow, 0.05,
                                  kFSEventStreamCreateFlagWatchRoot |
                                      kFSEventStreamCreateFlagFileEvents |
                                      kFSEventStreamCreateFlagNoDefer);
  CFRelease(paths);
  if (!w->stream) {
    status = FS_IO;
    goto failed;
  }
  FSEventStreamSetDispatchQueue(w->stream, w->queue);
  w->started = FSEventStreamStart(w->stream);
  if (!w->started) {
    status = FS_IO;
    goto failed;
  }
  struct stat current;
  if (lstat(w->root, &current))
    status = native_status(errno);
  else if (!S_ISDIR(current.st_mode) || !same_identity(&current, &w->identity))
    status = FS_STALE;
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
static FsStatus pump(FsWatch *w, bool flush) {
  if (atomic_load(&w->engine->stopping))
    return fail(w, FS_CANCELLED, ECANCELED);
  if (w->info.state == FS_WATCH_FAILED || w->info.state == FS_WATCH_STOPPED)
    return w->info.status;
  FsStatus status = root_identity(w);
  if (status != FS_OK)
    return status;
  if (flush)
    FSEventStreamFlushSync(w->stream);
  cmeta_mutex_lock(&w->bridge);
  bool changed = w->used || w->overflow;
  bool overflow = w->overflow;
  FSEventStreamEventFlags flags = w->overflow_flags;
  for (uint32_t i = 0; i < w->used; i++)
    flags |= w->events[i];
  w->used = 0;
  w->overflow = false;
  w->overflow_flags = 0;
  w->notified = false;
  (void)dispatch_semaphore_wait(w->ready, DISPATCH_TIME_NOW);
  cmeta_mutex_unlock(&w->bridge);
  if (atomic_load(&w->engine->stopping))
    return fail(w, FS_CANCELLED, ECANCELED);
  if (changed) {
    w->scan_changed = true;
    dirty(w, true);
    increment(&w->info.change_batches);
  }
  if (overflow || (flags & (kFSEventStreamEventFlagMustScanSubDirs |
                            kFSEventStreamEventFlagUserDropped |
                            kFSEventStreamEventFlagKernelDropped |
                            kFSEventStreamEventFlagEventIdsWrapped)))
    increment(&w->info.overflows);
  if (flags & (kFSEventStreamEventFlagRootChanged |
               kFSEventStreamEventFlagUnmount | kFSEventStreamEventFlagMount))
    return fail(w, FS_STALE, ESTALE);
  return root_identity(w);
}
FsStatus fsx_watch_wait(FsWatch *w, uint64_t deadline, FsWatchInfo *out) {
  if (!w || !out)
    return FS_INVALID;
  FsStatus status;
  for (;;) {
    status = pump(w, true);
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
    dispatch_time_t until = DISPATCH_TIME_FOREVER;
    if (deadline != UINT64_MAX) {
      uint64_t remaining = deadline - now;
      until =
          dispatch_time(DISPATCH_TIME_NOW,
                        remaining > INT64_MAX ? INT64_MAX : (int64_t)remaining);
    }
    (void)dispatch_semaphore_wait(w->ready, until);
  }
  *out = w->info;
  return status;
}
static FsStatus checkpoint(void *context) { return pump(context, false); }
static FsStatus enter_directory(void *context, int fd, unsigned depth) {
  FsWatch *w = context;
  FsStatus status = checkpoint(w);
  if (status != FS_OK || depth)
    return status;
  struct stat current;
  if (fstat(fd, &current)) {
    int error = errno;
    return fail(w, native_status(error), error);
  }
  return same_identity(&current, &w->identity) ? FS_OK
                                               : fail(w, FS_STALE, ESTALE);
}
FsStatus fsx_watch_scan(FsWatch *w, FsScanInfo *out) {
  if (!w || !out)
    return FS_INVALID;
  *out = (FsScanInfo){0};
  FsStatus status = pump(w, true);
  if (status != FS_OK)
    return status;
  dirty(w, true);
  w->scan_changed = false;
  w->info.state = FS_WATCH_SCANNING;
  FsScanHooks hooks = {enter_directory, checkpoint, w};
  status = fsx_engine_scan_watched(w->engine, w->root, out, &hooks);
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
  status = pump(w, true);
  if (status != FS_OK)
    return status;
  dirty(w, w->scan_changed || !out->coverage_complete);
  return FS_OK;
}
void fsx_watch_cancel_all(FsEngine *e) {
  for (FsWatch *w = e->watches; w; w = w->next)
    dispatch_semaphore_signal(w->ready);
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

#include "internal.h"

FsWatchConfig fsx_watch_config(void) {
  FsWatchConfig config = {sizeof(config), FSX_API_VERSION, 65536, 65536};
  return config;
}

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <wchar.h>
#include <windows.h>

struct FsWatch {
  FsEngine *engine;
  FsWatch *next;
  char *root;
  wchar_t *wide_root;
  void *buffer;
  DWORD buffer_bytes;
  HANDLE directory, completed, cancelled;
  OVERLAPPED read;
  BY_HANDLE_FILE_INFORMATION identity;
  FsWatchInfo info;
  bool dirty, pending;
};
static FsStatus native_status(DWORD error) {
  if (error == ERROR_ACCESS_DENIED || error == ERROR_SHARING_VIOLATION)
    return FS_DENIED;
  if (error == ERROR_NOT_ENOUGH_MEMORY || error == ERROR_OUTOFMEMORY)
    return FS_NOMEM;
  if (error == ERROR_INVALID_FUNCTION || error == ERROR_NOT_SUPPORTED)
    return FS_UNSUPPORTED;
  if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ||
      error == ERROR_DELETE_PENDING)
    return FS_STALE;
  return FS_IO;
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
static FsStatus fail(FsWatch *w, FsStatus status, DWORD error) {
  w->info.status = status;
  w->info.native_error = error;
  w->info.state = status == FS_CANCELLED ? FS_WATCH_STOPPED : FS_WATCH_FAILED;
  dirty(w, true);
  return status;
}
static bool same_identity(const BY_HANDLE_FILE_INFORMATION *a,
                          const BY_HANDLE_FILE_INFORMATION *b) {
  return a->dwVolumeSerialNumber == b->dwVolumeSerialNumber &&
         a->nFileIndexHigh == b->nFileIndexHigh &&
         a->nFileIndexLow == b->nFileIndexLow;
}
static FsStatus root_identity(FsWatch *w) {
  HANDLE path = CreateFileW(
      w->wide_root, FILE_READ_ATTRIBUTES,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
      OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
      NULL);
  if (path == INVALID_HANDLE_VALUE) {
    DWORD error = GetLastError();
    return fail(w, native_status(error), error);
  }
  BY_HANDLE_FILE_INFORMATION identity;
  BOOL read = GetFileInformationByHandle(path, &identity);
  DWORD error = GetLastError();
  CloseHandle(path);
  if (!read)
    return fail(w, native_status(error), error);
  if (!(identity.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
      identity.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT ||
      !same_identity(&identity, &w->identity))
    return fail(w, FS_STALE, ERROR_FILE_INVALID);
  return FS_OK;
}
/* Compare final native paths so relative paths, case and long-path prefixes do
 * not bypass the self-invalidation check. */
static FsStatus database_outside(FsWatch *w) {
  FsEngine *e = w->engine;
  if (!e->reader_path)
    return FS_OK;
  wchar_t *path = fsx_wide(e, e->reader_path);
  if (!path)
    return FS_NOMEM;
  HANDLE file =
      CreateFileW(path, FILE_READ_ATTRIBUTES,
                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                  OPEN_EXISTING, 0, NULL);
  fsx_free(path);
  if (file == INVALID_HANDLE_VALUE)
    return native_status(GetLastError());
  DWORD length = GetFinalPathNameByHandleW(
      file, NULL, 0, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
  if (!length || length > 32767) {
    CloseHandle(file);
    return FS_IO;
  }
  wchar_t *resolved = fsx_alloc(e, ((size_t)length + 1) * sizeof(*resolved));
  if (!resolved) {
    CloseHandle(file);
    return FS_NOMEM;
  }
  DWORD written = GetFinalPathNameByHandleW(
      file, resolved, length + 1, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
  CloseHandle(file);
  FsStatus status = FS_OK;
  if (!written || written > length)
    status = FS_IO;
  else {
    size_t prefix = wcslen(w->wide_root);
    bool beneath = written >= prefix &&
                   CompareStringOrdinal(resolved, (int)prefix, w->wide_root,
                                        (int)prefix, TRUE) == CSTR_EQUAL &&
                   (w->wide_root[prefix - 1] == L'\\' ||
                    resolved[prefix] == L'\\' || resolved[prefix] == 0);
    if (beneath)
      status = FS_INVALID;
  }
  fsx_free(resolved);
  return status;
}
static FsStatus arm(FsWatch *w) {
  ResetEvent(w->completed);
  memset(&w->read, 0, sizeof(w->read));
  w->read.hEvent = w->completed;
  BOOL queued = ReadDirectoryChangesW(
      w->directory, w->buffer, w->buffer_bytes, TRUE,
      FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
          FILE_NOTIFY_CHANGE_ATTRIBUTES | FILE_NOTIFY_CHANGE_SIZE |
          FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_CREATION |
          FILE_NOTIFY_CHANGE_SECURITY,
      NULL, &w->read, NULL);
  if (!queued)
    return native_status(GetLastError());
  w->pending = true;
  return FS_OK;
}
static void dispose(FsWatch *w) {
  if (w->pending) {
    CancelIoEx(w->directory, &w->read);
    DWORD bytes;
    /* Cancellation is only a request. Buffer/read storage remains live until
     * completion has been observed, including an already completed request. */
    GetOverlappedResult(w->directory, &w->read, &bytes, TRUE);
  }
  if (w->directory != INVALID_HANDLE_VALUE)
    CloseHandle(w->directory);
  if (w->completed)
    CloseHandle(w->completed);
  if (w->cancelled)
    CloseHandle(w->cancelled);
  fsx_free(w->buffer);
  fsx_free(w->wide_root);
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
  w->directory = INVALID_HANDLE_VALUE;
  w->info.state = FS_WATCH_DIRTY;
  w->buffer_bytes = config->buffer_bytes;
  FsStatus status = fsx_native_root(e, root, &w->root);
  if (status != FS_OK)
    goto failed;
  w->wide_root = fsx_wide(e, w->root);
  w->buffer = fsx_alloc(e, w->buffer_bytes);
  if (!w->wide_root || !w->buffer) {
    status = FS_NOMEM;
    goto failed;
  }
  w->directory =
      CreateFileW(w->wide_root, FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES,
                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                  OPEN_EXISTING,
                  FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT |
                      FILE_FLAG_OVERLAPPED,
                  NULL);
  if (w->directory == INVALID_HANDLE_VALUE) {
    status = native_status(GetLastError());
    goto failed;
  }
  if (!GetFileInformationByHandle(w->directory, &w->identity)) {
    status = native_status(GetLastError());
    goto failed;
  }
  if (!(w->identity.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
      w->identity.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
    status = FS_UNSUPPORTED;
    goto failed;
  }
  status = database_outside(w);
  if (status != FS_OK)
    goto failed;
  w->completed = CreateEventW(NULL, TRUE, FALSE, NULL);
  w->cancelled = CreateEventW(NULL, TRUE, FALSE, NULL);
  if (!w->completed || !w->cancelled) {
    status = native_status(GetLastError());
    goto failed;
  }
  status = arm(w);
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
    e->needs_rescan = true;
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
static FsStatus pump(FsWatch *w, bool *changed, bool *quiet) {
  *changed = false;
  *quiet = false;
  if (atomic_load(&w->engine->stopping))
    return fail(w, FS_CANCELLED, ERROR_OPERATION_ABORTED);
  if (w->info.state == FS_WATCH_FAILED || w->info.state == FS_WATCH_STOPPED)
    return w->info.status;
  FsStatus status = root_identity(w);
  if (status != FS_OK)
    return status;
  for (unsigned batch = 0; batch < 16; batch++) {
    DWORD bytes = 0;
    BOOL complete = GetOverlappedResult(w->directory, &w->read, &bytes, FALSE);
    DWORD error = complete ? ERROR_SUCCESS : GetLastError();
    if (!complete && error == ERROR_IO_INCOMPLETE) {
      *quiet = true;
      return FS_OK;
    }
    w->pending = false;
    if (!complete && error != ERROR_NOTIFY_ENUM_DIR &&
        error != ERROR_OPERATION_ABORTED)
      return fail(w, native_status(error), error);
    if (error == ERROR_OPERATION_ABORTED && atomic_load(&w->engine->stopping))
      return fail(w, FS_CANCELLED, error);
    *changed = true;
    dirty(w, true);
    increment(&w->info.change_batches);
    /* A serialized caller can exit after arming this event-based I/O. Windows
     * cancels that request; rearm on the new caller and require reconciliation.
     */
    if (error == ERROR_OPERATION_ABORTED) {
      increment(&w->info.interruptions);
      w->info.native_error = error;
    } else if (!complete || !bytes)
      increment(&w->info.overflows);
    status = arm(w);
    if (status != FS_OK)
      return fail(w, status, GetLastError());
  }
  return FS_OK;
}
FsStatus fsx_watch_wait(FsWatch *w, uint64_t deadline, FsWatchInfo *out) {
  if (!w || !out)
    return FS_INVALID;
  FsStatus status;
  for (;;) {
    bool changed, quiet;
    status = pump(w, &changed, &quiet);
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
    DWORD millis =
        250; /* Also revalidate a moved/deleted root without a child event. */
    if (deadline != UINT64_MAX) {
      uint64_t remaining = deadline - now;
      uint64_t rounded = remaining / 1000000 + (remaining % 1000000 != 0);
      if (rounded < millis)
        millis = (DWORD)rounded;
    }
    HANDLE events[] = {w->cancelled, w->completed};
    DWORD waited = WaitForMultipleObjects(2, events, FALSE, millis);
    if (waited == WAIT_FAILED) {
      DWORD error = GetLastError();
      status = fail(w, native_status(error), error);
      break;
    }
  }
  *out = w->info;
  return status;
}
FsStatus fsx_watch_scan(FsWatch *w, FsScanInfo *out) {
  if (!w || !out)
    return FS_INVALID;
  *out = (FsScanInfo){0};
  bool changed, quiet;
  FsStatus status = pump(w, &changed, &quiet);
  if (status != FS_OK)
    return status;
  dirty(w, true);
  w->info.state = FS_WATCH_SCANNING;
  status = fsx_engine_scan(w->engine, w->root, out);
  w->info.status = status;
  if (status != FS_OK) {
    w->info.state = FS_WATCH_DIRTY;
    return status;
  }
  w->info.snapshot_epoch = out->snapshot_epoch;
  w->info.source_id = out->source_id;
  w->info.coverage_complete = out->coverage_complete;
  increment(&w->info.rescans);
  status = pump(w, &changed, &quiet);
  if (status != FS_OK)
    return status;
  dirty(w, changed || !quiet);
  return FS_OK;
}
void fsx_watch_cancel_all(FsEngine *e) {
  for (FsWatch *w = e->watches; w; w = w->next)
    SetEvent(w->cancelled);
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
  dispose(
      w); /* Keep the external slot until allocator/native cleanup is done. */
  *owner = NULL;
  cmeta_mutex_lock(&e->mutex);
  e->handles--;
  cmeta_cond_broadcast(&e->changed);
  cmeta_mutex_unlock(&e->mutex);
  return FS_OK;
}
#elif !defined(__linux__)
FsStatus fsx_watch_open(FsEngine *e, const char *root,
                        const FsWatchConfig *config, FsWatch **out) {
  (void)e;
  (void)root;
  (void)config;
  if (!out)
    return FS_INVALID;
  *out = NULL;
  return FS_UNSUPPORTED;
}
FsStatus fsx_watch_wait(FsWatch *w, uint64_t deadline, FsWatchInfo *out) {
  (void)w;
  (void)deadline;
  (void)out;
  return FS_UNSUPPORTED;
}
FsStatus fsx_watch_scan(FsWatch *w, FsScanInfo *out) {
  (void)w;
  if (out)
    *out = (FsScanInfo){0};
  return FS_UNSUPPORTED;
}
FsStatus fsx_watch_close(FsWatch **w) {
  (void)w;
  return FS_UNSUPPORTED;
}
void fsx_watch_cancel_all(FsEngine *e) { (void)e; }
#endif

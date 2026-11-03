#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#endif
#include "../src/internal.h"
#include "../src/source/ntfs/ntfs.h"
#include <fsx/api.h>
#include <sqlite3.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <thread.h>
#include <tinytest.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <wchar.h>
#include <windows.h>
#include <winioctl.h>
#else
#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
static char fixture[1024], root[1200], database[1200];
static FsEngine *engine, *oracle_engine;
static bool fixture_ready;
static FsQuery *held_query;
static FsResult *held_result, *retained_result;
static FsWatch *watch, *second_watch;
static sqlite3 *scan_blocker;
static FsIndexJob *held_index;
typedef struct IndexGate {
  cmeta_mutex_t mutex;
  cmeta_cond_t changed;
  bool entered, released, native, armed;
} IndexGate;
static IndexGate index_gate;
static void gate_start(bool native) {
  memset(&index_gate, 0, sizeof(index_gate));
  cmeta_mutex_init(&index_gate.mutex);
  cmeta_cond_init(&index_gate.changed);
  check_not_null(index_gate.mutex);
  check_not_null(index_gate.changed);
  index_gate.native = native;
  index_gate.armed = true;
}
static int pause_index(void *context) {
  (void)context;
  if (!index_gate.armed)
    return 0;
  cmeta_mutex_lock(&index_gate.mutex);
  index_gate.entered = true;
  cmeta_cond_broadcast(&index_gate.changed);
  while (!index_gate.released && !fsx_writer_stopping(engine))
    cmeta_cond_timedwait(&index_gate.changed, &index_gate.mutex, 10000000);
  bool cancelled = fsx_writer_stopping(engine);
  cmeta_mutex_unlock(&index_gate.mutex);
  return cancelled ? 1 : 0;
}
static void occupy_index_worker(void *context) { (void)pause_index(context); }
static int pause_inventory(unsigned event, void *context, void *statement,
                           void *duration) {
  (void)context;
  (void)duration;
  if (event == SQLITE_TRACE_STMT) {
    const char *text = sqlite3_sql(statement);
    index_gate.armed = text && strstr(text, "INSERT INTO nt_inventory VALUES");
  }
  return 0;
}
static void gate_wait(void) {
  uint64_t deadline = cmeta_hrtime() + UINT64_C(5000000000);
  cmeta_mutex_lock(&index_gate.mutex);
  while (!index_gate.entered && cmeta_hrtime() < deadline)
    cmeta_cond_timedwait(&index_gate.changed, &index_gate.mutex, 10000000);
  bool entered = index_gate.entered;
  cmeta_mutex_unlock(&index_gate.mutex);
  check_true(entered);
}
static void gate_release(void) {
  if (!index_gate.mutex)
    return;
  cmeta_mutex_lock(&index_gate.mutex);
  index_gate.released = true;
  cmeta_cond_broadcast(&index_gate.changed);
  cmeta_mutex_unlock(&index_gate.mutex);
}
static void finish_index(void) {
  gate_release();
  if (held_index) {
    fsx_index_cancel(held_index);
    FsStatus status =
        fsx_index_wait(held_index, cmeta_hrtime() + UINT64_C(5000000000));
    check(status != FS_WAIT && status != FS_TIMEOUT, "index cleanup completed");
    fsx_index_release(held_index);
    held_index = NULL;
  }
}

#ifdef _WIN32
static void wide(const char *s, wchar_t *out, size_t cap) {
  check(MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, out,
                            (int)cap) > 0,
        "UTF-8 fixture path");
}
#endif
static void mkdir_utf8(const char *s) {
#ifdef _WIN32
  wchar_t w[2048];
  wide(s, w, 2048);
  check(CreateDirectoryW(w, NULL) != 0, "create fixture directory");
#else
  check_equal(mkdir(s, 0700), 0);
#endif
}
static void remove_utf8(const char *s) {
#ifdef _WIN32
  wchar_t w[2048];
  wide(s, w, 2048);
  check(DeleteFileW(w) != 0, "remove fixture file");
#else
  check_equal(unlink(s), 0);
#endif
}
static void put_path(const char *path, const char *content) {
  size_t prefix = strlen(fixture);
  check(fixture_ready && prefix && !strncmp(path, fixture, prefix) &&
            (path[prefix] == '/' || path[prefix] == '\\'),
        "owned fixture write");
#ifdef _WIN32
  wchar_t w[2048];
  wide(path, w, 2048);
  FILE *file = _wfopen(w, L"wb");
#else
  FILE *file = fopen(path, "wb");
#endif
  check_not_null(file);
  check_equal(fwrite(content, 1, strlen(content), file), strlen(content));
  check_equal(fclose(file), 0);
}
static void put(const char *name, const char *content) {
  char path[2048];
  snprintf(path, sizeof(path), "%s/%s", root, name);
  put_path(path, content);
}
static void subdir(const char *name) {
  char p[2048];
  snprintf(p, sizeof(p), "%s/%s", root, name);
  mkdir_utf8(p);
}
static void cleanup_tree(const char *path) {
  size_t prefix = strlen(fixture);
  if (!fixture_ready || !prefix || strncmp(path, fixture, prefix) ||
      (path[prefix] && path[prefix] != '/' && path[prefix] != '\\'))
    return;
#ifdef _WIN32
  wchar_t w[2048], pattern[2050];
  wide(path, w, 2048);
  swprintf(pattern, 2050, L"%ls/*", w);
  WIN32_FIND_DATAW d;
  HANDLE h = FindFirstFileW(pattern, &d);
  if (h != INVALID_HANDLE_VALUE) {
    do {
      if (!wcscmp(d.cFileName, L".") || !wcscmp(d.cFileName, L".."))
        continue;
      wchar_t child[4096];
      swprintf(child, 4096, L"%ls/%ls", w, d.cFileName);
      if ((d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
          !(d.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
        char utf8[8192];
        WideCharToMultiByte(CP_UTF8, 0, child, -1, utf8, sizeof(utf8), NULL,
                            NULL);
        cleanup_tree(utf8);
      } else if (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        RemoveDirectoryW(child);
      else
        DeleteFileW(child);
    } while (FindNextFileW(h, &d));
    FindClose(h);
  }
  RemoveDirectoryW(w);
#else
  DIR *dir = opendir(path);
  if (!dir)
    return;
  struct dirent *d;
  while ((d = readdir(dir))) {
    if (!strcmp(d->d_name, ".") || !strcmp(d->d_name, ".."))
      continue;
    char child[4096];
    snprintf(child, sizeof(child), "%s/%s", path, d->d_name);
    struct stat st;
    if (!lstat(child, &st) && S_ISDIR(st.st_mode))
      cleanup_tree(child);
    else
      unlink(child);
  }
  closedir(dir);
  rmdir(path);
#endif
}
static void setup(void) {
  fixture_ready = false;
  fixture[0] = 0;
#ifdef _WIN32
  wchar_t temp[512], unique[1024];
  DWORD temp_length = GetCurrentDirectoryW(512, temp);
  check(temp_length > 0 && temp_length < 512, "temp directory");
  UINT temp_created = GetTempFileNameW(temp, L"fsx", 0, unique);
  check(temp_created != 0, "exclusive fixture name");
  DeleteFileW(unique);
  check(CreateDirectoryW(unique, NULL) != 0, "fixture directory");
  check(WideCharToMultiByte(CP_UTF8, 0, unique, -1, fixture, sizeof(fixture),
                            NULL, NULL) > 0,
        "fixture UTF-8");
#else
  strcpy(fixture, "./fsx-test-XXXXXX");
  check_not_null(mkdtemp(fixture));
#endif
  fixture_ready = true;
  snprintf(root, sizeof(root), "%s/tree", fixture);
  mkdir_utf8(root);
  snprintf(database, sizeof(database), "%s/index.db", fixture);
  engine = NULL;
  oracle_engine = NULL;
  held_query = NULL;
  held_result = NULL;
  retained_result = NULL;
  watch = second_watch = NULL;
  scan_blocker = NULL;
  held_index = NULL;
  memset(&index_gate, 0, sizeof(index_gate));
}
static void teardown(void) {
  finish_index();
  if (index_gate.changed)
    cmeta_cond_destroy(&index_gate.changed);
  if (index_gate.mutex)
    cmeta_mutex_destroy(&index_gate.mutex);
  memset(&index_gate, 0, sizeof(index_gate));
  if (scan_blocker) {
    sqlite3_exec(scan_blocker, "ROLLBACK", NULL, NULL, NULL);
    sqlite3_close(scan_blocker);
    scan_blocker = NULL;
  }
  if (second_watch)
    check_equal(fsx_watch_close(&second_watch), FS_OK);
  if (watch)
    check_equal(fsx_watch_close(&watch), FS_OK);
  fsx_result_release(retained_result);
  retained_result = NULL;
  fsx_result_release(held_result);
  held_result = NULL;
  fsx_query_release(held_query);
  held_query = NULL;
  if (oracle_engine)
    check_equal(fsx_engine_close(&oracle_engine, FSX_DEADLINE_INFINITE), FS_OK);
  if (engine)
    check_equal(fsx_engine_close(&engine, FSX_DEADLINE_INFINITE), FS_OK);
  if (fixture_ready)
    cleanup_tree(fixture);
  fixture_ready = false;
}
static void open_engine(void) {
  FsEngineConfig config = fsx_engine_config(database);
  config.fts_min_entries =
      0; /* Existing posting/identity tests explicitly exercise FTS. */
  FsOpenError diagnostic = {0};
  config.open_error = &diagnostic;
  FsStatus status = fsx_engine_open(&config, &engine);
  check(status == FS_OK, "open: %s (%d): %s", fsx_status_name(status),
        diagnostic.native_code, diagnostic.diagnostic);
}
static FsScanInfo scan(void) {
  FsScanInfo info;
  check_equal(fsx_engine_scan(engine, root, &info), FS_OK);
  return info;
}
static FsStatus execute_on(FsEngine *target, const char *text,
                           FsQueryOptions options) {
  fsx_result_release(held_result);
  held_result = NULL;
  fsx_query_release(held_query);
  held_query = NULL;
  FsQueryRequest r = {sizeof(r), FSX_API_VERSION, text, strlen(text), options};
  FsStatus status = fsx_query_submit(target, &r, &held_query);
  if (status == FS_OK)
    status = fsx_query_wait(held_query, FSX_DEADLINE_INFINITE);
  if (status == FS_OK)
    status = fsx_query_result_acquire(held_query, &held_result);
  return status;
}
static FsStatus execute(const char *text, FsQueryOptions options) {
  return execute_on(engine, text, options);
}
static uint64_t count(const char *text) {
  check_equal(execute(text, fsx_query_options()), FS_OK);
  FsResultInfo info;
  check_equal(fsx_result_info(held_result, &info), FS_OK);
  return info.total_count;
}
static FsEntryView entry(uint32_t index, FsHit *saved) {
  FsHit hit;
  size_t written = 0;
  FsEntryView view;
  check_equal(fsx_result_page(held_result, index, 1, &hit, 1, &written), FS_OK);
  check_equal(written, (size_t)1);
  check_equal(fsx_result_entry(held_result, hit, &view), FS_OK);
  if (saved)
    *saved = hit;
  return view;
}
static void edit_database(const char *sql) {
  sqlite3 *db = NULL;
  int opened = sqlite3_open_v2(database, &db, SQLITE_OPEN_READWRITE, NULL);
  int rc =
      opened == SQLITE_OK ? sqlite3_exec(db, sql, NULL, NULL, NULL) : opened;
  sqlite3_close(db);
  check_equal(rc, SQLITE_OK);
}
static void open_oracle(void) {
  FsEngineConfig config = fsx_engine_config(database);
  config.flags = FS_ENGINE_SCAN_ONLY;
  check_equal(fsx_engine_open(&config, &oracle_engine), FS_OK);
}
static FsResultInfo equivalent(const char *text, FsQueryOptions options) {
  check_equal(execute(text, options), FS_OK);
  FsResultInfo accelerated, scanned;
  check_equal(fsx_result_info(held_result, &accelerated), FS_OK);
  check_true(accelerated.window_count <= 100);
  uint64_t keys[100];
  for (uint32_t i = 0; i < accelerated.window_count; i++)
    keys[i] = entry(i, NULL).entry_key;
  check_equal(execute_on(oracle_engine, text, options), FS_OK);
  check_equal(fsx_result_info(held_result, &scanned), FS_OK);
  check_equal(scanned.candidate_strategy, FS_CANDIDATE_SCAN_PROFILE);
  check_equal(scanned.total_count, accelerated.total_count);
  check_equal(scanned.snapshot_epoch, accelerated.snapshot_epoch);
  check_equal(scanned.window_count, accelerated.window_count);
  check_equal(scanned.window_offset, accelerated.window_offset);
  check_equal(scanned.total_known, accelerated.total_known);
  check_equal(scanned.coverage_complete, accelerated.coverage_complete);
  check_equal(scanned.text_coverage_complete,
              accelerated.text_coverage_complete);
  for (uint32_t i = 0; i < scanned.window_count; i++)
    check_equal(entry(i, NULL).entry_key, keys[i]);
  return accelerated;
}
#ifdef _WIN32
static void open_watch(FsWatch **out, const char *path, uint32_t bytes) {
  FsWatchConfig config = fsx_watch_config();
  config.buffer_bytes = bytes;
  check_equal(fsx_watch_open(engine, path, &config, out), FS_OK);
}
static FsWatchInfo settle_watch(FsWatch *w) {
  FsWatchInfo info;
  uint64_t end = fsx_engine_now_ns(engine) + 5000000000ull;
  for (unsigned pass = 0; pass < 64; pass++) {
    FsStatus status = fsx_watch_wait(w, 0, &info);
    if (status == FS_WAIT)
      return info;
    check_equal(status, FS_OK);
    check_true(fsx_engine_now_ns(engine) < end);
    FsScanInfo scanned;
    check_equal(fsx_watch_scan(w, &scanned), FS_OK);
  }
  check(false, "watch did not settle within 64 reconciliation passes");
  return info;
}
static FsWatchInfo wait_change(FsWatch *w) {
  FsWatchInfo info;
  check_equal(
      fsx_watch_wait(w, fsx_engine_now_ns(engine) + 5000000000ull, &info),
      FS_OK);
  check_true(info.needs_rescan);
  return info;
}
typedef struct WatchWaiter {
  FsWatch *watch;
  FsStatus status;
  DWORD native_error;
} WatchWaiter;
static void watch_waiter(void *arg) {
  WatchWaiter *waiter = arg;
  FsWatchInfo info;
  waiter->status = fsx_watch_wait(waiter->watch, FSX_DEADLINE_INFINITE, &info);
}
static void watch_scanner(void *arg) {
  WatchWaiter *waiter = arg;
  FsScanInfo scanned;
  waiter->status = fsx_watch_scan(waiter->watch, &scanned);
  waiter->native_error = GetLastError();
}
static void watch_opener(void *arg) {
  WatchWaiter *waiter = arg;
  FsWatchConfig config = fsx_watch_config();
  waiter->status = fsx_watch_open(engine, root, &config, &watch);
}
#endif
typedef struct ReplacedDirectory {
  char victim[2048], parked[2048], outside[2048];
  bool replace, replaced;
  unsigned secrets, safe, links;
  unsigned long native_error;
} ReplacedDirectory;
/* Real mount-point reparse data needs no developer-mode symlink privilege.
 * Every source and destination is checked as an owned fixture path beforehand.
 */
static bool junction(const char *path, const char *target) {
#ifdef _WIN32
  wchar_t name[2048], print[2048], substitute[2054];
  if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, name,
                           2048) ||
      !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, target, -1, print,
                           2048))
    return false;
  for (wchar_t *c = print; *c; c++)
    if (*c == L'/')
      *c = L'\\';
  if (wcslen(print) < 3 || print[1] != L':')
    return false;
  swprintf(substitute, 2054, L"\\??\\%ls", print);
  struct Junction {
    DWORD tag;
    USHORT length, reserved, substitute_offset, substitute_length, print_offset,
        print_length;
    wchar_t names[4096];
  } data = {0};
  size_t substitute_bytes = wcslen(substitute) * sizeof(wchar_t);
  size_t print_bytes = wcslen(print) * sizeof(wchar_t);
  size_t names_bytes = substitute_bytes + print_bytes + 2 * sizeof(wchar_t);
  if (names_bytes > sizeof(data.names))
    return false;
  data.tag = IO_REPARSE_TAG_MOUNT_POINT;
  data.length = (USHORT)(8 + names_bytes);
  data.substitute_length = (USHORT)substitute_bytes;
  data.print_offset = (USHORT)(substitute_bytes + sizeof(wchar_t));
  data.print_length = (USHORT)print_bytes;
  memcpy(data.names, substitute, substitute_bytes);
  memcpy((unsigned char *)data.names + data.print_offset, print, print_bytes);
  HANDLE directory = CreateFileW(
      name, GENERIC_WRITE,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
      OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
      NULL);
  if (directory == INVALID_HANDLE_VALUE)
    return false;
  DWORD returned;
  BOOL ok = DeviceIoControl(directory, FSCTL_SET_REPARSE_POINT, &data,
                            data.length + 8, NULL, 0, &returned, NULL);
  DWORD error = GetLastError();
  CloseHandle(directory);
  SetLastError(error);
  return ok != 0;
#else
  char *resolved = realpath(target, NULL);
  if (!resolved)
    return false;
  /* Replace the empty directory with a link inside the same owned fixture. */
  bool ok = !rmdir(path) && !symlink(resolved, path);
  free(resolved);
  return ok;
#endif
}
static FsStatus replace_observed_directory(void *context,
                                           const FsObserved *item) {
  ReplacedDirectory *state = context;
  if (!strcmp(item->name, "secret.txt"))
    state->secrets++;
  if (!strcmp(item->name, "safe.txt"))
    state->safe++;
  if (item->flags & FS_ENTRY_LINK)
    state->links++;
  if (!state->replace || state->replaced || strcmp(item->name, "victim") ||
      !(item->flags & FS_ENTRY_DIRECTORY) || item->flags & FS_ENTRY_LINK)
    return FS_OK;
  state->replaced = true;
#ifdef _WIN32
  wchar_t from[2048], to[2048];
  if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, state->victim, -1,
                           from, 2048) ||
      !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, state->parked, -1, to,
                           2048))
    return FS_IO;
  if (!MoveFileW(from, to) || !CreateDirectoryW(from, NULL) ||
      !junction(state->victim, state->outside)) {
    state->native_error = GetLastError();
    return FS_IO;
  }
#else
  if (rename(state->victim, state->parked) || mkdir(state->victim, 0700) ||
      !junction(state->victim, state->outside)) {
    state->native_error = (unsigned long)errno;
    return FS_IO;
  }
#endif
  return FS_OK;
}
static ReplacedDirectory replacement_fixture(void) {
  ReplacedDirectory state = {0};
  snprintf(state.victim, sizeof(state.victim), "%s/victim", root);
  snprintf(state.parked, sizeof(state.parked), "%s/parked", fixture);
  snprintf(state.outside, sizeof(state.outside), "%s/outside", fixture);
  size_t prefix = strlen(fixture);
  check_true(fixture_ready && prefix > 0);
  const char *paths[] = {state.victim, state.parked, state.outside};
  for (unsigned i = 0; i < 3; i++)
    check(!strncmp(paths[i], fixture, prefix) && paths[i][prefix] == '/',
          "directory mutation stays inside the owned fixture");
  subdir("victim");
  mkdir_utf8(state.outside);
  char secret[2048];
  snprintf(secret, sizeof(secret), "%s/secret.txt", state.outside);
  put_path(secret, "outside");
  return state;
}
typedef struct Sender {
  FsEngine *engine;
  atomic_uint failures;
} Sender;
static void sender_run(void *arg) {
  Sender *sender = arg;
  for (unsigned i = 0; i < 50; i++) {
    FsQueryRequest request = {sizeof(request), FSX_API_VERSION, "steady", 6,
                              fsx_query_options()};
    FsQuery *q = NULL;
    FsResult *r = NULL;
    FsStatus status = fsx_query_submit(sender->engine, &request, &q);
    if (status == FS_OK)
      status = fsx_query_wait(q, FSX_DEADLINE_INFINITE);
    if (status == FS_OK)
      status = fsx_query_result_acquire(q, &r);
    if (status == FS_OK) {
      FsResultInfo info;
      status = fsx_result_info(r, &info);
      if (status == FS_OK && (info.total_count != 1 || !info.snapshot_epoch))
        status = FS_CORRUPT;
      FsHit hit;
      size_t written = 0;
      FsEntryView view;
      if (status == FS_OK)
        status = fsx_result_page(r, 0, 1, &hit, 1, &written);
      if (status == FS_OK)
        status = fsx_result_entry(r, hit, &view);
      if (status == FS_OK && (!written || strcmp(view.name, "steady.txt")))
        status = FS_CORRUPT;
    }
    if (status != FS_OK)
      atomic_fetch_add(&sender->failures, 1);
    fsx_result_release(r);
    fsx_query_release(q);
  }
}
suite("FileSearch engine") {
  before_each() { setup(); }
  after_each() { teardown(); }
  it("indexes asynchronously while queries retain the committed revision") {
    put("old.txt", "x");
    open_engine();
    FsScanInfo before = scan();
    for (unsigned i = 0; i < 160; i++) {
      char name[64];
      snprintf(name, sizeof(name), "new-%u.txt", i);
      put(name, "new");
    }
    gate_start(false);
    engine->sql_observer = pause_index;
    check_equal(fsx_engine_scan_async(engine, root, &held_index), FS_OK);
    gate_wait();
    check_equal(fsx_index_wait(held_index, 0), FS_WAIT);
    check_equal(fsx_index_wait(held_index, cmeta_hrtime()), FS_TIMEOUT);
    FsScanInfo rejected;
    check_equal(fsx_engine_scan(engine, root, &rejected), FS_BUSY);
    FsIndexJob *duplicate = NULL;
    check_equal(fsx_engine_scan_async(engine, root, &duplicate), FS_BUSY);
    check_null(duplicate);
    check_equal(count("old"), (uint64_t)1);
    retained_result = held_result;
    held_result = NULL;
    check_equal(count("new"), (uint64_t)0);
    gate_release();
    check_equal(
        fsx_index_wait(held_index, cmeta_hrtime() + UINT64_C(5000000000)),
        FS_OK);
    FsIndexInfo done;
    check_equal(fsx_index_poll(held_index, &done), FS_OK);
    check_equal(done.state, (uint32_t)FS_INDEX_READY);
    check_equal(done.scan.snapshot_epoch, before.snapshot_epoch + 1);
    check_equal(done.scan.entries, (uint64_t)161);
    check_equal(count("new"), (uint64_t)160);
    FsResultInfo old;
    fsx_result_info(retained_result, &old);
    check_equal(old.snapshot_epoch, before.snapshot_epoch);
    engine->sql_observer = NULL;
  }
  it("cancels an asynchronous SQL writer without advancing its revision") {
    put("old.txt", "x");
    open_engine();
    FsScanInfo before = scan();
    for (unsigned i = 0; i < 160; i++) {
      char name[64];
      snprintf(name, sizeof(name), "later-%u.txt", i);
      put(name, "new");
    }
    gate_start(false);
    engine->sql_observer = pause_index;
    check_equal(fsx_engine_scan_async(engine, root, &held_index), FS_OK);
    gate_wait();
    fsx_index_cancel(held_index);
    check_equal(
        fsx_index_wait(held_index, cmeta_hrtime() + UINT64_C(5000000000)),
        FS_CANCELLED);
    FsProgress progress;
    fsx_engine_progress(engine, &progress);
    check_equal(progress.snapshot_epoch, before.snapshot_epoch);
    check_true(sqlite3_get_autocommit(engine->db) != 0);
    check_equal(count("later"), (uint64_t)0);
    engine->sql_observer = NULL;
    check_equal(scan().entries, (uint64_t)161);
  }
  it("cancels a queued index job and releases writer admission on STOPPING") {
    open_engine();
    gate_start(false);
    check_equal(cmeta_threadpool_try_submit(engine->index_pool,
                                            occupy_index_worker, NULL),
                0);
    gate_wait();
    check_equal(fsx_engine_scan_async(engine, root, &held_index), FS_OK);
    check_equal(fsx_engine_close(&engine, 0), FS_BUSY);
    check_equal(
        fsx_index_wait(held_index, cmeta_hrtime() + UINT64_C(5000000000)),
        FS_CANCELLED);
    FsProgress progress;
    fsx_engine_progress(engine, &progress);
    check_false(progress.scanning);
    check_equal(progress.snapshot_epoch, (uint64_t)0);
    fsx_index_release(held_index);
    held_index = NULL;
    check_equal(
        fsx_engine_close(&engine, cmeta_hrtime() + UINT64_C(5000000000)),
        FS_OK);
  }
  it("joins a released background index job during engine shutdown") {
    put("old.txt", "x");
    open_engine();
    scan();
    for (unsigned i = 0; i < 160; i++) {
      char name[64];
      snprintf(name, sizeof(name), "pending-%u.txt", i);
      put(name, "new");
    }
    gate_start(false);
    engine->sql_observer = pause_index;
    check_equal(fsx_engine_scan_async(engine, root, &held_index), FS_OK);
    gate_wait();
    fsx_index_release(held_index);
    held_index = NULL;
    check_equal(
        fsx_engine_close(&engine, cmeta_hrtime() + UINT64_C(5000000000)),
        FS_OK);
    check_null(engine);
  }
  it("indexes nested names and exposes committed metadata") {
    subdir("sub");
    put("alpha.txt", "12345");
    put("sub/annual report.md", "seven");
    open_engine();
    FsScanInfo info = scan();
    check_true(info.coverage_complete);
    check_equal(info.entries, (uint64_t)3);
    check_equal(count("file:"), (uint64_t)2);
    check_equal(count("folder:"), (uint64_t)1);
    check_equal(count("\"annual report\""), (uint64_t)1);
    check_equal(count("path:sub/annual"), (uint64_t)1);
    FsEntryView view = entry(0, NULL);
    check_equal(view.name, "annual report.md");
    check_equal(view.size, (uint64_t)5);
  }
  it("uses Unicode full folding without changing glob scalar boundaries") {
    put("Stra\xc3\x9f"
        "e.txt",
        "x");
    put("\xc3\x9f.utf", "y");
    open_engine();
    scan();
    check_equal(count("STRASSE"), (uint64_t)1);
    check_equal(count("case:STRASSE"), (uint64_t)0);
    check_equal(count("?.utf"), (uint64_t)1);
    check_equal(count("??.utf"), (uint64_t)0);
    check_equal(count("SS.utf"), (uint64_t)1);
  }
  it("implements NOT AND OR precedence and scoped modifiers") {
    put("alpha.txt", "x");
    put("beta gamma.md", "y");
    put("gamma.txt", "z");
    open_engine();
    scan();
    check_equal(count("alpha | beta gamma"), (uint64_t)2);
    check_equal(count("<alpha | beta> !gamma"), (uint64_t)1);
    check_equal(count("case:<ALPHA | beta> | gamma"), (uint64_t)2);
    check_equal(count("case:ALPHA gamma"), (uint64_t)0);
    check_equal(count("case:path:<alpha | beta>"), (uint64_t)2);
  }
  it("filters extension sets and checked logical-size intervals") {
    put("a.txt", "12345");
    put("b.md", "1");
    put("c.bin", "");
    subdir("folder");
    open_engine();
    scan();
    check_equal(count("ext:txt;md size:>=2B"), (uint64_t)1);
    check_equal(count("size:<2"), (uint64_t)2);
    check_equal(count("folder: size:>=0"), (uint64_t)0);
    check_equal(execute("size:18446744073709551616", fsx_query_options()),
                FS_INVALID);
    check_equal(execute("dm:2026-02-30", fsx_query_options()), FS_INVALID);
  }
  it("matches PCRE2 Unicode and explicit byte regex without changing backend") {
    put("report42.TXT", "data");
    put("other.txt", "none");
    open_engine();
    scan();
    check_equal(count("regex:\"^report[0-9]+\\.txt$\""), (uint64_t)1);
    check_equal(count("case:regex:\"^report[0-9]+\\.txt$\""), (uint64_t)0);
    check_equal(count("regexb:\"report[0-9]+\""), (uint64_t)1);
    check_equal(execute("regexb:\"(*UTF)a\"", fsx_query_options()), FS_INVALID);
    check_equal(execute("regex:\"[\"", fsx_query_options()), FS_INVALID);
    FsQueryInfo info;
    fsx_query_poll(held_query, &info);
    check_true(info.diagnostic[0] != 0);
    check_equal(info.error_offset, (size_t)8);
  }
  it("runs the explicit PCRE2 JIT profile and reports actual execution") {
    put("match123.txt", "x");
    FsEngineConfig config = fsx_engine_config(database);
    config.flags |= FS_ENGINE_REGEX_JIT;
    check_equal(fsx_engine_open(&config, &engine), FS_OK);
    scan();
    check_equal(count("regex:match[0-9]+"), (uint64_t)1);
    FsResultInfo info;
    fsx_result_info(held_result, &info);
    check_true(info.regex_jit);
  }
  it("bounds pathological regex execution rather than treating limit as no "
     "match") {
    put("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa!.txt", "x");
    open_engine();
    scan();
    check_equal(execute("regex:\"^(a+)+$\"", fsx_query_options()), FS_LIMIT);
  }
  it("keeps stable ties when sorting in descending order and paginating") {
    subdir("a");
    subdir("b");
    put("a/same.txt", "123");
    put("b/same.txt", "456");
    open_engine();
    scan();
    check_equal(count("ext:txt"), (uint64_t)2);
    uint64_t first = entry(0, NULL).entry_key,
             second = entry(1, NULL).entry_key;
    FsQueryOptions options = fsx_query_options();
    options.flags = FS_QUERY_DESCENDING;
    check_equal(execute("ext:txt", options), FS_OK);
    check_equal(entry(0, NULL).entry_key, first);
    check_equal(entry(1, NULL).entry_key, second);
    options.offset = 1;
    options.limit = 1;
    check_equal(execute("ext:txt", options), FS_OK);
    FsResultInfo info;
    fsx_result_info(held_result, &info);
    check_equal(info.total_count, (uint64_t)2);
    check_equal(info.window_count, 1u);
    check_equal(entry(0, NULL).entry_key, second);
  }
  it("places unknown directory sizes last for both sort directions") {
    subdir("directory");
    put("file.txt", "x");
    open_engine();
    scan();
    for (int descending = 0; descending < 2; descending++) {
      FsQueryOptions o = fsx_query_options();
      o.sort_key = FS_SORT_SIZE;
      if (descending)
        o.flags = FS_QUERY_DESCENDING;
      check_equal(execute("", o), FS_OK);
      check_equal(entry(0, NULL).name, "file.txt");
      check_equal(entry(1, NULL).name, "directory");
    }
  }
  it("retains old snapshots and rejects hits from another revision") {
    put("old.txt", "one");
    open_engine();
    scan();
    check_equal(count(""), (uint64_t)1);
    FsResult *old = held_result;
    retained_result = old;
    held_result = NULL;
    FsHit hit;
    size_t written = 0;
    check_equal(fsx_result_page(old, 0, 1, &hit, 1, &written), FS_OK);
    FsEntryView borrowed;
    check_equal(fsx_result_entry(old, hit, &borrowed), FS_OK);
    put("new.txt", "two");
    scan();
    check_equal(count(""), (uint64_t)2);
    FsEntryView ignored;
    check_equal(fsx_result_entry(held_result, hit, &ignored), FS_STALE);
    check_equal(borrowed.name, "old.txt");
    check_equal(fsx_result_entry(old, hit, &ignored), FS_OK);
    fsx_result_release(old);
    retained_result = NULL;
  }
  it("persists revisions and stable database keys across reopen and rescan") {
    put("kept.txt", "data");
    open_engine();
    scan();
    check_equal(count("kept"), (uint64_t)1);
    uint64_t key = entry(0, NULL).entry_key;
    fsx_result_release(held_result);
    held_result = NULL;
    fsx_query_release(held_query);
    held_query = NULL;
    check_equal(fsx_engine_close(&engine, FSX_DEADLINE_INFINITE), FS_OK);
    open_engine();
    check_equal(count("kept"), (uint64_t)1);
    check_equal(entry(0, NULL).entry_key, key);
    scan();
    check_equal(count("kept"), (uint64_t)1);
    check_equal(entry(0, NULL).entry_key, key);
    char path[2048];
    snprintf(path, sizeof(path), "%s/kept.txt", root);
    remove_utf8(path);
    scan();
    check_equal(count(""), (uint64_t)0);
  }
  it("keeps hardlink aliases as separate entries sharing file identity") {
    put("original.txt", "same data");
    char a[2048], b[2048];
    snprintf(a, sizeof(a), "%s/original.txt", root);
    snprintf(b, sizeof(b), "%s/alias.txt", root);
#ifdef _WIN32
    wchar_t wa[2048], wb[2048];
    wide(a, wa, 2048);
    wide(b, wb, 2048);
    check(CreateHardLinkW(wb, wa, NULL) != 0, "hardlink fixture");
#else
    check_equal(link(a, b), 0);
#endif
    open_engine();
    scan();
    check_equal(count("ext:txt"), (uint64_t)2);
    FsEntryView first = entry(0, NULL), second = entry(1, NULL);
    check_equal(first.key.file_id_lo, second.key.file_id_lo);
    check_equal(first.key.file_id_hi, second.key.file_id_hi);
    check_not_equal(first.entry_key, second.entry_key);
    remove_utf8(b);
    scan();
    check_equal(count("ext:txt"), (uint64_t)1);
  }
  it("rolls back a capacity failure without advancing the published revision") {
    put("kept.txt", "x");
    FsEngineConfig c = fsx_engine_config(database);
    c.max_entries = 2;
    check_equal(fsx_engine_open(&c, &engine), FS_OK);
    scan();
    put("new1.txt", "x");
    put("new2.txt", "x");
    FsScanInfo failed;
    check_equal(fsx_engine_scan(engine, root, &failed), FS_LIMIT);
    check_equal(failed.snapshot_epoch, (uint64_t)0);
    check_equal(count(""), (uint64_t)1);
    FsProgress progress;
    fsx_engine_progress(engine, &progress);
    check_equal(progress.snapshot_epoch, (uint64_t)1);
    check_true(progress.needs_rescan);
    FsResultInfo info;
    fsx_result_info(held_result, &info);
    check_true(info.needs_rescan);
  }
  it("bounds recursive file and folder modifiers before consuming the C "
     "stack") {
    put("seed.txt", "x");
    open_engine();
    scan();
    char text[65002];
    for (unsigned i = 0; i < 13000; i++)
      memcpy(text + 5 * i, "file:", 5);
    memcpy(text + 65000, "x", 2);
    check_equal(execute(text, fsx_query_options()), FS_LIMIT);
    check_null(held_result);
    for (unsigned i = 0; i < 31; i++)
      memcpy(text + 5 * i, "file:", 5);
    strcpy(text + 155, "seed");
    check_equal(execute(text, fsx_query_options()), FS_OK);
    for (unsigned i = 0; i < 32; i++)
      memcpy(text + 5 * i, "file:", 5);
    strcpy(text + 160, "seed");
    check_equal(execute(text, fsx_query_options()), FS_LIMIT);
    for (unsigned i = 0; i < 4000; i++)
      memcpy(text + 7 * i, "folder:", 7);
    memcpy(text + 28000, "x", 2);
    check_equal(execute(text, fsx_query_options()), FS_LIMIT);
    check_equal(count("seed"), (uint64_t)1);
  }
  it("keeps a root failure pending through unrelated successful scans") {
    put("kept.txt", "x");
    FsEngineConfig config = fsx_engine_config(database);
    config.max_entries = 2;
    check_equal(fsx_engine_open(&config, &engine), FS_OK);
    scan();
    char other[2048];
    snprintf(other, sizeof(other), "%s/other", fixture);
    mkdir_utf8(other);
    put("new1.txt", "x");
    put("new2.txt", "x");
    FsScanInfo result;
    check_equal(fsx_engine_scan(engine, root, &result), FS_LIMIT);
    check_equal(fsx_engine_scan(engine, other, &result), FS_OK);
    FsProgress progress;
    fsx_engine_progress(engine, &progress);
    check_true(progress.needs_rescan);
    check_equal(count("kept"), (uint64_t)1);
    FsResultInfo info;
    fsx_result_info(held_result, &info);
    check_true(info.needs_rescan);
    char path[2048];
    snprintf(path, sizeof(path), "%s/new1.txt", root);
    remove_utf8(path);
    snprintf(path, sizeof(path), "%s/new2.txt", root);
    remove_utf8(path);
    scan();
    fsx_engine_progress(engine, &progress);
    check_false(progress.needs_rescan);
  }
  it("conservatively validates every tracked root after an unidentified scan "
     "failure") {
    put("kept.txt", "x");
    open_engine();
    scan();
    char other[2048], missing[2048];
    snprintf(other, sizeof(other), "%s/other", fixture);
    mkdir_utf8(other);
    snprintf(missing, sizeof(missing), "%s/missing", fixture);
    FsScanInfo result;
    check_equal(fsx_engine_scan(engine, other, &result), FS_OK);
    check_not_equal(fsx_engine_scan(engine, missing, &result), FS_OK);
    check_equal(fsx_engine_scan(engine, other, &result), FS_OK);
    FsProgress progress;
    fsx_engine_progress(engine, &progress);
    check_true(progress.needs_rescan);
    scan();
    fsx_engine_progress(engine, &progress);
    check_false(progress.needs_rescan);
  }
  it("binds directory descent to the opened object through junction "
     "replacement") {
    ReplacedDirectory state = replacement_fixture();
    put("victim/safe.txt", "inside");
    open_engine();
    state.replace = true;
    FsWalk walk = {engine, replace_observed_directory, &state, 0, 0};
    FsStatus status = fsx_walk(&walk, root);
    check(status == FS_OK, "walk status=%s native=%lu", fsx_status_name(status),
          state.native_error);
    check_true(state.replaced);
    check_equal(state.secrets, 0u);
    check_equal(state.safe, 1u);
  }
  it("indexes a stable junction itself without enumerating its target") {
    ReplacedDirectory state = replacement_fixture();
    check_true(junction(state.victim, state.outside));
    open_engine();
    FsScanInfo info = scan();
    check_true(info.coverage_complete);
    check_equal(count("secret"), (uint64_t)0);
    check_equal(count("victim"), (uint64_t)1);
    uint32_t flags = entry(0, NULL).flags;
    check_true((flags & FS_ENTRY_LINK) != 0);
#ifdef _WIN32
    check_true((flags & FS_ENTRY_DIRECTORY) != 0);
#else
    /* lstat describes the symlink itself; its target is never traversed. */
    check_false((flags & FS_ENTRY_DIRECTORY) != 0);
#endif
  }
  it("enters retryable STOPPING while an external result is retained") {
    put("file.txt", "x");
    open_engine();
    scan();
    check_equal(count(""), (uint64_t)1);
    fsx_query_release(held_query);
    held_query = NULL;
    check_equal(fsx_engine_close(&engine, 0), FS_BUSY);
    check_not_null(engine);
    check_equal(entry(0, NULL).name, "file.txt");
    FsQueryRequest request = {sizeof(request), FSX_API_VERSION, "", 0,
                              fsx_query_options()};
    FsQuery *rejected = (FsQuery *)1;
    check_equal(fsx_query_submit(engine, &request, &rejected), FS_BUSY);
    check_null(rejected);
    fsx_result_release(held_result);
    held_result = NULL;
    check_equal(fsx_engine_close(&engine, FSX_DEADLINE_INFINITE), FS_OK);
    check_null(engine);
  }
  it("enforces query handle admission without revoking existing results") {
    put("file.txt", "x");
    FsEngineConfig config = fsx_engine_config(database);
    config.max_handles = 1;
    check_equal(fsx_engine_open(&config, &engine), FS_OK);
    scan();
    FsQueryRequest request = {sizeof(request), FSX_API_VERSION, "", 0,
                              fsx_query_options()};
    check_equal(fsx_query_submit(engine, &request, &held_query), FS_OK);
    check_equal(fsx_query_wait(held_query, FSX_DEADLINE_INFINITE), FS_OK);
    check_equal(fsx_query_result_acquire(held_query, &held_result), FS_BUSY);
    check_null(held_result);
  }
  it("reports deadlines, cancellation and invalid options explicitly") {
    put("file.txt", "x");
    open_engine();
    scan();
    FsQueryOptions options = fsx_query_options();
    options.deadline_ns = 1;
    check_equal(execute("", options), FS_TIMEOUT);
    options = fsx_query_options();
    options.offset = 100000;
    options.limit = 1;
    check_equal(execute("", options), FS_LIMIT);
    options = fsx_query_options();
    options.flags = 0x80000000u;
    check_equal(execute("", options), FS_INVALID);
    check_equal(execute("fuzzy:file", fsx_query_options()), FS_UNSUPPORTED);
    check_equal(execute("absent regex:\"[\"", fsx_query_options()), FS_INVALID);
    check_equal(execute("<a |>", fsx_query_options()), FS_INVALID);
    check_equal(execute("\"unterminated", fsx_query_options()), FS_INVALID);
    check_equal(execute("", fsx_query_options()), FS_OK);
    fsx_query_cancel(held_query);
    check_equal(fsx_query_wait(held_query, 0), FS_OK);
  }
  it("does not modify an unrelated SQLite database") {
    sqlite3 *db = NULL;
    check_equal(sqlite3_open(database, &db), SQLITE_OK);
    check_equal(
        sqlite3_exec(
            db,
            "CREATE TABLE user_data(value);INSERT INTO user_data VALUES(42)",
            NULL, NULL, NULL),
        SQLITE_OK);
    sqlite3_close(db);
    FsEngineConfig c = fsx_engine_config(database);
    check_equal(fsx_engine_open(&c, &engine), FS_CORRUPT);
    check_null(engine);
    check_equal(sqlite3_open(database, &db), SQLITE_OK);
    sqlite3_stmt *q = NULL;
    check_equal(
        sqlite3_prepare_v2(db, "SELECT value FROM user_data", -1, &q, NULL),
        SQLITE_OK);
    check_equal(sqlite3_step(q), SQLITE_ROW);
    check_equal(sqlite3_column_int(q, 0), 42);
    sqlite3_finalize(q);
    sqlite3_close(db);
  }
  it("indexes more than one 4096-entry batch without losing entries") {
    for (int i = 0; i < 4101; i++) {
      char name[64];
      snprintf(name, sizeof(name), "item-%04d.txt", i);
      put(name, "x");
    }
    open_engine();
    FsScanInfo info = scan();
    check_equal(info.entries, (uint64_t)4101);
    FsQueryOptions options = fsx_query_options();
    options.offset = 4100;
    options.limit = 1;
    check_equal(execute("ext:txt", options), FS_OK);
    check_equal(entry(0, NULL).name, "item-4100.txt");
    FsResultInfo result;
    fsx_result_info(held_result, &result);
    check_equal(result.total_count, (uint64_t)4101);
    FsEntryView original = entry(0, NULL);
    retained_result = held_result;
    held_result = NULL;
    FsProgress before, after;
    fsx_engine_progress(engine, &before);
    check_equal(before.snapshot_pages, 2u);
    check_equal(before.reused_snapshot_pages, 0u);
    scan();
    fsx_engine_progress(engine, &after);
    check_equal(after.reused_snapshot_pages, 2u);
    check_true(after.owned_bytes >= before.owned_bytes);
    check_true(after.owned_bytes - before.owned_bytes < 1024);
    check_equal(count("item-4100.txt"), (uint64_t)1);
    check_true(entry(0, NULL).name == original.name);
    put("item-4100.txt", "updated");
    scan();
    fsx_engine_progress(engine, &after);
    check_equal(after.reused_snapshot_pages, 1u);
    check_equal(count("item-4100.txt"), (uint64_t)1);
    FsEntryView changed = entry(0, NULL);
    check_equal(changed.size, (uint64_t)7);
    check_equal(changed.entry_key, original.entry_key);
    check_true(changed.name != original.name);
    check_equal(original.size, (uint64_t)1);
    check_equal(original.name, "item-4100.txt");
    fsx_engine_progress(engine, &before);
    fsx_result_release(retained_result);
    retained_result = NULL;
    fsx_engine_progress(engine, &after);
    check_true(after.owned_bytes < before.owned_bytes);

    /* Shrink to exactly one page, retaining an entry removed by that scan. */
    retained_result = held_result;
    held_result = NULL;
    for (unsigned i = 4096; i < 4101; i++) {
      char path[2048];
      snprintf(path, sizeof(path), "%s/item-%04u.txt", root, i);
      remove_utf8(path);
    }
    scan();
    fsx_engine_progress(engine, &after);
    check_equal(after.entries, (uint64_t)4096);
    check_equal(after.snapshot_pages, 1u);
    check_equal(changed.name, "item-4100.txt");
    put("appended.txt", "x");
    scan();
    fsx_engine_progress(engine, &after);
    check_equal(after.entries, (uint64_t)4097);
    check_equal(after.snapshot_pages, 2u);
    check_equal(after.reused_snapshot_pages, 1u);
    check_equal(count("appended"), (uint64_t)1);
    options.offset = 4096;
    check_equal(execute("", options), FS_OK);
    fsx_result_info(held_result, &result);
    check_equal(result.total_count, (uint64_t)4097);
  }
  it("rolls back a partial page copy when the managed budget is exhausted") {
    char names[512][96];
    for (unsigned i = 0; i < 512; i++) {
      memset(names[i], 'a', 60);
      snprintf(names[i] + 60, sizeof(names[i]) - 60, "-%03u.txt", i);
      put(names[i], "x");
    }
    FsEngineConfig config = fsx_engine_config(database);
    check_equal(fsx_engine_open(&config, &engine), FS_OK);
    scan();
    FsProgress loaded;
    fsx_engine_progress(engine, &loaded);
    check_equal(fsx_engine_close(&engine, FSX_DEADLINE_INFINITE), FS_OK);
    /* Enough for the query plan and a directory, but not another entire page.
     */
    config.owned_memory_budget = loaded.owned_bytes + 128 * 1024;
    check_equal(fsx_engine_open(&config, &engine), FS_OK);
    check_equal(count("ext:txt"), (uint64_t)512);
    FsEntryView old = entry(0, NULL);
    FsProgress before, after;
    fsx_engine_progress(engine, &before);
    scan();
    fsx_engine_progress(engine, &after);
    check_equal(after.reused_snapshot_pages, 1u);
    check_true(after.owned_bytes - before.owned_bytes < 1024);
    fsx_engine_progress(engine, &before);
    put(names[511], "changed");
    FsScanInfo failed;
    check_equal(fsx_engine_scan(engine, root, &failed), FS_NOMEM);
    fsx_engine_progress(engine, &after);
    check_equal(after.snapshot_epoch, before.snapshot_epoch);
    check_equal(after.owned_bytes, before.owned_bytes);
    check_true(after.needs_rescan);
    check_equal(old.size, (uint64_t)1);
    check_equal(old.name, names[0]);
    retained_result = held_result;
    held_result = NULL;
    open_oracle();
    FsProgress persisted;
    fsx_engine_progress(oracle_engine, &persisted);
    check_equal(persisted.snapshot_epoch, before.snapshot_epoch);
    check_equal(execute_on(oracle_engine, names[511], fsx_query_options()),
                FS_OK);
    check_equal(entry(0, NULL).size, (uint64_t)1);
    /* Smaller replacements can recover while the old result is still retained.
     */
    for (unsigned i = 1; i < 512; i++) {
      char path[2048];
      snprintf(path, sizeof(path), "%s/%s", root, names[i]);
      remove_utf8(path);
    }
    scan();
    fsx_engine_progress(engine, &after);
    check_equal(after.snapshot_epoch, before.snapshot_epoch + 1);
    check_equal(after.entries, (uint64_t)1);
    check_false(after.needs_rescan);
    check_equal(old.name, names[0]);
    check_equal(count("ext:txt"), (uint64_t)1);
  }
  it("represents an empty snapshot without retaining removed pages") {
    open_engine();
    FsProgress progress;
    fsx_engine_progress(engine, &progress);
    check_equal(progress.snapshot_pages, 0u);
    put("removed.txt", "x");
    scan();
    check_equal(count("removed"), (uint64_t)1);
    FsEntryView old = entry(0, NULL);
    char path[2048];
    snprintf(path, sizeof(path), "%s/removed.txt", root);
    remove_utf8(path);
    scan();
    fsx_engine_progress(engine, &progress);
    check_equal(progress.entries, (uint64_t)0);
    check_equal(progress.snapshot_pages, 0u);
    check_equal(progress.reused_snapshot_pages, 0u);
    check_equal(old.name, "removed.txt");
    check_equal(count(""), (uint64_t)0);
  }
#ifdef _WIN32
  it("watches create, metadata, rename and delete through retained snapshots") {
    put("seed.txt", "x");
    open_engine();
    open_watch(&watch, root, 65536);
    /* Changes after arming but before the first scan must be reconciled. */
    put("armed.txt", "x");
    FsWatchInfo info = settle_watch(watch);
    check_equal(info.state, FS_WATCH_IDLE);
    check_true(info.coverage_complete);
    check_true(info.source_id != 0);
    check_equal(
        fsx_watch_wait(watch, fsx_engine_now_ns(engine) + 10000000, &info),
        FS_TIMEOUT);
    check_equal(count("seed"), (uint64_t)1);
    FsEntryView old = entry(0, NULL);
    retained_result = held_result;
    held_result = NULL;
    subdir("nested");
    put("nested/created.txt", "two");
    wait_change(watch);
    FsProgress progress;
    fsx_engine_progress(engine, &progress);
    check_true(progress.needs_rescan);
    check_equal(progress.active_watches, 1u);
    check_equal(progress.dirty_watches, 1u);
    check_equal(count("created"), (uint64_t)0);
    FsResultInfo before;
    fsx_result_info(held_result, &before);
    check_true(before.needs_rescan);
    settle_watch(watch);
    check_equal(count("created"), (uint64_t)1);
    put("seed.txt", "updated");
    wait_change(watch);
    settle_watch(watch);
    check_equal(count("seed"), (uint64_t)1);
    check_equal(entry(0, NULL).size, (uint64_t)7);
    check_equal(old.name, "seed.txt");
    check_equal(old.size, (uint64_t)1);
    char from[2048], to[2048];
    snprintf(from, sizeof(from), "%s/nested/created.txt", root);
    snprintf(to, sizeof(to), "%s/nested/renamed.txt", root);
    wchar_t a[2048], b[2048];
    wide(from, a, 2048);
    wide(to, b, 2048);
    check(MoveFileW(a, b) != 0, "rename watched fixture file");
    wait_change(watch);
    settle_watch(watch);
    check_equal(count("created"), (uint64_t)0);
    check_equal(count("renamed"), (uint64_t)1);
    remove_utf8(to);
    wait_change(watch);
    settle_watch(watch);
    check_equal(count("renamed"), (uint64_t)0);
    check_equal(fsx_watch_close(&watch), FS_OK);
    fsx_engine_progress(engine, &progress);
    check_equal(progress.active_watches, 0u);
    check_true(progress.needs_rescan);
    put("offline.txt", "x");
    open_watch(&watch, root, 65536);
    info = wait_change(watch);
    check_equal(info.rescans, (uint64_t)0);
    settle_watch(watch);
    check_equal(count("offline"), (uint64_t)1);
    check_equal(old.name, "seed.txt");
  }
  it("turns notification overflow into a full reconciliation obligation") {
    open_engine();
    open_watch(&watch, root, 1024);
    settle_watch(watch);
    for (unsigned i = 0; i < 800; i++) {
      char name[64];
      snprintf(name, sizeof(name), "burst-%04u.txt", i);
      put(name, "x");
    }
    FsWatchInfo info = wait_change(watch);
    check_true(info.overflows > 0);
    /* A generic scan must not acknowledge a watch's unread generation. */
    scan();
    FsProgress progress;
    fsx_engine_progress(engine, &progress);
    check_true(progress.needs_rescan);
    info = settle_watch(watch);
    check_equal(info.state, FS_WATCH_IDLE);
    check_true(info.overflows > 0);
    check_equal(count("burst"), (uint64_t)800);
    fsx_engine_progress(engine, &progress);
    check_false(progress.needs_rescan);
  }
  it("keeps independent watch invalidations across successful source scans") {
    subdir("child");
    open_engine();
    open_watch(&watch, root, 65536);
    char child[2048];
    snprintf(child, sizeof(child), "%s/child", root);
    open_watch(&second_watch, child, 65536);
    settle_watch(watch);
    FsProgress progress;
    fsx_engine_progress(engine, &progress);
    check_equal(progress.dirty_watches, 1u);
    check_true(progress.needs_rescan);
    settle_watch(second_watch);
    put("child/new.txt", "x");
    wait_change(watch);
    wait_change(second_watch);
    settle_watch(watch);
    fsx_engine_progress(engine, &progress);
    check_equal(progress.dirty_watches, 1u);
    check_true(progress.needs_rescan);
    settle_watch(second_watch);
    fsx_engine_progress(engine, &progress);
    check_equal(progress.dirty_watches, 0u);
    check_false(progress.needs_rescan);
  }
  it("rejects invalid, duplicate, over-budget and self-indexing watches") {
    open_engine();
    FsWatchConfig config = fsx_watch_config();
    config.buffer_bytes = 0;
    check_equal(fsx_watch_open(engine, root, &config, &watch), FS_INVALID);
    check_null(watch);
    config = fsx_watch_config();
    config.api_version++;
    check_equal(fsx_watch_open(engine, root, &config, &watch), FS_INVALID);
    open_watch(&watch, root, 65536);
    FsProgress before, after;
    fsx_engine_progress(engine, &before);
    config = fsx_watch_config();
    check_equal(fsx_watch_open(engine, root, &config, &second_watch), FS_BUSY);
    check_null(second_watch);
    fsx_engine_progress(engine, &after);
    check_equal(after.owned_bytes, before.owned_bytes);
    check_equal(fsx_watch_open(engine, fixture, &config, &second_watch),
                FS_INVALID);
    check_null(second_watch);
    check_equal(fsx_watch_close(&watch), FS_OK);
    check_equal(fsx_engine_close(&engine, FSX_DEADLINE_INFINITE), FS_OK);
    FsEngineConfig limited = fsx_engine_config(database);
    limited.owned_memory_budget = 32768;
    check_equal(fsx_engine_open(&limited, &engine), FS_OK);
    check_equal(fsx_watch_open(engine, root, &config, &watch), FS_NOMEM);
    check_null(watch);
    fsx_engine_progress(engine, &after);
    check_equal(after.active_watches, 0u);
    check_equal(fsx_engine_close(&engine, FSX_DEADLINE_INFINITE), FS_OK);
    limited = fsx_engine_config(database);
    limited.max_handles = 1;
    check_equal(fsx_engine_open(&limited, &engine), FS_OK);
    open_watch(&watch, root, 65536);
    check_equal(execute("", fsx_query_options()), FS_BUSY);
    check_equal(fsx_watch_close(&watch), FS_OK);
    FsQueryRequest request = {sizeof(request), FSX_API_VERSION, "", 0,
                              fsx_query_options()};
    check_equal(fsx_query_submit(engine, &request, &held_query), FS_OK);
    check_equal(fsx_query_wait(held_query, FSX_DEADLINE_INFINITE), FS_OK);
  }
  it("retains invalidation from a change arriving during a watch scan") {
    open_engine();
    open_watch(&watch, root, 65536);
    settle_watch(watch);
    check_equal(sqlite3_open(database, &scan_blocker), SQLITE_OK);
    check_equal(sqlite3_exec(scan_blocker, "BEGIN IMMEDIATE", NULL, NULL, NULL),
                SQLITE_OK);
    char path[2048];
    snprintf(path, sizeof(path), "%s/midscan.txt", root);
    wchar_t target[2048];
    wide(path, target, 2048);
    WatchWaiter scanning = {watch, FS_WAIT};
    cmeta_thread_t thread = {0};
    check_equal(cmeta_thread_create(&thread, watch_scanner, &scanning), 0);
    bool observed = false;
    uint64_t end = fsx_engine_now_ns(engine) + 500000000;
    while (fsx_engine_now_ns(engine) < end) {
      FsProgress progress;
      fsx_engine_progress(engine, &progress);
      if (progress.scanning) {
        observed = true;
        break;
      }
      Sleep(1);
    }
    /* No fatal assertions until the worker has joined and its DB blocker is
     * gone. */
    HANDLE file =
        CreateFileW(target, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                    NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD written = 0;
    BOOL wrote =
        file != INVALID_HANDLE_VALUE && WriteFile(file, "x", 1, &written, NULL);
    if (file != INVALID_HANDLE_VALUE)
      CloseHandle(file);
    int committed = sqlite3_exec(scan_blocker, "COMMIT", NULL, NULL, NULL);
    sqlite3_close(scan_blocker);
    scan_blocker = NULL;
    int joined = cmeta_thread_join(&thread);
    cmeta_thread_destroy(&thread);
    check_equal(joined, 0);
    check_equal(committed, SQLITE_OK);
    check_true(observed);
    check_true(wrote != 0 && written == 1);
    FsWatchInfo info;
    FsStatus pumped = fsx_watch_wait(watch, 0, &info);
    check(scanning.status == FS_OK,
          "scan status=%s worker OS=%lu watch OS=%u state=%s rescans=%llu",
          fsx_status_name(scanning.status),
          (unsigned long)scanning.native_error, info.native_error,
          FsWatchState_to_string(info.state), (unsigned long long)info.rescans);
    check_equal(pumped, FS_OK);
    check_true(info.needs_rescan);
    check_equal(count("midscan"), (uint64_t)1);
    settle_watch(watch);
    FsProgress progress;
    fsx_engine_progress(engine, &progress);
    check_false(progress.needs_rescan);
  }
  it("keeps a failed watch scan dirty and recovers without losing old "
     "results") {
    put("seed.txt", "x");
    FsEngineConfig config = fsx_engine_config(database);
    config.max_entries = 1;
    check_equal(fsx_engine_open(&config, &engine), FS_OK);
    open_watch(&watch, root, 65536);
    settle_watch(watch);
    check_equal(count("seed"), (uint64_t)1);
    FsEntryView old = entry(0, NULL);
    retained_result = held_result;
    held_result = NULL;
    FsProgress before, after;
    fsx_engine_progress(engine, &before);
    put("overflow.txt", "new");
    wait_change(watch);
    FsScanInfo scanned;
    check_equal(fsx_watch_scan(watch, &scanned), FS_LIMIT);
    FsWatchInfo info = wait_change(watch);
    check_equal(info.state, FS_WATCH_DIRTY);
    check_equal(info.status, FS_LIMIT);
    fsx_engine_progress(engine, &after);
    check_equal(after.snapshot_epoch, before.snapshot_epoch);
    check_true(after.needs_rescan);
    char path[2048];
    snprintf(path, sizeof(path), "%s/overflow.txt", root);
    remove_utf8(path);
    settle_watch(watch);
    fsx_engine_progress(engine, &after);
    check_false(after.needs_rescan);
    check_equal(after.snapshot_epoch, before.snapshot_epoch + 1);
    check_equal(old.name, "seed.txt");
    check_equal(count("overflow"), (uint64_t)0);
  }
  it("rearms an interrupted watch after its serialized issuing thread exits") {
    open_engine();
    WatchWaiter opened = {NULL, FS_WAIT};
    cmeta_thread_t thread = {0};
    check_equal(cmeta_thread_create(&thread, watch_opener, &opened), 0);
    int joined = cmeta_thread_join(&thread);
    cmeta_thread_destroy(&thread);
    check_equal(joined, 0);
    check_equal(opened.status, FS_OK);
    FsWatchInfo info = wait_change(watch);
    check_equal(info.interruptions, (uint64_t)1);
    check_equal(info.native_error, (uint32_t)ERROR_OPERATION_ABORTED);
    check_equal(info.state, FS_WATCH_DIRTY);
    info = settle_watch(watch);
    check_equal(info.state, FS_WATCH_IDLE);
    check_equal(info.interruptions, (uint64_t)1);
  }
  it("fails a moved or replaced watch root without acknowledging its "
     "snapshot") {
    put("original.txt", "x");
    open_engine();
    open_watch(&watch, root, 65536);
    settle_watch(watch);
    check_equal(count("original"), (uint64_t)1);
    FsProgress before, after;
    fsx_engine_progress(engine, &before);
    char moved[2048];
    snprintf(moved, sizeof(moved), "%s/moved-tree", fixture);
    check_true(fixture_ready);
    check_true(!strncmp(root, fixture, strlen(fixture)) &&
               !strncmp(moved, fixture, strlen(fixture)));
    wchar_t a[2048], b[2048];
    wide(root, a, 2048);
    wide(moved, b, 2048);
    check(MoveFileW(a, b) != 0, "move owned fixture directory");
    mkdir_utf8(root);
    put("replacement.txt", "y");
    FsWatchInfo info;
    check_equal(fsx_watch_wait(watch, 0, &info), FS_STALE);
    check_equal(info.state, FS_WATCH_FAILED);
    check_true(info.needs_rescan);
    FsScanInfo scanned;
    check_equal(fsx_watch_scan(watch, &scanned), FS_STALE);
    fsx_engine_progress(engine, &after);
    check_equal(after.snapshot_epoch, before.snapshot_epoch);
    check_true(after.needs_rescan);
    check_equal(count("original"), (uint64_t)1);
    check_equal(count("replacement"), (uint64_t)0);
  }
  it("wakes a waiting watch during retryable engine STOPPING") {
    open_engine();
    open_watch(&watch, root, 65536);
    settle_watch(watch);
    WatchWaiter waiter = {watch, FS_WAIT};
    cmeta_thread_t thread = {0};
    check_equal(cmeta_thread_create(&thread, watch_waiter, &waiter), 0);
    FsStatus closed = fsx_engine_close(&engine, 0);
    int joined = cmeta_thread_join(&thread);
    cmeta_thread_destroy(&thread);
    check_equal(joined, 0);
    check_equal(closed, FS_BUSY);
    check_equal(waiter.status, FS_CANCELLED);
    FsWatchInfo info;
    check_equal(fsx_watch_wait(watch, 0, &info), FS_CANCELLED);
    check_equal(info.state, FS_WATCH_STOPPED);
    check_equal(fsx_watch_close(&watch), FS_OK);
    check_null(watch);
    check_equal(fsx_engine_close(&engine, FSX_DEADLINE_INFINITE), FS_OK);
  }
#endif
#ifdef _WIN32
  it("reports an unrepresentable native name as partial coverage") {
    put("safe.txt", "x");
    wchar_t path[2048];
    wide(root, path, 2048);
    size_t n = wcslen(path);
    path[n++] = L'/';
    path[n++] = 0xd800;
    path[n++] = L'.';
    path[n++] = L'x';
    path[n] = 0;
    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_NEW,
                              FILE_ATTRIBUTE_NORMAL, NULL);
    check(file != INVALID_HANDLE_VALUE, "create raw UTF-16 fixture");
    CloseHandle(file);
    open_engine();
    FsScanInfo info = scan();
    check_false(info.coverage_complete);
    check_equal(info.unsupported_names, (uint64_t)1);
    check_equal(count("safe"), (uint64_t)1);
    FsResultInfo ri;
    fsx_result_info(held_result, &ri);
    check_false(ri.coverage_complete);
    check_equal(execute("regex:safe", fsx_query_options()), FS_UNSUPPORTED);
    check_equal(execute("absent regex:safe", fsx_query_options()),
                FS_UNSUPPORTED);
    FsQueryOptions options = fsx_query_options();
    options.flags = FS_QUERY_ALLOW_PARTIAL_TEXT;
    check_equal(execute("regex:safe", options), FS_OK);
    fsx_result_info(held_result, &ri);
    check_false(ri.text_coverage_complete);
    DeleteFileW(path);
  }
#endif
  it("keeps committed FTS rows aligned after updates and removal") {
    put("old-name.txt", "x");
    open_engine();
    scan();
    char path[2048];
    snprintf(path, sizeof(path), "%s/old-name.txt", root);
    remove_utf8(path);
    put("new-name.txt", "y");
    scan();
    sqlite3 *db = NULL;
    check_equal(sqlite3_open_v2(database, &db, SQLITE_OPEN_READONLY, NULL),
                SQLITE_OK);
    sqlite3_stmt *q = NULL;
    check_equal(sqlite3_prepare_v2(
                    db, "SELECT count(*) FROM name_fts WHERE name_fts MATCH ?1",
                    -1, &q, NULL),
                SQLITE_OK);
    sqlite3_bind_text(q, 1, "\"old\"", -1, SQLITE_STATIC);
    check_equal(sqlite3_step(q), SQLITE_ROW);
    check_equal(sqlite3_column_int(q, 0), 0);
    sqlite3_reset(q);
    sqlite3_bind_text(q, 1, "\"new\"", -1, SQLITE_STATIC);
    check_equal(sqlite3_step(q), SQLITE_ROW);
    check_equal(sqlite3_column_int(q, 0), 1);
    sqlite3_finalize(q);
    sqlite3_close(db);
  }

  it("uses a configured small-index scan floor and admits FTS at the "
     "boundary") {
    put("needle.txt", "x");
    FsEngineConfig config = fsx_engine_config(database);
    config.fts_min_entries = 2;
    check_equal(fsx_engine_open(&config, &engine), FS_OK);
    scan();
    check_equal(count("needle"), (uint64_t)1);
    FsResultInfo info;
    fsx_result_info(held_result, &info);
    check_equal(info.candidate_strategy, FS_CANDIDATE_SCAN_COST);
    put("ordinary.txt", "x");
    scan();
    check_equal(count("needle"), (uint64_t)1);
    fsx_result_info(held_result, &info);
    check_equal(info.candidate_strategy, FS_CANDIDATE_FTS5);
  }
  it("uses FTS candidates with exact totals and scanner-equivalent pages") {
    for (unsigned i = 0; i < 256; i++) {
      char name[64];
      snprintf(name, sizeof(name), "ordinary-%03u.txt", i);
      put(name, "same");
    }
    put("unique-needle.txt", "target");
    open_engine();
    scan();
    open_oracle();
    FsResultInfo info = equivalent("needle", fsx_query_options());
    check_equal(info.total_count, (uint64_t)1);
    check_equal(info.candidate_strategy, FS_CANDIDATE_FTS5);
    check_equal(info.candidates_examined, (uint64_t)1);
    FsResultInfo baseline;
    fsx_result_info(held_result, &baseline);
    check_equal(baseline.candidates_examined, (uint64_t)257);
    FsQueryOptions options = fsx_query_options();
    options.limit = 0;
    info = equivalent("ordinary", options);
    check_equal(info.total_count, (uint64_t)256);
    check_equal(info.window_count, 0u);
    options.limit = 3;
    options.offset = 123;
    options.sort_key = FS_SORT_SIZE;
    options.flags = FS_QUERY_DESCENDING;
    info = equivalent("ordinary", options);
    check_equal(info.total_count, (uint64_t)256);
    check_equal(info.window_count, 3u);
  }
  it("preserves Unicode, punctuation and Boolean recall across candidate "
     "plans") {
    const char *names[] = {
        "alpha-beta.txt",
        "alpha.beta.md",
        "alphabet.txt",
        "beta.txt",
        "x",
        "Stra\xc3\x9f"
        "e.txt",
        "\xe4\xb8\xad\xe6\x96\x87\xe6\x8a\xa5\xe5\x91\x8a.txt",
        "\xf0\x9f\x98\x80\xf0\x9f\x98\x81\xf0\x9f\x98\x82.txt",
        "\xef\xac\x83.txt",
        "\xc4\xb0.txt",
        "alpha OR beta.txt",
        "Alpha.txt"};
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
      put(names[i], i & 1 ? "12" : "1234");
    subdir("folder-alpha");
    put("folder-alpha/inside.txt", "abc");
    open_engine();
    scan();
    open_oracle();
    struct {
      const char *text;
      FsCandidateStrategy strategy;
    } cases[] = {
        {"alpha", FS_CANDIDATE_FTS5},
        {"case:Alpha", FS_CANDIDATE_FTS5},
        {"STRASSE", FS_CANDIDATE_FTS5},
        {"case:STRASSE", FS_CANDIDATE_FTS5},
        {"\xe4\xb8\xad\xe6\x96\x87", FS_CANDIDATE_SCAN},
        {"\xe4\xb8\xad\xe6\x96\x87\xe6\x8a\xa5", FS_CANDIDATE_FTS5},
        {"\xf0\x9f\x98\x80\xf0\x9f\x98\x81", FS_CANDIDATE_SCAN},
        {"\xf0\x9f\x98\x80\xf0\x9f\x98\x81\xf0\x9f\x98\x82", FS_CANDIDATE_FTS5},
        {"\xef\xac\x83", FS_CANDIDATE_FTS5},
        {"case:\xef\xac\x83", FS_CANDIDATE_SCAN},
        {"\xc4\xb0", FS_CANDIDATE_SCAN},
        {"alpha | beta", FS_CANDIDATE_FTS5},
        {"alpha beta", FS_CANDIDATE_FTS5},
        {"<alpha !txt> | <beta file:>", FS_CANDIDATE_FTS5},
        {"<alpha | x> beta", FS_CANDIDATE_FTS5},
        {"alpha | x", FS_CANDIDATE_SCAN},
        {"alpha | !beta", FS_CANDIDATE_SCAN},
        {"!alpha", FS_CANDIDATE_SCAN},
        {"!<alpha | beta>", FS_CANDIDATE_SCAN},
        {"alpha | path:inside", FS_CANDIDATE_SCAN},
        {"alpha | regex:inside", FS_CANDIDATE_SCAN},
        {"alpha !path:folder", FS_CANDIDATE_FTS5},
        {"alpha regex:\"[.]txt$\"", FS_CANDIDATE_FTS5},
        {"alpha size:>=3B ext:txt", FS_CANDIDATE_FTS5},
        {"path:folder inside", FS_CANDIDATE_FTS5},
        {"path:alpha", FS_CANDIDATE_SCAN},
        {"*alpha*", FS_CANDIDATE_SCAN},
        {"\"alpha-beta\"", FS_CANDIDATE_FTS5},
        {"\"alpha OR beta\"", FS_CANDIDATE_FTS5},
        {"\"alpha\\\"beta\"", FS_CANDIDATE_FTS5},
        {"case:<Alpha | beta>", FS_CANDIDATE_FTS5},
        {"", FS_CANDIDATE_SCAN},
        {"absent", FS_CANDIDATE_FTS5}};
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
      for (uint32_t sort = FS_SORT_NAME; sort <= FS_SORT_MTIME; sort++) {
        FsQueryOptions options = fsx_query_options();
        options.sort_key = sort;
        options.flags = sort & 1 ? FS_QUERY_DESCENDING : 0;
        FsResultInfo info = equivalent(cases[i].text, options);
        check_equal(info.candidate_strategy, cases[i].strategy);
        options.offset = 1;
        options.limit = 2;
        options.flags ^= FS_QUERY_DESCENDING;
        equivalent(cases[i].text, options);
      }
    }
    FsQueryOptions options = fsx_query_options();
    options.flags = FS_QUERY_CASE_SENSITIVE;
    equivalent("Alpha | beta", options);
  }
  it("scans a retained revision when another writer advances the database") {
    put("oldneedle.txt", "x");
    open_engine();
    scan();
    open_oracle();
    char path[2048];
    snprintf(path, sizeof(path), "%s/oldneedle.txt", root);
    remove_utf8(path);
    put("newneedle.txt", "y");
    FsScanInfo scanned;
    check_equal(fsx_engine_scan(oracle_engine, root, &scanned), FS_OK);
    check_equal(scanned.snapshot_epoch, (uint64_t)2);
    check_equal(count("oldneedle"), (uint64_t)1);
    FsResultInfo info;
    fsx_result_info(held_result, &info);
    check_equal(info.snapshot_epoch, (uint64_t)1);
    check_equal(info.candidate_strategy, FS_CANDIDATE_SCAN_REVISION);
    check_equal(entry(0, NULL).name, "oldneedle.txt");
    check_equal(count("newneedle"), (uint64_t)0);
    scan();
    check_equal(count("newneedle"), (uint64_t)1);
    fsx_result_info(held_result, &info);
    check_equal(info.candidate_strategy, FS_CANDIDATE_FTS5);
  }
  it("fails a changed database identity instead of using unrelated postings") {
    put("needle.txt", "x");
    open_engine();
    scan();
    edit_database("UPDATE fsx_identity SET value=randomblob(16) WHERE id=1");
    check_equal(execute("needle", fsx_query_options()), FS_STALE);
    check_null(held_result);
  }
  it("reads legacy indexes without migration and upgrades them on write open") {
    put("needle.txt", "x");
    open_engine();
    FsScanInfo scanned = scan();
    check_equal(fsx_engine_close(&engine, FSX_DEADLINE_INFINITE), FS_OK);
    edit_database("DROP TABLE fsx_identity");
    FsEngineConfig config = fsx_engine_config(database);
    config.flags = 0;
    check_equal(fsx_engine_open(&config, &engine), FS_OK);
    check_equal(count("needle"), (uint64_t)1);
    uint64_t key = entry(0, NULL).entry_key;
    FsResultInfo info;
    fsx_result_info(held_result, &info);
    check_equal(info.candidate_strategy, FS_CANDIDATE_SCAN_LEGACY);
    fsx_result_release(held_result);
    held_result = NULL;
    fsx_query_release(held_query);
    held_query = NULL;
    check_equal(fsx_engine_close(&engine, FSX_DEADLINE_INFINITE), FS_OK);
    /* CREATE proves read-existing left the optional table absent. */
    edit_database("CREATE TABLE fsx_identity(id INTEGER PRIMARY KEY,value "
                  "BLOB); DROP TABLE fsx_identity");
    open_engine();
    check_equal(count("needle"), (uint64_t)1);
    check_equal(entry(0, NULL).entry_key, key);
    fsx_result_info(held_result, &info);
    check_equal(info.snapshot_epoch, scanned.snapshot_epoch);
    check_equal(info.candidate_strategy, FS_CANDIDATE_FTS5);
  }
  it("uses its owned snapshot for an in-memory database") {
    put("needle.txt", "x");
    FsEngineConfig config = fsx_engine_config(":memory:");
    check_equal(fsx_engine_open(&config, &engine), FS_OK);
    scan();
    check_equal(count("needle"), (uint64_t)1);
    FsResultInfo info;
    fsx_result_info(held_result, &info);
    check_equal(info.candidate_strategy, FS_CANDIDATE_SCAN_MEMORY);
  }
  it("rejects an FTS row that has no stable key in the admitted snapshot") {
    put("needle.txt", "x");
    open_engine();
    scan();
    edit_database("INSERT INTO name_fts(rowid,name,name_fold) "
                  "VALUES(999999,'needle','needle')");
    check_equal(execute("needle", fsx_query_options()), FS_CORRUPT);
    check_null(held_result);
  }
  it("releases the FTS read transaction before publishing retained results") {
    put("needle.txt", "x");
    open_engine();
    scan();
    check_equal(count("needle"), (uint64_t)1);
    put("newneedle.txt", "y");
    scan();
    sqlite3 *db = NULL;
    int opened = sqlite3_open_v2(database, &db, SQLITE_OPEN_READWRITE, NULL);
    /* Opening alone need not load the persistent WAL journal mode. Complete a
     * read first so checkpoint counters prove that WAL was actually checked. */
    if (opened == SQLITE_OK)
      opened =
          sqlite3_exec(db, "SELECT revision FROM fsx_meta", NULL, NULL, NULL);
    int frames = -1, checkpointed = -1;
    int rc =
        opened == SQLITE_OK
            ? sqlite3_wal_checkpoint_v2(db, NULL, SQLITE_CHECKPOINT_TRUNCATE,
                                        &frames, &checkpointed)
            : opened;
    sqlite3_close(db);
    check_equal(rc, SQLITE_OK);
    check_equal(frames, 0);
    check_equal(checkpointed, 0);
    check_equal(entry(0, NULL).name, "needle.txt");
    FsResultInfo info;
    fsx_result_info(held_result, &info);
    check_equal(info.snapshot_epoch, (uint64_t)1);
  }
  it("fails a managed allocation budget without publishing a partial result") {
    put("file.txt", "x");
    FsEngineConfig config = fsx_engine_config(database);
    config.owned_memory_budget = 32768;
    check_equal(fsx_engine_open(&config, &engine), FS_OK);
    scan();
    check_equal(execute("file", fsx_query_options()), FS_NOMEM);
    check_null(held_result);
    FsProgress progress;
    fsx_engine_progress(engine, &progress);
    check_true(progress.owned_bytes <= config.owned_memory_budget);
  }
  it("rejects embedded NUL, malformed UTF-8 and unadmitted request versions") {
    open_engine();
    const char nul[] = {'a', 0, 'b'};
    FsQueryRequest r = {sizeof(r), FSX_API_VERSION, nul, sizeof(nul),
                        fsx_query_options()};
    check_equal(fsx_query_submit(engine, &r, &held_query), FS_INVALID);
    check_null(held_query);
    r.text = "\xc0\xaf";
    r.text_bytes = 2;
    check_equal(fsx_query_submit(engine, &r, &held_query), FS_INVALID);
    r.struct_size = 4;
    r.text_bytes = SIZE_MAX;
    check_equal(fsx_query_submit(engine, &r, &held_query), FS_INVALID);
    char deep[80];
    memset(deep, '!', sizeof(deep) - 2);
    deep[78] = 'a';
    deep[79] = 0;
    check_equal(execute(deep, fsx_query_options()), FS_LIMIT);
  }
  it("supports concurrent query senders while the single writer republishes") {
    put("steady.txt", "x");
    open_engine();
    scan();
    Sender sender = {0};
    sender.engine = engine;
    atomic_init(&sender.failures, 0);
    cmeta_thread_t threads[4] = {0};
    unsigned started = 0;
    int creation = 0;
    for (; started < 4; started++) {
      creation = cmeta_thread_create(&threads[started], sender_run, &sender);
      if (creation)
        break;
    }
    FsStatus status = FS_OK;
    for (unsigned i = 0; i < 20 && status == FS_OK; i++) {
      FsScanInfo info;
      status = fsx_engine_scan(engine, root, &info);
    }
    for (unsigned i = 0; i < started; i++) {
      int joined = cmeta_thread_join(&threads[i]);
      cmeta_thread_destroy(&threads[i]);
      if (joined)
        creation = joined;
    }
    check_equal(creation, 0);
    check_equal(status, FS_OK);
    check_equal(atomic_load(&sender.failures), 0u);
  }
#ifdef _WIN32
  it("compiles modification days using the query's fixed timezone offset") {
    put("dated.txt", "x");
    char path[2048];
    snprintf(path, sizeof(path), "%s/dated.txt", root);
    wchar_t w[2048];
    wide(path, w, 2048);
    HANDLE file =
        CreateFileW(w, FILE_WRITE_ATTRIBUTES,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    NULL, OPEN_EXISTING, 0, NULL);
    check(file != INVALID_HANDLE_VALUE, "timestamp fixture");
    SYSTEMTIME utc = {2026, 11, 0, 2, 0, 30, 0, 0};
    FILETIME time;
    check(SystemTimeToFileTime(&utc, &time) != 0, "UTC timestamp");
    check(SetFileTime(file, NULL, NULL, &time) != 0, "set fixture timestamp");
    CloseHandle(file);
    open_engine();
    scan();
    check_equal(count("dm:2026-11-02"), (uint64_t)1);
    FsQueryOptions options = fsx_query_options();
    options.timezone_offset_minutes = -60;
    check_equal(execute("dm:2026-11-01", options), FS_OK);
    FsResultInfo info;
    fsx_result_info(held_result, &info);
    check_equal(info.total_count, (uint64_t)1);
    check_equal(info.core_unicode_version, "17.0.0");
    check_true(info.regex_version[0] != 0);
    check_true(info.regex_unicode_version[0] != 0);
  }
#endif
}

/* Deterministic source provider drives the production transaction coordinator.
 * Native byte layouts, SQLite, FTS and retained result queries remain real. */
typedef struct NtNode {
  uint64_t frn, parent, size;
  uint32_t attributes;
  char name[48];
  bool live;
} NtNode;
typedef struct NtEvent {
  NtNode node;
  int64_t usn;
  uint32_t reason, length;
} NtEvent;
static struct {
  FsNtfsIdentity identity;
  NtNode nodes[32];
  NtEvent events[64];
  unsigned node_count, event_count, queries, lists, closed;
  bool inject_create, inject_gap, inject_directory, stale_metadata;
  int read_fault;
  FsStatus list_failure, link_status, inventory_failure;
  bool stale_inventory;
  unsigned inventory_repeat;
} nt;
static FsNtfs *native_source, *other_native_source;
static FsNtfsMetrics nt_metrics;
#define NT_FRN(index) (UINT64_C(0x8123000000000000) | (index))
static void nt16(unsigned char *p, uint16_t n) {
  p[0] = (unsigned char)n;
  p[1] = (unsigned char)(n >> 8);
}
static void nt32(unsigned char *p, uint32_t n) {
  for (unsigned i = 0; i < 4; i++)
    p[i] = (unsigned char)(n >> (8 * i));
}
static uint32_t nt_record(void *buffer, const NtNode *node, int64_t usn,
                          uint32_t reason) {
  unsigned char *p = buffer;
  size_t length = strlen(node->name);
  uint32_t size = (uint32_t)((60 + length * 2 + 7) & ~7u);
  memset(p, 0, size);
  nt32(p, size);
  nt16(p + 4, 2);
  fsx_ntfs_put64(p + 8, node->frn);
  fsx_ntfs_put64(p + 16, node->parent);
  fsx_ntfs_put64(p + 24, (uint64_t)usn);
  nt32(p + 40, reason);
  nt32(p + 52, node->attributes);
  nt16(p + 56, (uint16_t)(length * 2));
  nt16(p + 58, 60);
  for (size_t i = 0; i < length; i++)
    nt16(p + 60 + 2 * i, (unsigned char)node->name[i]);
  return size;
}
static NtNode *nt_add(unsigned id, unsigned parent, const char *name,
                      bool dir) {
  check_true(nt.node_count < 32);
  NtNode *node = &nt.nodes[nt.node_count++];
  memset(node, 0, sizeof(*node));
  node->frn = NT_FRN(id);
  node->parent = NT_FRN(parent);
  node->live = true;
  node->size = 7;
  node->attributes = dir ? FSX_NTFS_DIR : 0;
  snprintf(node->name, sizeof(node->name), "%s", name);
  return node;
}
static void nt_event(NtNode *node, uint32_t reason) {
  check_true(nt.event_count < 64);
  NtEvent *e = &nt.events[nt.event_count++];
  e->node = *node;
  e->usn = nt.identity.next_usn;
  e->reason = reason;
  unsigned char record[256];
  e->length = nt_record(record, node, e->usn, reason);
  nt.identity.next_usn += e->length;
}
static FsStatus nt_query(void *context, FsNtfsIdentity *out) {
  (void)context;
  if (index_gate.native && pause_index(NULL))
    return FS_CANCELLED;
  if (++nt.queries == 3 && nt.inject_create) {
    NtNode *created = nt_add(40, 20, "catchup.txt", false);
    nt_event(created, FSX_USN_CREATE);
  }
  if (nt.queries == 3 && nt.inject_gap) {
    nt.identity.next_usn += 8;
    nt.identity.first_usn = nt.identity.next_usn;
  }
  *out = nt.identity;
  return FS_OK;
}
static FsStatus nt_mft(void *context, uint64_t from, void *data,
                       size_t capacity, size_t *size) {
  (void)context;
  unsigned char *p = data;
  *size = 8;
  uint64_t last = from;
  bool any = false;
  for (unsigned i = 0; i < nt.node_count; i++) {
    NtNode *node = &nt.nodes[i];
    uint64_t index = node->frn & UINT64_C(0x0000ffffffffffff);
    bool duplicate = false;
    for (unsigned j = 0; j < i; j++)
      if (nt.nodes[j].live && nt.nodes[j].frn == node->frn)
        duplicate = true;
    if (!node->live || index < from || duplicate)
      continue;
    unsigned char record[256];
    uint32_t bytes = nt_record(record, node, 0, 0);
    if (*size + bytes > capacity)
      break;
    memcpy(p + *size, record, bytes);
    *size += bytes;
    last = index + 1;
    any = true;
  }
  if (!any)
    return FS_END;
  fsx_ntfs_put64(p, last);
  return FS_OK;
}
static FsStatus nt_read(void *context, int64_t from, uint64_t journal,
                        void *data, size_t capacity, size_t *size) {
  (void)context;
  if (journal != nt.identity.journal_id || from < nt.identity.first_usn)
    return FS_STALE;
  unsigned char *p = data;
  *size = 8;
  int64_t next = from;
  for (unsigned i = 0; i < nt.event_count; i++) {
    NtEvent *e = &nt.events[i];
    if (e->usn < from)
      continue;
    if (*size + e->length > capacity)
      break;
    *size += nt_record(p + *size, &e->node, e->usn, e->reason);
    next = e->usn + e->length;
  }
  if (*size == 8)
    next = nt.identity.next_usn;
  fsx_ntfs_put64(p, (uint64_t)next);
  if (nt.read_fault == 1 && *size > 8)
    --*size;
  if (nt.read_fault == 2 && *size > 8)
    nt16(p + 12, 3);
  return FS_OK;
}
static void nt_observed(NtNode *node, FsObserved *o) {
  memset(o, 0, sizeof(*o));
  o->file_lo = node->frn;
  o->size = node->size;
  o->mtime = 100;
  o->flags = FS_ENTRY_MTIME_VALID |
             (node->attributes & FSX_NTFS_DIR ? FS_ENTRY_DIRECTORY
                                              : FS_ENTRY_SIZE_VALID);
  if (node->attributes & FSX_NTFS_REPARSE)
    o->flags |= FS_ENTRY_LINK;
}
static FsStatus nt_list(void *context, uint64_t parent, FsObserve visit,
                        void *target) {
  (void)context;
  nt.lists++;
  if (nt.inject_directory && parent == NT_FRN(10)) {
    nt.inject_directory = false;
    NtNode *dir = nt_add(40, 10, "late-directory", true);
    nt_event(dir, FSX_USN_CREATE);
    nt_add(50, 40, "late-file.txt", false);
  }
  if (nt.list_failure != FS_OK)
    return nt.list_failure;
  bool exists = false;
  for (unsigned i = 0; i < nt.node_count; i++)
    if (nt.nodes[i].live && nt.nodes[i].frn == parent)
      exists = true;
  if (!exists)
    return FS_END;
  for (unsigned i = 0; i < nt.node_count; i++) {
    NtNode *node = &nt.nodes[i];
    if (!node->live || node->parent != parent)
      continue;
    unsigned char raw[96];
    size_t n = strlen(node->name);
    for (size_t j = 0; j < n; j++)
      nt16(raw + 2 * j, (unsigned char)node->name[j]);
    FsObserved o;
    nt_observed(node, &o);
    if (nt.stale_metadata) {
      o.size = 1;
      o.mtime = 1;
    }
    o.name = node->name;
    o.raw_name = raw;
    o.raw_bytes = 2 * n;
    FsStatus status = visit(target, &o);
    if (status != FS_OK)
      return status;
  }
  return FS_OK;
}
static FsStatus nt_stat(void *context, uint64_t frn, FsObserved *out) {
  (void)context;
  for (unsigned i = 0; i < nt.node_count; i++)
    if (nt.nodes[i].live && nt.nodes[i].frn == frn) {
      nt_observed(&nt.nodes[i], out);
      return FS_OK;
    }
  return FS_END;
}
static FsStatus nt_links(void *context, uint64_t frn, FsNtfsParentFn visit,
                         void *target) {
  (void)context;
  if (nt.link_status != FS_OK)
    return nt.link_status;
  bool found = false;
  for (unsigned i = 0; i < nt.node_count; i++)
    if (nt.nodes[i].live && nt.nodes[i].frn == frn) {
      found = true;
      FsStatus status = visit(target, nt.nodes[i].parent);
      if (status != FS_OK)
        return status;
    }
  return found ? FS_OK : FS_END;
}
static FsStatus nt_inventory(void *context, FsNtfsRecordFn visit, void *target,
                             FsNtfsRawStats *stats) {
  (void)context;
  memset(stats, 0, sizeof(*stats));
  stats->slots = nt.node_count;
  stats->read_calls = 1;
  stats->read_bytes = 4096;
  stats->kernel_records = 2;
  for (unsigned i = 0; i < nt.node_count; i++) {
    NtNode *node = &nt.nodes[i];
    if (!node->live)
      continue;
    stats->records++;
    if (!(node->attributes & FSX_NTFS_DIR))
      continue;
    unsigned char raw[96];
    size_t bytes = strlen(node->name) * 2;
    for (size_t j = 0; j < bytes / 2; j++)
      nt16(raw + j * 2, (unsigned char)node->name[j]);
    FsNtfsRecord item = {0};
    item.frn = node->frn;
    item.parent = node->parent;
    item.attributes = node->attributes;
    item.name = raw;
    item.name_bytes = (uint16_t)bytes;
    if (nt.stale_inventory && node->frn == NT_FRN(40))
      item.parent = NT_FRN(10); /* A moved-out directory still on disk. */
    if (nt.stale_inventory && node->frn == NT_FRN(60))
      item.parent = NT_FRN(40); /* Must be checked when 40 later enters. */
    for (unsigned repeat = 0; repeat <= nt.inventory_repeat; repeat++) {
      FsStatus status = visit(target, &item);
      if (status != FS_OK)
        return status;
    }
    if (nt.inventory_failure != FS_OK)
      return nt.inventory_failure;
  }
  return FS_OK;
}
static uint32_t nt_error(void *context) {
  (void)context;
  return nt.list_failure == FS_DENIED ? 5 : 0;
}
static void nt_close(void *context) {
  (void)context;
  nt.closed++;
}
static const FsNtfsOps nt_ops = {nt_query, nt_mft,   nt_read,
                                 nt_list,  nt_stat,  nt_error,
                                 nt_close, nt_links, nt_inventory};
static void nt_setup(void) {
  setup();
  memset(&nt, 0, sizeof(nt));
  native_source = other_native_source = NULL;
  nt.identity = (FsNtfsIdentity){0};
  nt.identity.volume_serial = UINT64_C(0xfedcba9876543210);
  nt.identity.common_volume = 123;
  nt.identity.journal_id = UINT64_C(0xabcdef9876543210);
  nt.identity.root_frn = NT_FRN(10);
  nt.identity.volume_root_frn = NT_FRN(5);
  nt.identity.next_usn = 1024;
  strcpy(nt.identity.volume_guid, "test-volume");
  nt_add(10, 5, "tree", true);
  nt_add(20, 10, "sub", true);
  nt_add(30, 20, "alpha.txt", false);
  open_engine();
  check_equal(fsx_native_volume(engine, root, &nt.identity.common_volume),
              FS_OK);
}
static void nt_open(uint32_t batch) {
  FsNtfsConfig config = fsx_ntfs_config();
  config.max_batch_records = batch;
  check_equal(fsx_ntfs_open_provider(engine, root, &config, &nt_ops, &nt,
                                     &native_source),
              FS_OK);
}
static FsNtfsInfo nt_build(void) {
  nt_open(4096);
  FsNtfsInfo out;
  FsStatus status = fsx_ntfs_rebuild(native_source, &out);
  check(status == FS_OK, "native rebuild: %s (%s)", fsx_status_name(status),
        sqlite3_errmsg(engine->db));
  check_true(out.caught_up);
  return out;
}
static int64_t nt_scalar(const char *text) {
  sqlite3_stmt *s = NULL;
  check_equal(sqlite3_prepare_v2(engine->db, text, -1, &s, NULL), SQLITE_OK);
  check_equal(sqlite3_step(s), SQLITE_ROW);
  int64_t out = sqlite3_column_int64(s, 0);
  sqlite3_finalize(s);
  return out;
}
typedef struct ParentIds {
  uint64_t ids[4];
  uint32_t count;
} ParentIds;
static FsStatus capture_parent(void *context, uint64_t parent) {
  ParentIds *ids = context;
  if (ids->count == 4)
    return FS_LIMIT;
  ids->ids[ids->count++] = parent;
  return FS_OK;
}
typedef struct MergeStop {
  bool armed, interrupted;
} MergeStop;
static int arm_merge_stop(unsigned event, void *context, void *statement,
                          void *duration) {
  (void)duration;
  MergeStop *stop = context;
  if (event == SQLITE_TRACE_STMT) {
    const char *text = sqlite3_sql(statement);
    stop->armed =
        text && strstr(text, "SELECT source_id,frn,parent,name,raw,attributes "
                             "FROM nt_inventory");
  }
  return 0;
}
static int stop_merge(void *context) {
  MergeStop *stop = context;
  if (!stop->armed)
    return 0;
  stop->interrupted = true;
  atomic_store(&engine->stopping, true);
  return 1;
}
static void nt_raw_open(uint64_t flags, uint32_t cap) {
  FsNtfsConfig config = fsx_ntfs_config();
  config.mft_reader = FS_NTFS_MFT_RAW;
  config.raw_flags = flags;
  config.max_mft_records = cap;
  check_equal(fsx_ntfs_open_provider(engine, root, &config, &nt_ops, &nt,
                                     &native_source),
              FS_OK);
}
suite("NTFS transaction protocol") {
  before_each() { nt_setup(); }
  after_each() {
    finish_index();
    if (other_native_source)
      check_equal(fsx_ntfs_close(&other_native_source), FS_OK);
    if (native_source)
      check_equal(fsx_ntfs_close(&native_source), FS_OK);
    teardown();
  }

  it("keeps serial and bounded native pipelines equivalent across rebuilds") {
    nt.inventory_repeat = 256;
    for (uint32_t batches = 0; batches <= 2; batches++) {
      FsNtfsConfig config = fsx_ntfs_config();
      config.mft_reader = FS_NTFS_MFT_RAW;
      config.buffer_bytes = 4096;
      config.pipeline_batches = batches;
      check_equal(fsx_ntfs_open_provider(engine, root, &config, &nt_ops, &nt,
                                         &native_source),
                  FS_OK);
      native_source->metrics = &nt_metrics;
      FsNtfsInfo info;
      check_equal(fsx_ntfs_rebuild(native_source, &info), FS_OK);
      check_true(info.caught_up);
      check_equal(count("alpha"), (uint64_t)1);
      check_equal(info.mft_records, (uint64_t)3);
      check_true(info.pipeline_peak_batches <= batches);
      if (batches)
        check_true(info.pipeline_batches > 1);
      else
        check_equal(info.pipeline_batches, (uint64_t)0);
      if (batches == 1) {
        check_true(nt_metrics.producer_waits > 0);
        check_true(nt_metrics.producer_wait_ns > 0);
        check_true(nt_metrics.consumer_callback_ns > 0);
      }
      check_true(nt_metrics.total_ns > 0);
      check_true(nt_metrics.inventory_ns > 0);
      check_equal(fsx_ntfs_sync(native_source, &info), FS_OK);
      check_equal(nt_metrics.inventory_ns, UINT64_C(0));
      check_equal(nt_metrics.producer_waits, UINT64_C(0));
      check_equal(nt_metrics.consumer_waits, UINT64_C(0));
      check_equal(fsx_ntfs_close(&native_source), FS_OK);
    }
  }
  it("pins an asynchronous NTFS session while old queries remain available") {
    FsNtfsInfo before = nt_build();
    nt.nodes[2].size = 70;
    nt_event(&nt.nodes[2], 1);
    gate_start(true);
    check_equal(fsx_ntfs_sync_async(native_source, &held_index), FS_OK);
    gate_wait();
    check_equal(fsx_ntfs_close(&native_source), FS_BUSY);
    FsNtfsInfo rejected;
    check_equal(fsx_ntfs_sync(native_source, &rejected), FS_BUSY);
    check_equal(count("alpha"), (uint64_t)1);
    check_equal(entry(0, NULL).size, (uint64_t)7);
    retained_result = held_result;
    held_result = NULL;
    gate_release();
    check_equal(
        fsx_index_wait(held_index, cmeta_hrtime() + UINT64_C(5000000000)),
        FS_OK);
    check_equal(fsx_ntfs_close(&native_source), FS_OK);
    FsIndexInfo info;
    check_equal(fsx_index_poll(held_index, &info), FS_OK);
    check_equal(info.ntfs.next_usn, nt.identity.next_usn);
    check_equal(info.ntfs.snapshot_epoch, before.snapshot_epoch + 1);
    check_equal(count("alpha"), (uint64_t)1);
    check_equal(entry(0, NULL).size, (uint64_t)70);
    FsResultInfo old;
    fsx_result_info(retained_result, &old);
    check_equal(old.snapshot_epoch, before.snapshot_epoch);
  }
  it("retains the durable USN cursor after cancelling an asynchronous batch") {
    FsNtfsInfo before = nt_build();
    nt.nodes[2].size = 70;
    nt_event(&nt.nodes[2], 1);
    gate_start(true);
    check_equal(fsx_ntfs_sync_async(native_source, &held_index), FS_OK);
    gate_wait();
    fsx_index_cancel(held_index);
    check_equal(
        fsx_index_wait(held_index, cmeta_hrtime() + UINT64_C(5000000000)),
        FS_CANCELLED);
    check_equal(nt_scalar("SELECT next_usn FROM fsx_ntfs_sources"),
                before.next_usn);
    check_equal(nt_scalar("SELECT revision FROM fsx_meta"),
                (int64_t)before.snapshot_epoch);
    check_true(sqlite3_get_autocommit(engine->db) != 0);
    gate_release();
    FsNtfsInfo retried;
    check_equal(fsx_ntfs_sync(native_source, &retried), FS_OK);
    check_equal(retried.next_usn, nt.identity.next_usn);
  }
  it("bounds memory work pages and rolls back facts before a larger-budget "
     "retry") {
    put("baseline.txt", "x");
    FsScanInfo before = scan();
    check_equal(sqlite3_exec(engine->db, "PRAGMA fsx_work.max_page_count=1",
                             NULL, NULL, NULL),
                SQLITE_OK);
    nt_open(4096);
    FsNtfsInfo info;
    check_equal(fsx_ntfs_rebuild(native_source, &info), FS_LIMIT);
    check_equal(nt_scalar("SELECT revision FROM fsx_meta"),
                (int64_t)before.snapshot_epoch);
    check_equal(count("baseline"), (uint64_t)1);
    check_true(sqlite3_get_autocommit(engine->db) != 0);
    check_equal(sqlite3_exec(engine->db, "PRAGMA fsx_work.max_page_count=4096",
                             NULL, NULL, NULL),
                SQLITE_OK);
    check_equal(fsx_ntfs_rebuild(native_source, &info), FS_OK);
    check_equal(count("alpha"), (uint64_t)1);
    check_equal(
        nt_scalar(
            "SELECT count(*) FROM main.sqlite_schema WHERE name='nt_changes'"),
        (int64_t)0);
  }
  it("cancels with a borrowed full queue before releasing native buffers") {
    nt_raw_open(FS_NTFS_RAW_BATCH_DIRS, 16000000);
    native_source->config.pipeline_batches = 1;
    native_source->config.buffer_bytes = 4096;
    FsNtfsInfo before;
    check_equal(fsx_ntfs_rebuild(native_source, &before), FS_OK);
    nt.inventory_repeat = 1000;
    gate_start(false);
    index_gate.armed = false;
    engine->sql_observer = pause_index;
    sqlite3_trace_v2(engine->db, SQLITE_TRACE_STMT, pause_inventory, NULL);
    check_equal(fsx_ntfs_rebuild_async(native_source, &held_index), FS_OK);
    gate_wait();
    fsx_index_cancel(held_index);
    check_equal(
        fsx_index_wait(held_index, cmeta_hrtime() + UINT64_C(5000000000)),
        FS_CANCELLED);
    engine->sql_observer = NULL;
    sqlite3_trace_v2(engine->db, 0, NULL, NULL);
    check_true(sqlite3_get_autocommit(engine->db) != 0);
    check_equal(nt_scalar("SELECT revision FROM fsx_meta"),
                (int64_t)before.snapshot_epoch);
    check_equal(nt_scalar("SELECT next_usn FROM fsx_ntfs_sources"),
                before.next_usn);
    check_equal(fsx_ntfs_close(&native_source), FS_OK);
  }
  it("preserves a consumer SQL error while draining the native producer") {
    nt_raw_open(FS_NTFS_RAW_BATCH_DIRS, 16000000);
    FsNtfsInfo before;
    check_equal(fsx_ntfs_rebuild(native_source, &before), FS_OK);
    nt.inventory_repeat = 1000;
    check_equal(
        sqlite3_exec(
            engine->db,
            "CREATE TEMP TRIGGER refuse_pipeline BEFORE INSERT ON nt_inventory "
            "BEGIN SELECT RAISE(ABORT,'pipeline consumer'); END",
            NULL, NULL, NULL),
        SQLITE_OK);
    FsNtfsInfo failed;
    check_equal(fsx_ntfs_rebuild(native_source, &failed), FS_CORRUPT);
    check_true(sqlite3_get_autocommit(engine->db) != 0);
    check_equal(nt_scalar("SELECT revision FROM fsx_meta"),
                (int64_t)before.snapshot_epoch);
    check_equal(nt_scalar("SELECT next_usn FROM fsx_ntfs_sources"),
                before.next_usn);
    check_equal(sqlite3_exec(engine->db, "DROP TRIGGER refuse_pipeline", NULL,
                             NULL, NULL),
                SQLITE_OK);
    check_equal(fsx_ntfs_rebuild(native_source, &failed), FS_OK);
  }
  it("supports disk-backed work storage with the same committed results") {
    check_equal(fsx_engine_close(&engine, FSX_DEADLINE_INFINITE), FS_OK);
    FsEngineConfig config = fsx_engine_config(database);
    config.work_memory_pages = 0;
    check_equal(fsx_engine_open(&config, &engine), FS_OK);
    FsNtfsInfo info = nt_build();
    check_true(info.caught_up);
    check_equal(count("alpha"), (uint64_t)1);
  }

  it("validates complete native hardlink buffers and preserves full parent "
     "FRNs") {
    unsigned char raw[64] = {0};
    nt32(raw, 30);
    nt32(raw + 4, 1);
    fsx_ntfs_put64(raw + 16, NT_FRN(20));
    nt32(raw + 24, 1);
    nt16(raw + 28, 'a');
    ParentIds parents = {0};
    uint32_t count;
    check_equal(
        fsx_ntfs_link_records(raw, 30, 4, capture_parent, &parents, &count),
        FS_OK);
    check_equal(count, 1u);
    check_equal(parents.ids[0], NT_FRN(20));
    nt32(raw + 4, 2);
    check_equal(
        fsx_ntfs_link_records(raw, 30, 4, capture_parent, &parents, &count),
        FS_CORRUPT);
    nt32(raw + 4, 1);
    nt32(raw + 24, UINT32_MAX);
    check_equal(
        fsx_ntfs_link_records(raw, 30, 4, capture_parent, &parents, &count),
        FS_CORRUPT);
  }
  it("verifies stale raw directory edges and newly entering cached scopes") {
    NtNode *outside = nt_add(40, 90, "outside", true);
    nt_add(50, 40, "arriving.txt", false);
    nt_add(60, 90, "elsewhere", true);
    nt_add(70, 60, "ghost.txt", false);
    nt.stale_inventory = true;
    FsNtfsConfig config = fsx_ntfs_config();
    config.mft_reader = FS_NTFS_MFT_RAW;
    check_equal(fsx_ntfs_open_provider(engine, root, &config, &nt_ops, &nt,
                                       &native_source),
                FS_OK);
    FsNtfsInfo info;
    check_equal(fsx_ntfs_rebuild(native_source, &info), FS_OK);
    check_equal(info.mft_slots, (uint64_t)7);
    check_equal(count("ghost"), (uint64_t)0);
    check_equal(count("arriving"), (uint64_t)0);
    check_equal(count("alpha"), (uint64_t)1);
    check_equal(fsx_ntfs_close(&native_source), FS_OK);
    nt_open(4096); /* ENUM resume must also verify newly entering raw-cache
                      nodes. */
    outside->parent = NT_FRN(10);
    nt_event(outside, FSX_USN_NEW);
    check_equal(fsx_ntfs_sync(native_source, &info), FS_OK);
    check_equal(count("arriving"), (uint64_t)1);
    check_equal(count("ghost"), (uint64_t)0);
  }
  it("rejects old raw config layouts and an unavailable raw provider without "
     "downgrade") {
    FsNtfsConfig config = fsx_ntfs_config();
    config.struct_size = offsetof(FsNtfsConfig, mft_reader);
    check_equal(fsx_ntfs_open(engine, root, &config, &native_source),
                FS_INVALID);
    check_null(native_source);
    config = fsx_ntfs_config();
    config.struct_size = offsetof(FsNtfsConfig, raw_flags);
    check_equal(fsx_ntfs_open(engine, root, &config, &native_source),
                FS_INVALID);
    check_null(native_source);
    config = fsx_ntfs_config();
    config.raw_flags = UINT64_C(4);
    check_equal(fsx_ntfs_open(engine, root, &config, &native_source),
                FS_INVALID);
    check_null(native_source);
    config = fsx_ntfs_config();
    config.mft_reader = FS_NTFS_MFT_RAW;
    static const FsNtfsOps no_raw = {nt_query, nt_mft,   nt_read,
                                     nt_list,  nt_stat,  nt_error,
                                     nt_close, nt_links, NULL};
    check_equal(fsx_ntfs_open_provider(engine, root, &config, &no_raw, &nt,
                                       &native_source),
                FS_OK);
    FsNtfsInfo info;
    check_equal(fsx_ntfs_rebuild(native_source, &info), FS_UNSUPPORTED);
    check_equal(nt.lists, 0u);
    check_equal(nt_scalar("SELECT count(*) FROM sources"), (int64_t)0);
  }
  it("rolls back a partially emitted raw inventory and permits retry") {
    FsNtfsInfo before = nt_build();
    check_equal(fsx_ntfs_close(&native_source), FS_OK);
    FsNtfsConfig config = fsx_ntfs_config();
    config.mft_reader = FS_NTFS_MFT_RAW;
    check_equal(fsx_ntfs_open_provider(engine, root, &config, &nt_ops, &nt,
                                       &native_source),
                FS_OK);
    nt.inventory_failure = FS_IO;
    FsNtfsInfo info;
    check_equal(fsx_ntfs_rebuild(native_source, &info), FS_IO);
    check_equal(nt_scalar("SELECT revision FROM fsx_meta"),
                (int64_t)before.snapshot_epoch);
    check_equal(count("alpha"), (uint64_t)1);
    nt.inventory_failure = FS_OK;
    check_equal(fsx_ntfs_rebuild(native_source, &info), FS_OK);
    check_equal(count("alpha"), (uint64_t)1);
  }
  it("keeps direct and staged raw graphs equivalent with ordered duplicate "
     "FRNs") {
    nt_add(40, 90, "first", true);
    nt_add(40, 91, "last", true);
    FsNtfsInfo info;
    for (uint64_t flags = 0; flags < 4; flags++) {
      nt_raw_open(flags, 100);
      check_equal(fsx_ntfs_rebuild(native_source, &info), FS_OK);
      check_equal(count("alpha"), (uint64_t)1);
      check_equal(
          nt_scalar("SELECT count(*) FROM fsx_ntfs_dirs WHERE name='last'"),
          (int64_t)1);
      check_equal(
          nt_scalar("SELECT count(*) FROM fsx_ntfs_dirs WHERE name='first'"),
          (int64_t)0);
      check_equal(nt_scalar("SELECT count(*) FROM temp.nt_inventory"),
                  (int64_t)0);
      check_equal(fsx_ntfs_close(&native_source), FS_OK);
    }
  }
  it("rolls back a directory merge failure and preserves unrelated sources on "
     "retry") {
    FsNtfsInfo before = nt_build();
    check_equal(fsx_ntfs_close(&native_source), FS_OK);
    subdir("other");
    put("other/independent.txt", "x");
    char other_root[2048];
    snprintf(other_root, sizeof(other_root), "%s/other", root);
    FsScanInfo scanned;
    check_equal(fsx_engine_scan(engine, other_root, &scanned), FS_OK);
    int64_t revision = nt_scalar("SELECT revision FROM fsx_meta");
    nt_raw_open(FS_NTFS_RAW_PREFETCH | FS_NTFS_RAW_BATCH_DIRS, 100);
    edit_database("CREATE TRIGGER refuse_graph BEFORE INSERT ON fsx_ntfs_dirs "
                  "BEGIN SELECT RAISE(ABORT,'injected merge failure'); END;");
    FsNtfsInfo info;
    check_equal(fsx_ntfs_rebuild(native_source, &info), FS_CORRUPT);
    check_equal(nt_scalar("SELECT revision FROM fsx_meta"), revision);
    check_equal(nt_scalar("SELECT next_usn FROM fsx_ntfs_sources"),
                before.next_usn);
    check_equal(count("alpha"), (uint64_t)1);
    check_equal(count("independent"), (uint64_t)1);
    check_equal(nt_scalar("SELECT count(*) FROM temp.nt_inventory"),
                (int64_t)0);
    edit_database("DROP TRIGGER refuse_graph");
    check_equal(fsx_ntfs_rebuild(native_source, &info), FS_OK);
    check_equal(count("alpha"), (uint64_t)1);
    check_equal(count("independent"), (uint64_t)1);
    open_oracle();
    equivalent("independent", fsx_query_options());
  }
  it("bounds staged callbacks independently of provider inventory counters") {
    FsNtfsInfo before = nt_build();
    check_equal(fsx_ntfs_close(&native_source), FS_OK);
    nt.inventory_repeat =
        4; /* Provider reports three records, emits ten dirs. */
    nt_raw_open(FS_NTFS_RAW_BATCH_DIRS, 4);
    FsNtfsInfo info;
    check_equal(fsx_ntfs_rebuild(native_source, &info), FS_LIMIT);
    check_equal(nt_scalar("SELECT revision FROM fsx_meta"),
                (int64_t)before.snapshot_epoch);
    check_equal(nt_scalar("SELECT next_usn FROM fsx_ntfs_sources"),
                before.next_usn);
    check_equal(count("alpha"), (uint64_t)1);
    check_equal(nt_scalar("SELECT count(*) FROM temp.nt_inventory"),
                (int64_t)0);
    nt.inventory_repeat = 0;
    check_equal(fsx_ntfs_rebuild(native_source, &info), FS_OK);
  }
  it("interrupts the bulk merge on STOPPING without publishing or advancing "
     "the checkpoint") {
    FsNtfsInfo before = nt_build();
    check_equal(fsx_ntfs_close(&native_source), FS_OK);
    check_equal(count("alpha"),
                (uint64_t)1); /* Pin prior result through cancellation. */
    nt.inventory_repeat = 4095;
    nt_raw_open(FS_NTFS_RAW_BATCH_DIRS, 10000);
    MergeStop stop = {0};
    engine->sql_observer = stop_merge;
    engine->sql_observer_context = &stop;
    sqlite3_trace_v2(engine->db, SQLITE_TRACE_STMT, arm_merge_stop, &stop);
    FsNtfsInfo info;
    FsStatus status = fsx_ntfs_rebuild(native_source, &info);
    engine->sql_observer = NULL;
    engine->sql_observer_context = NULL;
    sqlite3_trace_v2(engine->db, 0, NULL, NULL);
    check_equal(status, FS_CANCELLED);
    check_true(stop.interrupted);
    check_equal(info.next_usn, before.next_usn);
    check_true(sqlite3_get_autocommit(engine->db) != 0);
    FsEntryView pinned = entry(0, NULL);
    check_equal(pinned.name, "alpha.txt");
    open_oracle();
    FsEngine *stopping = engine;
    engine = oracle_engine;
    oracle_engine = stopping;
    check_equal(nt_scalar("SELECT revision FROM fsx_meta"),
                (int64_t)before.snapshot_epoch);
    check_equal(nt_scalar("SELECT next_usn FROM fsx_ntfs_sources"),
                before.next_usn);
    check_equal(count("alpha"), (uint64_t)1);
  }
  it("builds subtree topology and all hardlink aliases with full unsigned "
     "identities") {
    nt_add(30, 10, "alias.txt", false);
    FsNtfsInfo info = nt_build();
    check_equal(info.root_frn, NT_FRN(10));
    check_equal(info.journal_id, nt.identity.journal_id);
    check_equal(count("file:"), (uint64_t)2);
    check_equal(count("alpha"), (uint64_t)1);
    check_equal(entry(0, NULL).key.file_id_lo, NT_FRN(30));
    check_equal(nt_scalar("SELECT count(*) FROM objects"), (int64_t)2);
    check_equal(nt_scalar("SELECT next_usn FROM fsx_ntfs_sources"),
                info.next_usn);
    open_oracle();
    equivalent("alpha", fsx_query_options());
  }
  it("refreshes cached enumeration metadata by full identity before "
     "publishing") {
    nt.stale_metadata = true;
    nt_build();
    check_equal(count("alpha"), (uint64_t)1);
    check_equal(entry(0, NULL).size, (uint64_t)7);
    check_equal(entry(0, NULL).mtime_ticks, (uint64_t)100);
    nt.nodes[2].size = 999;
    nt_event(&nt.nodes[2], 1);
    FsNtfsInfo info;
    check_equal(fsx_ntfs_sync(native_source, &info), FS_OK);
    check_equal(count("alpha"), (uint64_t)1);
    check_equal(entry(0, NULL).size, (uint64_t)999);
  }
  it("catches changes after initial namespace enumeration before publishing "
     "U1") {
    nt.inject_create = true;
    FsNtfsInfo info = nt_build();
    check_equal(count("catchup"), (uint64_t)1);
    check_equal(info.journal_records, (uint64_t)1);
    check_equal(info.next_usn, nt.identity.next_usn);
  }

  it("expands scope when directory enumeration discovers topology absent from "
     "MFT inventory") {
    nt.inject_directory = true;
    nt_build();
    check_equal(count("late-file"), (uint64_t)1);
    check_contains(entry(0, NULL).path, "/late-directory/late-file.txt");
  }
  it("aborts MFT handoff if the journal stops covering U0 during enumeration") {
    nt.inject_gap = true;
    nt_open(4096);
    FsNtfsInfo info;
    check_equal(fsx_ntfs_rebuild(native_source, &info), FS_STALE);
    check_true(info.needs_rebuild);
    check_equal(nt_scalar("SELECT count(*) FROM sources"), (int64_t)0);
    check_equal(nt_scalar("SELECT count(*) FROM fsx_ntfs_sources"), (int64_t)0);
    check_equal(count("alpha"), (uint64_t)0);
  }
  it("resumes a committed checkpoint after closing and reopening the session") {
    FsNtfsInfo first = nt_build();
    unsigned lists = nt.lists;
    check_equal(fsx_ntfs_close(&native_source), FS_OK);
    nt_open(4096);
    FsNtfsInfo info;
    check_equal(fsx_ntfs_sync(native_source, &info), FS_OK);
    check_equal(info.next_usn, first.next_usn);
    check_equal(nt.lists, lists);
    NtNode *node = nt_add(40, 20, "created.txt", false);
    nt_event(node, FSX_USN_CREATE);
    check_equal(fsx_ntfs_sync(native_source, &info), FS_OK);
    check_equal(count("created"), (uint64_t)1);
    check_equal(info.next_usn, nt.identity.next_usn);
  }

  it("refreshes an older mirror before acknowledging a checkpoint advanced by "
     "another engine") {
    nt_build();
    open_oracle();
    FsNtfsConfig config = fsx_ntfs_config();
    check_equal(fsx_ntfs_open_provider(oracle_engine, root, &config, &nt_ops,
                                       &nt, &other_native_source),
                FS_OK);
    NtNode *node = nt_add(40, 20, "external.txt", false);
    nt_event(node, FSX_USN_CREATE);
    FsNtfsInfo info;
    check_equal(fsx_ntfs_sync(other_native_source, &info), FS_OK);
    check_equal(count("external"), (uint64_t)0);
    check_equal(fsx_ntfs_sync(native_source, &info), FS_OK);
    check_true(info.caught_up);
    check_equal(count("external"), (uint64_t)1);
    check_equal(info.snapshot_epoch, (uint64_t)2);
  }
  it("handles split rename records and aliases without requiring an OLD NEW "
     "pair") {
    nt_build();
    check_equal(fsx_ntfs_close(&native_source), FS_OK);
    nt_open(1);
    nt_event(&nt.nodes[2], FSX_USN_OLD);
    strcpy(nt.nodes[2].name, "renamed.txt");
    nt_event(&nt.nodes[2], FSX_USN_NEW);
    FsNtfsInfo info;
    check_equal(fsx_ntfs_sync(native_source, &info), FS_OK);
    check_false(info.caught_up);
    check_equal(count("alpha"), (uint64_t)0);
    check_equal(count("renamed"), (uint64_t)1);
    check_equal(fsx_ntfs_sync(native_source, &info), FS_OK);
    check_true(info.caught_up);
    nt_add(30, 10, "hardlink.txt", false);
    nt_event(&nt.nodes[2], FSX_USN_LINK);
    check_equal(fsx_ntfs_sync(native_source, &info), FS_OK);
    check_equal(count("file:"), (uint64_t)2);
  }
  it("rewrites descendant paths on directory rename while preserving alias "
     "entry keys") {
    nt_build();
    check_equal(count("alpha"), (uint64_t)1);
    uint64_t key = entry(0, NULL).entry_key;
    nt_event(&nt.nodes[1], FSX_USN_OLD);
    strcpy(nt.nodes[1].name, "moved");
    nt_event(&nt.nodes[1], FSX_USN_NEW);
    FsNtfsInfo info;
    check_equal(fsx_ntfs_sync(native_source, &info), FS_OK);
    check_equal(count("alpha"), (uint64_t)1);
    check_equal(entry(0, NULL).entry_key, key);
    check_contains(entry(0, NULL).path, "/moved/alpha.txt");
  }
  it("drops subtrees moved outside the selected root and enumerates them on "
     "return") {
    nt_build();
    nt.nodes[1].parent = NT_FRN(90);
    nt_event(&nt.nodes[1], FSX_USN_NEW);
    FsNtfsInfo info;
    check_equal(fsx_ntfs_sync(native_source, &info), FS_OK);
    check_equal(count("alpha"), (uint64_t)0);
    nt.nodes[1].parent = NT_FRN(10);
    nt_event(&nt.nodes[1], FSX_USN_NEW);
    check_equal(fsx_ntfs_sync(native_source, &info), FS_OK);
    check_equal(count("alpha"), (uint64_t)1);
  }

  it("limits file rename and hardlink reconciliation to known and current "
     "parents") {
    nt_add(40, 10, "untouched", true);
    nt_add(50, 40, "unrelated.txt", false);
    nt_build();
    unsigned lists = nt.lists;
    nt_event(&nt.nodes[2], FSX_USN_OLD);
    strcpy(nt.nodes[2].name, "renamed.txt");
    nt_event(&nt.nodes[2], FSX_USN_NEW);
    FsNtfsInfo info;
    check_equal(fsx_ntfs_sync(native_source, &info), FS_OK);
    check_equal(nt.lists - lists, 1u);
    check_equal(count("renamed"), (uint64_t)1);
    NtNode *outside = nt_add(60, 90, "outside.txt", false);
    lists = nt.lists;
    nt_event(outside, FSX_USN_OLD);
    nt_event(outside, FSX_USN_NEW);
    check_equal(fsx_ntfs_sync(native_source, &info), FS_OK);
    check_equal(nt.lists, lists);
  }
  it("finds a newly added inside hardlink when its journal parent is outside") {
    NtNode *outside = nt_add(60, 90, "outside.txt", false);
    nt_build();
    nt_add(60, 20, "entered.txt", false);
    nt_event(outside, FSX_USN_LINK);
    FsNtfsInfo info;
    check_equal(fsx_ntfs_sync(native_source, &info), FS_OK);
    check_equal(count("entered"), (uint64_t)1);
  }
  it("retains cursor on a partial hardlink response and keeps an explicit "
     "unsupported fallback") {
    FsNtfsInfo before = nt_build();
    nt_event(&nt.nodes[2], FSX_USN_LINK);
    nt.link_status = FS_LIMIT;
    FsNtfsInfo info;
    check_equal(fsx_ntfs_sync(native_source, &info), FS_LIMIT);
    check_equal(info.next_usn, before.next_usn);
    nt.link_status = FS_UNSUPPORTED;
    check_equal(fsx_ntfs_sync(native_source, &info), FS_OK);
    check_true(info.caught_up);
    check_equal(count("alpha"), (uint64_t)1);
  }
  it("bounds distinct hardlink parents without acknowledging a partial set") {
    FsNtfsConfig config = fsx_ntfs_config();
    config.max_mft_records = 3;
    check_equal(fsx_ntfs_open_provider(engine, root, &config, &nt_ops, &nt,
                                       &native_source),
                FS_OK);
    FsNtfsInfo before, info;
    check_equal(fsx_ntfs_rebuild(native_source, &before), FS_OK);
    nt_add(30, 40, "outside-a.txt", false);
    nt_add(30, 50, "outside-b.txt", false);
    nt_add(30, 60, "outside-c.txt", false);
    nt_event(&nt.nodes[2], FSX_USN_LINK);
    check_equal(fsx_ntfs_sync(native_source, &info), FS_LIMIT);
    check_equal(nt_scalar("SELECT next_usn FROM fsx_ntfs_sources"),
                before.next_usn);
    check_equal(count("alpha"), (uint64_t)1);
    check_equal(info.snapshot_epoch, before.snapshot_epoch);
  }
  it("refreshes indexed object metadata for writes through an outside alias") {
    nt_build();
    nt.nodes[2].size = 999;
    NtNode outside = nt.nodes[2];
    outside.parent = NT_FRN(90);
    nt_event(&outside, 1);
    FsNtfsInfo info;
    check_equal(fsx_ntfs_sync(native_source, &info), FS_OK);
    check_equal(count("alpha"), (uint64_t)1);
    check_equal(entry(0, NULL).size, (uint64_t)999);
  }
  it("patches only affected native metadata pages and rolls back partial "
     "copies") {
    FsNtfsInfo before = nt_build();
    subdir("other");
    for (unsigned i = 0; i < 4100; i++) {
      char name[64];
      snprintf(name, sizeof(name), "other/item-%04u.txt", i);
      put(name, "x");
    }
    char other_root[2048];
    snprintf(other_root, sizeof(other_root), "%s/other", root);
    FsScanInfo scanned;
    check_equal(fsx_engine_scan(engine, other_root, &scanned), FS_OK);
    check_equal(engine->current->page_count, 2u);
    check_equal(count("alpha"), (uint64_t)1);
    FsEntryView old = entry(0, NULL);
    retained_result = held_result;
    held_result = NULL;
    FsSnapshot *base = engine->current;
    FsPage *untouched = base->pages[1];
    FsProgress progress, after;
    fsx_engine_progress(engine, &progress);
    uint64_t budget = engine->config.owned_memory_budget;
    /* Admit a new page and its first strings, but not its complete contents. */
    engine->config.owned_memory_budget = progress.owned_bytes + sizeof(FsPage) +
                                         FSX_PAGE_ENTRIES * sizeof(FsEntry) +
                                         4096;
    nt.nodes[2].size = 555;
    nt_event(&nt.nodes[2], 1);
    FsNtfsInfo info;
    FsStatus status = fsx_ntfs_sync(native_source, &info);
    engine->config.owned_memory_budget = budget;
    check_equal(status, FS_NOMEM);
    check_true(engine->current == base);
    fsx_engine_progress(engine, &after);
    check_equal(after.owned_bytes, progress.owned_bytes);
    check_equal(nt_scalar("SELECT next_usn FROM fsx_ntfs_sources"),
                before.next_usn);
    check_equal(old.size, (uint64_t)7);
    check_equal(fsx_ntfs_sync(native_source, &info), FS_OK);
    check_true(engine->current->pages[0] != base->pages[0]);
    check_true(engine->current->pages[1] == untouched);
    check_equal(engine->current->reused_pages, 1u);
    check_equal(count("alpha"), (uint64_t)1);
    check_equal(entry(0, NULL).size, (uint64_t)555);
    check_equal(old.size, (uint64_t)7);
    check_equal(old.name, "alpha.txt");
    check_equal(count("item-4099"), (uint64_t)1);
    check_equal(entry(0, NULL).size, (uint64_t)1);
  }
  it("falls back to namespace removal for a missing metadata-only identity") {
    nt_build();
    nt.nodes[2].live = false;
    nt_event(&nt.nodes[2], 1);
    FsNtfsInfo info;
    check_equal(fsx_ntfs_sync(native_source, &info), FS_OK);
    check_equal(count("alpha"), (uint64_t)0);
    check_equal(
        nt_scalar("SELECT count(*) FROM fsx_ntfs_names WHERE name='alpha.txt'"),
        (int64_t)0);
  }
  it("reloads external namespace changes before patching native metadata") {
    nt_build();
    open_oracle();
    FsNtfsConfig config = fsx_ntfs_config();
    check_equal(fsx_ntfs_open_provider(oracle_engine, root, &config, &nt_ops,
                                       &nt, &other_native_source),
                FS_OK);
    NtNode *node = nt_add(40, 20, "external.txt", false);
    nt_event(node, FSX_USN_CREATE);
    FsNtfsInfo info;
    check_equal(fsx_ntfs_sync(other_native_source, &info), FS_OK);
    nt.nodes[2].size = 222;
    nt_event(&nt.nodes[2], 1);
    check_equal(fsx_ntfs_sync(native_source, &info), FS_OK);
    check_equal(count("external"), (uint64_t)1);
    check_equal(count("alpha"), (uint64_t)1);
    check_equal(entry(0, NULL).size, (uint64_t)222);
  }
  it("distinguishes a reused MFT record number by its sequence bits") {
    nt_build();
    nt.nodes[2].frn = UINT64_C(0x812400000000001e);
    nt_event(&nt.nodes[2], FSX_USN_CREATE);
    FsNtfsInfo info;
    check_equal(fsx_ntfs_sync(native_source, &info), FS_OK);
    check_equal(count("alpha"), (uint64_t)1);
    check_equal(entry(0, NULL).key.file_id_lo, nt.nodes[2].frn);
  }
  it("rolls back both facts and cursor when a required directory is denied") {
    FsNtfsInfo before = nt_build();
    NtNode *node = nt_add(40, 20, "denied.txt", false);
    nt_event(node, FSX_USN_CREATE);
    nt.list_failure = FS_DENIED;
    FsNtfsInfo info;
    check_equal(fsx_ntfs_sync(native_source, &info), FS_DENIED);
    check_equal(info.next_usn, before.next_usn);
    check_equal(nt_scalar("SELECT next_usn FROM fsx_ntfs_sources"),
                before.next_usn);
    check_equal(count("denied"), (uint64_t)0);
    nt.list_failure = FS_OK;
    check_equal(fsx_ntfs_sync(native_source, &info), FS_OK);
    check_equal(count("denied"), (uint64_t)1);
  }
  it("requires rebuilding after journal wrap or reset without advancing the "
     "checkpoint") {
    FsNtfsInfo before = nt_build();
    nt.identity.first_usn = before.next_usn + 8;
    nt.identity.next_usn += 8;
    FsNtfsInfo info;
    check_equal(fsx_ntfs_sync(native_source, &info), FS_STALE);
    check_true(info.needs_rebuild);
    check_equal(info.next_usn, before.next_usn);
    nt.identity.first_usn = 0;
    nt.identity.journal_id++;
    check_equal(fsx_ntfs_rebuild(native_source, &info), FS_OK);
    check_equal(info.journal_id, nt.identity.journal_id);
  }
  it("rejects truncated and unsupported journal records atomically") {
    FsNtfsInfo before = nt_build();
    nt_event(&nt.nodes[2], 1);
    nt.read_fault = 1;
    FsNtfsInfo info;
    check_equal(fsx_ntfs_sync(native_source, &info), FS_CORRUPT);
    check_equal(nt_scalar("SELECT next_usn FROM fsx_ntfs_sources"),
                before.next_usn);
    nt.read_fault = 2;
    check_equal(fsx_ntfs_rebuild(native_source, &info), FS_OK);
    nt_event(&nt.nodes[2], 1);
    check_equal(fsx_ntfs_sync(native_source, &info), FS_UNSUPPORTED);
  }

  it("rejects a native alias binding that points into another source") {
    FsNtfsInfo before = nt_build();
    subdir("other");
    put("other/independent.txt", "x");
    char other_root[2048];
    snprintf(other_root, sizeof(other_root), "%s/other", root);
    FsScanInfo scanned;
    check_equal(fsx_engine_scan(engine, other_root, &scanned), FS_OK);
    edit_database("UPDATE fsx_ntfs_names SET entry_key=(SELECT min(entry_key)"
                  " FROM entries WHERE source_id=2) WHERE name='alpha.txt'");
    nt_event(&nt.nodes[2], 1);
    FsNtfsInfo info;
    check_equal(fsx_ntfs_sync(native_source, &info), FS_CORRUPT);
    check_equal(nt_scalar("SELECT next_usn FROM fsx_ntfs_sources"),
                before.next_usn);
    check_equal(count("independent"), (uint64_t)1);
  }
  it("retains the prior snapshot and checkpoint when SQL projection fails") {
    FsNtfsInfo before = nt_build();
    NtNode *node = nt_add(40, 20, "rollback.txt", false);
    nt_event(node, FSX_USN_CREATE);
    edit_database("CREATE TRIGGER refuse_native BEFORE INSERT ON entries BEGIN "
                  "SELECT RAISE(ABORT,'injected failure'); END;");
    FsNtfsInfo info;
    check_equal(fsx_ntfs_sync(native_source, &info), FS_CORRUPT);
    check_equal(nt_scalar("SELECT next_usn FROM fsx_ntfs_sources"),
                before.next_usn);
    check_equal(count("rollback"), (uint64_t)0);
    edit_database("DROP TRIGGER refuse_native");
    check_equal(fsx_ntfs_rebuild(native_source, &info), FS_OK);
    check_equal(count("rollback"), (uint64_t)1);
  }
  it("invalidates the native profile when a generic scan replaces the source") {
    nt_build();
    put("generic.txt", "x");
    scan();
    check_equal(nt_scalar("SELECT count(*) FROM sources"), (int64_t)1);
    check_equal(nt_scalar("SELECT valid FROM fsx_ntfs_sources"), (int64_t)0);
    check_equal(nt_scalar("SELECT count(*) FROM fsx_ntfs_names"), (int64_t)0);
    FsNtfsInfo info;
    check_equal(fsx_ntfs_sync(native_source, &info), FS_STALE);
    check_equal(count("generic"), (uint64_t)1);
    check_equal(fsx_ntfs_rebuild(native_source, &info), FS_OK);
    check_equal(count("alpha"), (uint64_t)1);
  }

  it("publishes no partial state when managed allocations exceed the owned "
     "budget") {
    FsNtfsInfo before = nt_build();
    FsProgress progress;
    fsx_engine_progress(engine, &progress);
    uint64_t budget = engine->config.owned_memory_budget;
    engine->config.owned_memory_budget = progress.owned_bytes + 1;
    NtNode *node = nt_add(40, 20, "budget.txt", false);
    nt_event(node, FSX_USN_CREATE);
    FsNtfsInfo info;
    FsStatus status = fsx_ntfs_sync(native_source, &info);
    engine->config.owned_memory_budget = budget;
    check_equal(status, FS_NOMEM);
    check_equal(info.next_usn, before.next_usn);
    check_equal(nt_scalar("SELECT next_usn FROM fsx_ntfs_sources"),
                before.next_usn);
    check_equal(count("budget"), (uint64_t)0);
    check_equal(fsx_ntfs_sync(native_source, &info), FS_OK);
    check_equal(count("budget"), (uint64_t)1);
  }
  it("retains old result metadata while FTS observes the newly committed "
     "revision") {
    nt_build();
    check_equal(count("alpha"), (uint64_t)1);
    retained_result = held_result;
    held_result = NULL;
    nt.nodes[2].size = 111;
    nt_event(&nt.nodes[2], 1);
    FsNtfsInfo info;
    check_equal(fsx_ntfs_sync(native_source, &info), FS_OK);
    check_equal(count("alpha"), (uint64_t)1);
    check_equal(entry(0, NULL).size, (uint64_t)111);
    FsHit hit;
    size_t written;
    FsEntryView previous;
    check_equal(fsx_result_page(retained_result, 0, 1, &hit, 1, &written),
                FS_OK);
    check_equal(fsx_result_entry(retained_result, hit, &previous), FS_OK);
    check_equal(previous.size, (uint64_t)7);
    open_oracle();
    equivalent("alpha", fsx_query_options());
  }
  it("handles directory name swaps without unique path collisions") {
    nt_add(40, 10, "other", true);
    nt_add(50, 40, "beta.txt", false);
    nt_build();
    strcpy(nt.nodes[1].name, "other");
    strcpy(nt.nodes[3].name, "sub");
    nt_event(&nt.nodes[1], FSX_USN_NEW);
    nt_event(&nt.nodes[3], FSX_USN_NEW);
    FsNtfsInfo info;
    check_equal(fsx_ntfs_sync(native_source, &info), FS_OK);
    check_equal(count("alpha"), (uint64_t)1);
    check_contains(entry(0, NULL).path, "/other/");
    check_equal(count("beta"), (uint64_t)1);
    check_contains(entry(0, NULL).path, "/sub/");
  }
  it("refuses MFT inventory limits without creating a partial source "
     "checkpoint") {
    FsNtfsConfig config = fsx_ntfs_config();
    config.max_mft_records = 1;
    check_equal(fsx_ntfs_open_provider(engine, root, &config, &nt_ops, &nt,
                                       &native_source),
                FS_OK);
    FsNtfsInfo info;
    check_equal(fsx_ntfs_rebuild(native_source, &info), FS_LIMIT);
    check_equal(nt_scalar("SELECT count(*) FROM fsx_ntfs_sources"), (int64_t)0);
    check_equal(nt_scalar("SELECT count(*) FROM sources"), (int64_t)0);
    check_equal(count(""), (uint64_t)0);
  }
  it("rejects malformed UTF16 names without publishing partial text coverage") {
    unsigned char raw[] = {0x00, 0xd8};
    char *text = NULL;
    check_equal(fsx_ntfs_text(engine, raw, sizeof(raw), &text), FS_UNSUPPORTED);
    check_null(text);
    unsigned char slash[] = {'a', 0, '/', 0};
    check_equal(fsx_ntfs_text(engine, slash, sizeof(slash), &text),
                FS_UNSUPPORTED);
    check_null(text);
  }
#ifdef _WIN32
  it("rejects a persistent database inside the native source root before "
     "opening the journal") {
    check_equal(fsx_engine_close(&engine, FSX_DEADLINE_INFINITE), FS_OK);
    snprintf(database, sizeof(database), "%s/inside.db", root);
    open_engine();
    FsNtfsConfig config = fsx_ntfs_config();
    check_equal(fsx_ntfs_open(engine, root, &config, &native_source),
                FS_INVALID);
    check_null(native_source);
  }
#endif

  it("leaves indexed filename facts unchanged for events outside the selected "
     "tree") {
    nt_build();
    edit_database(
        "CREATE TRIGGER reject_unrelated_update BEFORE UPDATE ON entries BEGIN"
        " SELECT RAISE(ABORT,'unchanged entry touched'); END;");
    NtNode outside = {NT_FRN(99), NT_FRN(90), 1, 0, "outside", true};
    nt_event(&outside, 1);
    FsNtfsInfo info;
    check_equal(fsx_ntfs_sync(native_source, &info), FS_OK);
    check_equal(count("alpha"), (uint64_t)1);
  }
  it("keeps empty scopes stable on outside-volume journal activity") {
    nt.nodes[2].live = false;
    nt_build();
    unsigned lists = nt.lists;
    NtNode outside = {NT_FRN(99), NT_FRN(90), 1, 0, "outside", true};
    nt_event(&outside, 1);
    FsNtfsInfo info;
    check_equal(fsx_ntfs_sync(native_source, &info), FS_OK);
    check_equal(nt.lists, lists);
  }
  it("holds an external engine handle and observes STOPPING at the next "
     "boundary") {
    nt_build();
    check_equal(fsx_engine_close(&engine, 0), FS_BUSY);
    FsNtfsInfo info;
    check_equal(fsx_ntfs_sync(native_source, &info), FS_CANCELLED);
    check_equal(fsx_ntfs_close(&native_source), FS_OK);
    check_equal(nt.closed, 1u);
  }
#ifdef _WIN32
  it("reports native volume capabilities explicitly without switching to RDC") {
    const FsNtfsOps *ops = NULL;
    void *context = NULL;
    FsNtfsConfig config = fsx_ntfs_config();
    FsOpenError diagnostic = {0};
    config.open_error = &diagnostic;

    wchar_t probe_path[2048], filesystem[32] = {0};
    wide(root, probe_path, 2048);
    HANDLE probe =
        CreateFileW(probe_path, FILE_READ_ATTRIBUTES,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    check(probe != INVALID_HANDLE_VALUE, "native capability probe");
    BOOL detected = GetVolumeInformationByHandleW(probe, NULL, 0, NULL, NULL,
                                                  NULL, filesystem, 32);
    DWORD detected_error = GetLastError();
    CloseHandle(probe);
    fprintf(stderr,
            "NTFS native filesystem probe: success=%u error=%lu name=%ls\n",
            detected, detected_error, filesystem);
    FsStatus status =
        fsx_ntfs_native_open(engine, root, &config, &ops, &context);
    if (status == FS_OK) {
      FsNtfsIdentity id;
      check_equal(ops->query(context, &id), FS_OK);
      unsigned char buffer[65536];
      size_t size = 0;
      FsStatus enumerated = ops->mft(context, 0, buffer, sizeof(buffer), &size);
      check(enumerated == FS_OK || enumerated == FS_DENIED,
            "MFT capability: %s", fsx_status_name(enumerated));
      if (enumerated == FS_OK)
        check_true(size >= 8);
      fprintf(stderr, "NTFS native capability: journal=OK MFT=%s\n",
              fsx_status_name(enumerated));
      ops->close(context);
    } else {
      fprintf(stderr, "NTFS native capability: open=%s error=%d phase=%s\n",
              fsx_status_name(status), diagnostic.native_code,
              diagnostic.diagnostic);
      check(status == FS_DENIED || status == FS_STALE ||
                (status == FS_UNSUPPORTED &&
                 (!detected || wcscmp(filesystem, L"NTFS"))),
            "native open: %s (%s)", fsx_status_name(status),
            diagnostic.diagnostic);
    }
  }
#endif
}

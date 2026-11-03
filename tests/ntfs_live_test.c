
/* Opt-in privileged acceptance: real volume enumeration, real USN, and an owned
 * namespace fixture. All filesystem mutations stay inside that fixture. */
#define _WIN32_WINNT 0x0602
#define WIN32_LEAN_AND_MEAN
#include "../src/source/ntfs/ntfs.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <tinytest.h>
#include <wchar.h>
#include <windows.h>

#include <psapi.h>

enum { GROUPS = 100, FILES_PER_GROUP = 100, REBUILDS = 3, UPDATES = 20 };
static char fixture[2048], root[2304], outside[2304], database[2304];
static bool fixture_ready;
static FsEngine *native_engine, *directory_engine, *trial_engine;
static FsNtfs *source;
static FsQuery *query;
static FsResult *result, *old_result;
static FILE *metrics;
static uint64_t frequency;
static FsNtfsInfo info;
typedef struct Provider {
  const FsNtfsOps *ops;
  void *context;
  uint64_t mft_ns, read_ns, list_ns, stat_ns, query_ns, mft_calls, read_calls,
      list_calls, stat_calls, mft_bytes;
  uint64_t announced;
} Provider;
static Provider provider;
static uint64_t now(void) {
  LARGE_INTEGER value;
  QueryPerformanceCounter(&value);
  uint64_t ticks = (uint64_t)value.QuadPart;
  return ticks / frequency * 1000000000 +
         (ticks % frequency) * 1000000000 / frequency;
}
static uint64_t cpu(void) {
  FILETIME a, b, k, u;
  check_true(GetProcessTimes(GetCurrentProcess(), &a, &b, &k, &u) != 0);
  return (((uint64_t)k.dwHighDateTime << 32) | k.dwLowDateTime) +
         (((uint64_t)u.dwHighDateTime << 32) | u.dwLowDateTime);
}
static void wide(const char *text, wchar_t *out, size_t capacity) {
  check_true(MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, out,
                                 (int)capacity) > 0);
}
static bool owned(const char *path) {
  size_t n = strlen(fixture);
  return fixture_ready && n && !strncmp(path, fixture, n) &&
         (!path[n] || path[n] == '/' || path[n] == '\\');
}
static void mkdir_path(const char *path) {
  check_true(owned(path));
  wchar_t w[4096];
  wide(path, w, 4096);
  check(CreateDirectoryW(w, NULL) != 0, "mkdir: %lu", GetLastError());
}
static void path_for(char *out, size_t cap, const char *relative) {
  snprintf(out, cap, "%s/%s", root, relative);
  check_true(owned(out));
}
static void write_path(const char *path, size_t length) {
  check_true(owned(path));
  wchar_t w[4096];
  wide(path, w, 4096);
  HANDLE file = CreateFileW(
      w, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
      NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
  check(file != INVALID_HANDLE_VALUE, "fixture write: %lu", GetLastError());
  char bytes[256];
  memset(bytes, 'x', sizeof(bytes));
  DWORD written = 0;
  BOOL ok = length <= sizeof(bytes) &&
            WriteFile(file, bytes, (DWORD)length, &written, NULL);
  DWORD error = GetLastError();
  CloseHandle(file);
  check(ok && written == length, "fixture contents: %lu", error);
}
static void put(const char *relative, size_t length) {
  char path[4096];
  path_for(path, sizeof(path), relative);
  write_path(path, length);
}
static void move_path(const char *from, const char *to) {
  check_true(owned(from) && owned(to));
  wchar_t a[4096], b[4096];
  wide(from, a, 4096);
  wide(to, b, 4096);
  check(MoveFileExW(a, b, 0) != 0, "fixture move: %lu", GetLastError());
}
static void link_path(const char *existing, const char *alias) {
  check_true(owned(existing) && owned(alias));
  wchar_t a[4096], b[4096];
  wide(existing, a, 4096);
  wide(alias, b, 4096);
  check(CreateHardLinkW(b, a, NULL) != 0, "fixture hardlink: %lu",
        GetLastError());
}
static void erase(const char *relative) {
  char path[4096];
  path_for(path, sizeof(path), relative);
  wchar_t w[4096];
  wide(path, w, 4096);
  check(DeleteFileW(w) != 0, "fixture delete: %lu", GetLastError());
}
static void cleanup(const char *path) {
  if (!owned(path))
    return;
  wchar_t w[4096], pattern[4100];
  wide(path, w, 4096);
  swprintf(pattern, 4100, L"%ls/*", w);
  WIN32_FIND_DATAW entry;
  HANDLE search = FindFirstFileW(pattern, &entry);
  if (search != INVALID_HANDLE_VALUE) {
    do {
      if (!wcscmp(entry.cFileName, L".") || !wcscmp(entry.cFileName, L".."))
        continue;
      wchar_t child[8192];
      swprintf(child, 8192, L"%ls/%ls", w, entry.cFileName);
      if ((entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
          !(entry.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
        char text[16384];
        int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, child, -1,
                                    text, sizeof(text), NULL, NULL);
        if (n > 0)
          cleanup(text);
      } else if (entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        RemoveDirectoryW(child);
      else
        DeleteFileW(child);
    } while (FindNextFileW(search, &entry));
    FindClose(search);
  }
  RemoveDirectoryW(w);
}

typedef struct SqlTiming {
  char text[224];
  uint64_t runs, ns, vm, scans;
} SqlTiming;
static SqlTiming sql_timings[64];
static char current_sql[224];
static uint64_t sql_last_report, sql_deadline;
static bool sql_profile;
static int trace_sql(unsigned event, void *context, void *statement,
                     void *duration) {
  (void)context;
  sqlite3_stmt *stmt = statement;
  const char *text = sqlite3_sql(stmt);
  if (!text)
    return 0;
  char key[224];
  snprintf(key, sizeof(key), "%s", text);
  for (char *p = key; *p; p++)
    if (*p == '\n' || *p == '\r')
      *p = ' ';
  if (event == SQLITE_TRACE_STMT) {
    strcpy(current_sql, key);
    return 0;
  }
  for (unsigned i = 0; i < 64; i++)
    if (!sql_timings[i].runs || !strcmp(sql_timings[i].text, key)) {
      SqlTiming *t = &sql_timings[i];
      strcpy(t->text, key);
      t->runs++;
      t->ns += *(sqlite3_uint64 *)duration;
      t->vm +=
          (uint32_t)sqlite3_stmt_status(stmt, SQLITE_STMTSTATUS_VM_STEP, 1);
      t->scans += (uint32_t)sqlite3_stmt_status(
          stmt, SQLITE_STMTSTATUS_FULLSCAN_STEP, 1);
      break;
    }
  return 0;
}
static int sql_progress(void *context) {
  (void)context;
  uint64_t time = now();
  if (time - sql_last_report > 2000000000) {
    fprintf(metrics, "# SQL progress: %s\n", current_sql);
    fflush(metrics);
    fprintf(stderr, "SQL progress: %s\n", current_sql);
    fflush(stderr);
    sql_last_report = time;
  }
  return sql_deadline && time >= sql_deadline;
}
static void sql_report(void) {
  if (!sql_profile)
    return;
  for (unsigned i = 0; i < 64; i++)
    if (sql_timings[i].runs)
      fprintf(metrics,
              "# SQL profile runs=%" PRIu64 " ms=%.3f vm=%" PRIu64
              " scans=%" PRIu64 " sql=%s\n",
              sql_timings[i].runs, (double)sql_timings[i].ns / 1e6,
              sql_timings[i].vm, sql_timings[i].scans, sql_timings[i].text);
  fflush(metrics);
}
static FsStatus p_query(void *ctx, FsNtfsIdentity *id) {
  Provider *p = ctx;
  uint64_t start = now();
  FsStatus s = p->ops->query(p->context, id);
  p->query_ns += now() - start;
  return s;
}
static FsStatus p_mft(void *ctx, uint64_t from, void *buffer, size_t capacity,
                      size_t *bytes) {
  Provider *p = ctx;
  uint64_t start = now();
  FsStatus s = p->ops->mft(p->context, from, buffer, capacity, bytes);
  p->mft_ns += now() - start;
  p->mft_calls++;
  if (s == FS_OK)
    p->mft_bytes += *bytes;
  if (now() - p->announced > 2000000000) {
    fprintf(stderr, "MFT progress calls=%" PRIu64 " bytes=%" PRIu64 "\n",
            p->mft_calls, p->mft_bytes);
    fflush(stderr);
    fprintf(metrics, "# MFT progress calls=%" PRIu64 " bytes=%" PRIu64 "\n",
            p->mft_calls, p->mft_bytes);
    fflush(metrics);
    p->announced = now();
  }
  return s;
}
static FsStatus p_read(void *ctx, int64_t from, uint64_t journal, void *buffer,
                       size_t capacity, size_t *bytes) {
  Provider *p = ctx;
  uint64_t start = now();
  FsStatus s = p->ops->read(p->context, from, journal, buffer, capacity, bytes);
  p->read_ns += now() - start;
  p->read_calls++;
  return s;
}
static FsStatus p_list(void *ctx, uint64_t frn, FsObserve visit, void *target) {
  Provider *p = ctx;
  uint64_t start = now();
  FsStatus s = p->ops->list(p->context, frn, visit, target);
  p->list_ns += now() - start;
  p->list_calls++;
  return s;
}
static FsStatus p_stat(void *ctx, uint64_t frn, FsObserved *out) {
  Provider *p = ctx;
  uint64_t start = now();
  FsStatus s = p->ops->stat(p->context, frn, out);
  p->stat_ns += now() - start;
  p->stat_calls++;
  return s;
}
static FsStatus p_links(void *ctx, uint64_t frn, FsNtfsParentFn visit,
                        void *target) {
  Provider *p = ctx;
  return p->ops->links(p->context, frn, visit, target);
}
static uint32_t p_error(void *ctx) {
  Provider *p = ctx;
  return p->ops->error(p->context);
}
static void p_close(void *ctx) {
  Provider *p = ctx;
  p->ops->close(p->context);
  p->context = NULL;
}
static const FsNtfsOps profiling_ops = {p_query, p_mft,   p_read,  p_list,
                                        p_stat,  p_error, p_close, p_links};

static void open_engine(FsEngine **out, const char *name, bool scanner) {
  snprintf(database, sizeof(database), "%s/%s", fixture, name);
  check_true(owned(database));
  FsEngineConfig config = fsx_engine_config(database);
  if (scanner)
    config.flags |= FS_ENGINE_SCAN_ONLY;
  FsOpenError error = {0};
  config.open_error = &error;
  FsStatus status = fsx_engine_open(&config, out);
  check(status == FS_OK, "engine: %s %d %s", fsx_status_name(status),
        error.native_code, error.diagnostic);
}
static void open_source(FsEngine *engine) {
  check_null(source);
  check_null(provider.context);
  memset(&provider, 0, sizeof(provider));
  provider.announced = now();
  if (sql_profile) {
    memset(sql_timings, 0, sizeof(sql_timings));
    current_sql[0] = 0;
    sql_last_report = now();
    const char *timeout = getenv("FSX_NTFS_PROFILE_TIMEOUT_MS");
    sql_deadline =
        timeout ? now() + (uint64_t)strtoul(timeout, NULL, 10) * 1000000 : 0;
    sqlite3_trace_v2(engine->db, SQLITE_TRACE_STMT | SQLITE_TRACE_PROFILE,
                     trace_sql, NULL);
    sqlite3_progress_handler(engine->db, 100000, sql_progress, NULL);
  }
  FsNtfsConfig config = fsx_ntfs_config();
  config.buffer_bytes = 1048576;
  config.max_batch_records = 65536;
  FsOpenError error = {0};
  config.open_error = &error;
  FsStatus status = fsx_ntfs_native_open(engine, root, &config, &provider.ops,
                                         &provider.context);
  check(status == FS_OK,
        "real-volume admission requires a readable NTFS volume: %s native=%d "
        "phase=%s",
        fsx_status_name(status), error.native_code, error.diagnostic);
  status = fsx_ntfs_open_provider(engine, root, &config, &profiling_ops,
                                  &provider, &source);
  check(status == FS_OK, "real source admission: %s", fsx_status_name(status));
}
static void close_source(void) {
  if (source)
    check_equal(fsx_ntfs_close(&source), FS_OK);
}
static void close_engine(FsEngine **engine) {
  if (*engine)
    check_equal(
        fsx_engine_close(engine, fsx_engine_now_ns(*engine) + 5000000000),
        FS_OK);
}
typedef struct Measurement {
  uint64_t wall, cpu;
  IO_COUNTERS io;
  PROCESS_MEMORY_COUNTERS memory;
} Measurement;
static Measurement begin_measurement(void) {
  Measurement m = {0};
  m.cpu = cpu();
  m.memory.cb = sizeof(m.memory);
  check_true(GetProcessIoCounters(GetCurrentProcess(), &m.io) != 0);
  check_true(GetProcessMemoryInfo(GetCurrentProcess(), &m.memory,
                                  sizeof(m.memory)) != 0);
  m.wall = now();
  return m;
}
static void record(const char *phase, unsigned sample, Measurement before,
                   FsEngine *engine, const FsNtfsInfo *native,
                   const Provider *initial) {
  uint64_t wall = now() - before.wall, used_cpu = cpu() - before.cpu;
  IO_COUNTERS io;
  PROCESS_MEMORY_COUNTERS memory = {0};
  memory.cb = sizeof(memory);
  check_true(GetProcessIoCounters(GetCurrentProcess(), &io) != 0);
  check_true(
      GetProcessMemoryInfo(GetCurrentProcess(), &memory, sizeof(memory)) != 0);
  FsProgress progress;
  check_equal(fsx_engine_progress(engine, &progress), FS_OK);
  uint64_t mft = initial ? provider.mft_ns - initial->mft_ns : 0;
  uint64_t read = initial ? provider.read_ns - initial->read_ns : 0;
  uint64_t list = initial ? provider.list_ns - initial->list_ns : 0;
  uint64_t stat = initial ? provider.stat_ns - initial->stat_ns : 0;
  fprintf(metrics,
          "%s,%u,%.6f,%.6f,%" PRIu64 ",%" PRIu64 ",%" PRIu64
          ",%zu,%zu,%zu,%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64
          ",%" PRIu64 ",%" PRIu64 ",%.6f,%.6f,%.6f,%.6f\n",
          phase, sample, (double)wall / 1e6, (double)used_cpu / 1e4,
          progress.entries, native ? native->mft_records : 0,
          native ? native->journal_records : 0,
          (size_t)before.memory.WorkingSetSize, (size_t)memory.WorkingSetSize,
          (size_t)memory.PeakWorkingSetSize, progress.owned_bytes,
          (uint64_t)(io.ReadTransferCount - before.io.ReadTransferCount),
          (uint64_t)(io.WriteTransferCount - before.io.WriteTransferCount),
          initial ? provider.mft_calls - initial->mft_calls : 0,
          initial ? provider.read_calls - initial->read_calls : 0,
          initial ? provider.list_calls - initial->list_calls : 0,
          (double)mft / 1e6, (double)read / 1e6, (double)list / 1e6,
          (double)stat / 1e6);
  sql_report();
  fflush(metrics);
  fprintf(stderr,
          "%s sample=%u wall_ms=%.3f cpu_ms=%.3f entries=%" PRIu64
          " mft=%" PRIu64 " usn=%" PRIu64 "\n",
          phase, sample, (double)wall / 1e6, (double)used_cpu / 1e4,
          progress.entries, native ? native->mft_records : 0,
          native ? native->journal_records : 0);
  fflush(stderr);
}
static void compare(FsEngine *native, FsEngine *reference) {
  const char *sql =
      "SELECT e.path,e.name,o.file_id,o.size,o.mtime,o.flags FROM entries e"
      " JOIN objects o USING(object_key) ORDER BY e.path";
  sqlite3_stmt *a = NULL, *b = NULL;
  check_equal(sqlite3_prepare_v2(native->db, sql, -1, &a, NULL), SQLITE_OK);
  check_equal(sqlite3_prepare_v2(reference->db, sql, -1, &b, NULL), SQLITE_OK);
  int x, y;
  uint64_t entries = 0;
  bool equal = true;
  char mismatch[2048] = {0};
  while ((x = sqlite3_step(a)) == SQLITE_ROW) {
    y = sqlite3_step(b);
    if (y != SQLITE_ROW) {
      equal = false;
      snprintf(mismatch, sizeof(mismatch), "reference ended early");
      break;
    }
    for (int i = 0; i < 6; i++) {
      uint32_t flags = (uint32_t)sqlite3_column_int(a, 5);
      if ((i == 3 && !(flags & FS_ENTRY_SIZE_VALID)) ||
          (i == 4 && !(flags & FS_ENTRY_MTIME_VALID)))
        continue;
      int bytes = sqlite3_column_bytes(a, i);
      bool same = i == 5
                      ? sqlite3_column_int(a, i) == sqlite3_column_int(b, i)
                      : bytes == sqlite3_column_bytes(b, i) &&
                            !memcmp(sqlite3_column_blob(a, i),
                                    sqlite3_column_blob(b, i), (size_t)bytes);
      if (!same) {
        equal = false;
        snprintf(
            mismatch, sizeof(mismatch),
            "column=%d value=%" PRIu64 "/%" PRIu64 " native=%s reference=%s", i,
            (i == 3 || i == 4) ? fsx_ntfs_get64(sqlite3_column_blob(a, i)) : 0,
            (i == 3 || i == 4) ? fsx_ntfs_get64(sqlite3_column_blob(b, i)) : 0,
            (const char *)sqlite3_column_text(a, 0),
            (const char *)sqlite3_column_text(b, 0));
        break;
      }
    }
    if (!equal)
      break;
    entries++;
  }
  if (equal && (x != SQLITE_DONE || sqlite3_step(b) != SQLITE_DONE))
    equal = false;
  sqlite3_finalize(a);
  sqlite3_finalize(b);
  check(equal,
        "full namespace/identity/metadata comparison at row %" PRIu64 ": %s",
        entries, mismatch);
  fprintf(stderr, "namespace verified: %" PRIu64 " entries\n", entries);
  fflush(stderr);
}
static void scan_reference(const char *phase, unsigned sample) {
  Measurement m = begin_measurement();
  FsScanInfo scan;
  FsStatus status = fsx_engine_scan(directory_engine, root, &scan);
  record(phase, sample, m, directory_engine, NULL, NULL);
  check(status == FS_OK, "directory scan: %s", fsx_status_name(status));
  check_true(scan.coverage_complete);
}
static void sync_native(const char *phase, unsigned sample) {
  Provider before = provider;
  Measurement m = begin_measurement();
  FsStatus status = FS_OK;
  uint64_t records = 0;
  unsigned batches = 0;
  uint64_t deadline = now() + 120000000000;
  do {
    status = fsx_ntfs_sync(source, &info);
    records += info.journal_records;
    batches++;
    if (status != FS_OK || info.caught_up)
      break;
  } while (batches < 512 && now() < deadline);
  FsNtfsInfo total = info;
  total.journal_records = records;
  record(phase, sample, m, native_engine, &total, &before);
  check(status == FS_OK, "USN sync: %s native=%u cursor=%" PRId64,
        fsx_status_name(status), info.native_error, info.next_usn);
  check_true(info.caught_up);
}
static uint64_t file_entry_key(const char *suffix) {
  sqlite3_stmt *s = NULL;
  check_equal(sqlite3_prepare_v2(
                  native_engine->db,
                  "SELECT entry_key FROM entries WHERE path LIKE '%'||?1", -1,
                  &s, NULL),
              SQLITE_OK);
  sqlite3_bind_text(s, 1, suffix, -1, SQLITE_STATIC);
  int rc = sqlite3_step(s);
  uint64_t key = rc == SQLITE_ROW ? (uint64_t)sqlite3_column_int64(s, 0) : 0;
  sqlite3_finalize(s);
  check_true(key > 0);
  return key;
}
static FsStatus execute_status(FsEngine *engine, const char *text,
                               FsResultInfo *out) {
  fsx_result_release(result);
  result = NULL;
  fsx_query_release(query);
  query = NULL;
  memset(out, 0, sizeof(*out));
  FsQueryOptions options = fsx_query_options();
  options.limit = 100;
  FsQueryRequest request = {sizeof(request), FSX_API_VERSION, text,
                            strlen(text), options};
  FsStatus status = fsx_query_submit(engine, &request, &query);
  if (status == FS_OK)
    status = fsx_query_wait(query, FSX_DEADLINE_INFINITE);
  if (status == FS_OK)
    status = fsx_query_result_acquire(query, &result);
  if (status == FS_OK)
    status = fsx_result_info(result, out);
  return status;
}
static FsResultInfo execute(FsEngine *engine, const char *text) {
  FsResultInfo out;
  check_equal(execute_status(engine, text, &out), FS_OK);
  return out;
}
static void setup(void) {
  fixture_ready = false;
  sql_profile = getenv("FSX_NTFS_SQL_PROFILE") != NULL;
  fixture[0] = 0;
  native_engine = directory_engine = trial_engine = NULL;
  source = NULL;
  query = NULL;
  result = old_result = NULL;
  metrics = NULL;
  memset(&provider, 0, sizeof(provider));
  LARGE_INTEGER f;
  check_true(QueryPerformanceFrequency(&f) != 0);
  frequency = (uint64_t)f.QuadPart;
  wchar_t cwd[1024], unique[2048];
  DWORD length = GetCurrentDirectoryW(1024, cwd);
  check_true(length > 0 && length < 1024);
  check_true(GetTempFileNameW(cwd, L"ntf", 0, unique) != 0);
  check_true(DeleteFileW(unique) != 0);
  check_true(CreateDirectoryW(unique, NULL) != 0);
  check_true(WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, unique, -1,
                                 fixture, sizeof(fixture), NULL, NULL) > 0);
  fixture_ready = true;
  snprintf(root, sizeof(root), "%s/tree", fixture);
  snprintf(outside, sizeof(outside), "%s/outside", fixture);
  mkdir_path(root);
  mkdir_path(outside);
  char report[128];
  snprintf(report, sizeof(report), "ntfs-live-%lu-%llu.csv",
           GetCurrentProcessId(), (unsigned long long)GetTickCount64());
  metrics = fopen(report, "wb");
  check_not_null(metrics);
  SYSTEM_INFO system;
  GetSystemInfo(&system);
  fprintf(metrics,
          "# build=%s compiler=%s logical_processors=%lu files=%u dirs=%u "
          "rebuilds=%u updates=%u\n",
          FSX_TEST_BUILD_TYPE, __clang_version__, system.dwNumberOfProcessors,
          GROUPS * FILES_PER_GROUP, GROUPS, REBUILDS, UPDATES);
  fputs("# OS cache uncontrolled; list_ms includes directory callbacks/SQLite; "
        "peak_ws is cumulative process peak; IO bytes are process counters, "
        "not physical disk bytes.\n",
        metrics);
  fputs("phase,sample,wall_ms,cpu_ms,entries,mft_records,journal_records,ws_"
        "before,ws_after,process_peak_ws,owned_bytes,io_read_bytes,io_write_"
        "bytes,mft_calls,read_calls,list_calls,mft_ms,read_ms,list_callback_ms,"
        "stat_ms\n",
        metrics);
  fprintf(stderr, "metrics=%s\n", report);
  fflush(stderr);
}
static void teardown(void) {
  fsx_result_release(old_result);
  old_result = NULL;
  fsx_result_release(result);
  result = NULL;
  fsx_query_release(query);
  query = NULL;
  close_source();
  close_engine(&trial_engine);
  close_engine(&native_engine);
  close_engine(&directory_engine);
  if (metrics) {
    fclose(metrics);
    metrics = NULL;
  }
  if (fixture_ready)
    cleanup(fixture);
  fixture_ready = false;
}

typedef struct RealParents {
  uint64_t values[4];
  unsigned count;
} RealParents;
static FsStatus real_parent(void *context, uint64_t parent) {
  RealParents *parents = context;
  if (parents->count == 4)
    return FS_LIMIT;
  parents->values[parents->count++] = parent;
  return FS_OK;
}
static uint64_t fixture_id(const char *path) {
  wchar_t wide_path[4096];
  wide(path, wide_path, 4096);
  HANDLE file = CreateFileW(
      wide_path, FILE_READ_ATTRIBUTES,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
      OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
      NULL);
  check(file != INVALID_HANDLE_VALUE, "open identity fixture");
  BY_HANDLE_FILE_INFORMATION value;
  BOOL ok = GetFileInformationByHandle(file, &value);
  CloseHandle(file);
  check_true(ok != 0);
  return (uint64_t)value.nFileIndexHigh << 32 | value.nFileIndexLow;
}
suite("Real NTFS volume acceptance and performance") {
  before_each() { setup(); }
  after_each() { teardown(); }

  it("enumerates all current hardlink parent identities through the admitted "
     "native handle") {
    open_engine(&native_engine, "native-0.db", false);
    open_source(native_engine);
    put("link-target.txt", 5);
    char target[4096], alias[4096];
    path_for(target, sizeof(target), "link-target.txt");
    snprintf(alias, sizeof(alias), "%s/link-alias.txt", outside);
    link_path(target, alias);
    RealParents parents = {0};
    FsStatus status = provider.ops->links(provider.context, fixture_id(target),
                                          real_parent, &parents);
    check(status == FS_OK, "native link query: %s error=%u",
          fsx_status_name(status), provider.ops->error(provider.context));
    check_equal(parents.count, 2u);
    uint64_t a = fixture_id(root), b = fixture_id(outside);
    check((parents.values[0] == a && parents.values[1] == b) ||
              (parents.values[1] == a && parents.values[0] == b),
          "full parent identity including sequence");
  }
  it("fully enumerates the real MFT and reconciles an owned subtree against "
     "directory scanning") {
    /* A denied open is a failed opt-in acceptance, never a passing fallback. */
    open_engine(&native_engine, "native-0.db", false);
    open_source(native_engine);
    close_source();
    for (unsigned group = 0; group < GROUPS; group++) {
      char path[4096], name[80];
      snprintf(name, sizeof(name), "group%03u", group);
      path_for(path, sizeof(path), name);
      mkdir_path(path);
      for (unsigned file = 0; file < FILES_PER_GROUP; file++) {
        snprintf(name, sizeof(name), "group%03u/file%03u.txt", group, file);
        put(name, 64);
      }
    }
    put("Stra\xc3\x9f"
        "e-\xe6\x96\x87\xe4\xbb\xb6.txt",
        31);
    for (unsigned sample = 0; sample < REBUILDS; sample++) {
      if (sample == 0)
        open_engine(&directory_engine, "directory-0.db", true);
      else {
        char name[64];
        snprintf(name, sizeof(name), "directory-%u.db", sample);
        open_engine(&trial_engine, name, true);
      }
      FsEngine *baseline = sample == 0 ? directory_engine : trial_engine;
      Measurement m = begin_measurement();
      FsScanInfo scanned;
      FsStatus scan_status;
      benchmark_batch("directory initial build", 1) {
        scan_status = fsx_engine_scan(baseline, root, &scanned);
      }
      record("directory_initial", sample, m, baseline, NULL, NULL);
      check_equal(scan_status, FS_OK);
      check_true(scanned.coverage_complete);
      check_equal(scanned.entries,
                  (uint64_t)GROUPS * (FILES_PER_GROUP + 1) + 1);
      if (sample)
        close_engine(&trial_engine);
      if (sample) {
        char name[64];
        snprintf(name, sizeof(name), "native-%u.db", sample);
        open_engine(&trial_engine, name, false);
      }
      FsEngine *target = sample == 0 ? native_engine : trial_engine;
      open_source(target);
      Provider before = provider;
      m = begin_measurement();
      FsStatus native_status;
      benchmark_batch("MFT initial build with USN handoff", 1) {
        native_status = fsx_ntfs_rebuild(source, &info);
      }
      record("mft_initial", sample, m, target, &info, &before);
      check(native_status == FS_OK, "MFT rebuild: %s native=%u cursor=%" PRId64,
            fsx_status_name(native_status), info.native_error, info.next_usn);
      check_true(info.caught_up);
      check_true(info.mft_records > GROUPS * FILES_PER_GROUP);
      compare(target, directory_engine);
      close_source();
      if (sample)
        close_engine(&trial_engine);
    }
    open_source(native_engine);
    uint64_t descendant = file_entry_key("/group001/file001.txt");
    FsResultInfo prior = execute(native_engine, "Stra\xc3\x9f"
                                                "e");
    check_equal(prior.total_count, (uint64_t)1);
    old_result = result;
    result = NULL;
    char a[4096], b[4096];
    put("created.txt", 17);
    path_for(a, sizeof(a), "newdir");
    mkdir_path(a);
    put("newdir/nested.txt", 9);
    path_for(a, sizeof(a), "group000/file000.txt");
    path_for(b, sizeof(b), "renamed.txt");
    move_path(a, b);
    path_for(a, sizeof(a), "group000/file001.txt");
    path_for(b, sizeof(b), "hardlink.txt");
    link_path(a, b);
    snprintf(b, sizeof(b), "%s/outside-alias.txt", outside);
    link_path(a, b);
    write_path(b, 119);
    path_for(a, sizeof(a), "group001");
    path_for(b, sizeof(b), "moved-group");
    move_path(a, b);
    path_for(a, sizeof(a), "group002");
    snprintf(b, sizeof(b), "%s/moved-out", outside);
    move_path(a, b);
    sync_native("mixed_increment", 0);
    scan_reference("directory_mixed_rescan", 0);
    compare(native_engine, directory_engine);
    check_equal(file_entry_key("/moved-group/file001.txt"), descendant);
    FsResultInfo retained;
    check_equal(fsx_result_info(old_result, &retained), FS_OK);
    check_equal(retained.snapshot_epoch, prior.snapshot_epoch);
    snprintf(a, sizeof(a), "%s/moved-out", outside);
    path_for(b, sizeof(b), "returned-group");
    move_path(a, b);
    erase("hardlink.txt");
    erase("group000/file002.txt");
    sync_native("return_delete", 0);
    scan_reference("directory_return_rescan", 0);
    compare(native_engine, directory_engine);

    fsx_result_release(old_result);
    old_result = NULL;
    fsx_result_release(result);
    result = NULL;
    fsx_query_release(query);
    query = NULL;
    close_source();
    close_engine(&native_engine);
    put("offline-created.txt", 7);
    open_engine(&native_engine, "native-0.db", false);
    open_source(native_engine);
    sync_native("reopen_resume", 0);
    scan_reference("directory_resume_rescan", 0);
    compare(native_engine, directory_engine);
    for (unsigned sample = 0; sample < UPDATES; sample++) {
      path_for(a, sizeof(a), "group000/file001.txt");
      snprintf(b, sizeof(b), "%s/outside-alias.txt", outside);
      write_path(sample % 2 ? b : a, 120 + sample);
      sync_native("single_metadata_increment", sample);
      scan_reference("directory_metadata_rescan", sample);
      compare(native_engine, directory_engine);
    }
    const char *queries[] = {"file099",
                             "Stra\xc3\x9f"
                             "e",
                             "file: size:>=128"};
    for (unsigned q = 0; q < 3; q++) {
      uint64_t expected = execute(directory_engine, queries[q]).total_count;
      for (unsigned sample = 0; sample < 20; sample++) {
        Measurement m = begin_measurement();
        FsResultInfo got;
        FsStatus query_status = execute_status(native_engine, queries[q], &got);
        record(q == 0   ? "query_auto"
               : q == 1 ? "query_unicode_auto"
                        : "query_size",
               sample, m, native_engine, NULL, NULL);
        check_equal(query_status, FS_OK);
        check_equal(got.total_count, expected);
        if (q < 2) {
          uint32_t floor = native_engine->config.fts_min_entries;
          native_engine->config.fts_min_entries = 0;
          m = begin_measurement();
          query_status = execute_status(native_engine, queries[q], &got);
          record(q == 0 ? "query_force_fts" : "query_unicode_force_fts", sample,
                 m, native_engine, NULL, NULL);
          native_engine->config.fts_min_entries = floor;
          check_equal(query_status, FS_OK);
          check_equal(got.total_count, expected);
          check_equal(got.candidate_strategy, FS_CANDIDATE_FTS5);
        }
        m = begin_measurement();
        query_status = execute_status(directory_engine, queries[q], &got);
        record(q == 0   ? "query_scan"
               : q == 1 ? "query_unicode_scan"
                        : "query_size_scan",
               sample, m, directory_engine, NULL, NULL);
        check_equal(query_status, FS_OK);
        check_equal(got.total_count, expected);
      }
    }
    fputs("# acceptance=passed\n", metrics);
    fflush(metrics);
  }
}

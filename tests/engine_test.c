#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#include <fsx/api.h>
#include <salts/thread.h>
#include <sqlite3.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <tinytest.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <wchar.h>
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
static char fixture[1024], root[1200], database[1200];
static FsEngine *engine;
static bool fixture_ready;
static FsQuery *held_query;
static FsResult *held_result, *retained_result;
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
static void put(const char *name, const char *content) {
  char path[2048];
  snprintf(path, sizeof(path), "%s/%s", root, name);
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
  held_query = NULL;
  held_result = NULL;
  retained_result = NULL;
}
static void teardown(void) {
  fsx_result_release(retained_result);
  retained_result = NULL;
  fsx_result_release(held_result);
  held_result = NULL;
  fsx_query_release(held_query);
  held_query = NULL;
  if (engine)
    check_equal(fsx_engine_close(&engine, FSX_DEADLINE_INFINITE), FS_OK);
  if (fixture_ready)
    cleanup_tree(fixture);
  fixture_ready = false;
}
static void open_engine(void) {
  FsEngineConfig config = fsx_engine_config(database);
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
static FsStatus execute(const char *text, FsQueryOptions options) {
  fsx_result_release(held_result);
  held_result = NULL;
  fsx_query_release(held_query);
  held_query = NULL;
  FsQueryRequest r = {sizeof(r), FSX_API_VERSION, text, strlen(text), options};
  FsStatus status = fsx_query_submit(engine, &r, &held_query);
  if (status == FS_OK)
    status = fsx_query_wait(held_query, FSX_DEADLINE_INFINITE);
  if (status == FS_OK)
    status = fsx_query_result_acquire(held_query, &held_result);
  return status;
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
typedef struct Sender {
  FsEngine *engine;
  atomic_uint failures;
} Sender;
static void sender_run(void *arg) {
  Sender *sender = arg;
  for (unsigned i = 0; i < 50; i++) {
    FsQueryRequest request = {sizeof(request), FSX_API_VERSION, "ext:txt", 7,
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
  }
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

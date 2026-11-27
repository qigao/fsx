#ifndef _WIN32
#define _XOPEN_SOURCE 700
#ifdef __APPLE__
#define _DARWIN_C_SOURCE
#endif
#endif
#include <clock.h>
#include <fsx/api.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <thread.h>
#include <tinytest.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <wchar.h>
#include <windows.h>
#else
#include <dirent.h>
#include <unistd.h>
#endif

static FsEngine *engine;
static FsContentQuery *query;
static FsContentBatch *batch;
static FsQuery *selection;
static FsResult *candidates;
static char root[4096];

#ifdef _WIN32
static void wide_path(const char *text, wchar_t out[4096]) {
  check_true(MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1,
                                 out, 4096) > 0);
}
#endif
static void put_bytes(const char *name, const void *data, size_t bytes) {
  char path[4600];
  snprintf(path, sizeof(path), "%s/%s", root, name);
#ifdef _WIN32
  wchar_t wide[4096];
  wide_path(path, wide);
  FILE *file = _wfopen(wide, L"wb");
#else
  FILE *file = fopen(path, "wb");
#endif
  check_not_null(file);
  check_equal(fwrite(data, 1, bytes, file), bytes);
  check_equal(fclose(file), 0);
}
static void put(const char *name, const char *text) {
  put_bytes(name, text, strlen(text));
}
static void setup(void) {
  engine = NULL;
  query = NULL;
  batch = NULL;
  selection = NULL;
  candidates = NULL;
#ifdef _WIN32
  wchar_t unique[MAX_PATH], absolute[4096];
  check_true(GetTempFileNameW(L".", L"fsn", 0, unique) != 0);
  check_true(DeleteFileW(unique));
  check_true(CreateDirectoryW(unique, NULL));
  check_true(GetFullPathNameW(unique, 4096, absolute, NULL) > 0);
  check_true(WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, absolute, -1,
                                 root, sizeof(root), NULL, NULL) > 0);
#else
  char relative[] = "./fsx-native-content-XXXXXX";
  check_not_null(mkdtemp(relative));
  check_not_null(realpath(relative, root));
#endif
  FsEngineConfig config = fsx_engine_config(":memory:");
  check_equal(fsx_engine_open(&config, &engine), FS_OK);
}
static void teardown(void) {
  fsx_content_batch_release(batch);
  fsx_content_release(query);
  fsx_result_release(candidates);
  fsx_query_release(selection);
  if (engine)
    check_equal(fsx_engine_close(&engine, UINT64_MAX), FS_OK);
#ifdef _WIN32
  wchar_t directory[4096], glob[4600];
  wide_path(root, directory);
  swprintf(glob, 4600, L"%ls\\*", directory);
  WIN32_FIND_DATAW entry;
  HANDLE listing = FindFirstFileW(glob, &entry);
  if (listing != INVALID_HANDLE_VALUE) {
    do {
      if (!wcscmp(entry.cFileName, L".") || !wcscmp(entry.cFileName, L".."))
        continue;
      wchar_t child[4600];
      swprintf(child, 4600, L"%ls\\%ls", directory, entry.cFileName);
      check_true(DeleteFileW(child));
    } while (FindNextFileW(listing, &entry));
    FindClose(listing);
  }
  check_true(RemoveDirectoryW(directory));
#else
  DIR *directory = opendir(root);
  check_not_null(directory);
  struct dirent *entry;
  while ((entry = readdir(directory))) {
    if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
      continue;
    char path[4600];
    snprintf(path, sizeof(path), "%s/%s", root, entry->d_name);
    check_equal(unlink(path), 0);
  }
  closedir(directory);
  check_equal(rmdir(root), 0);
#endif
}
static FsContentRequest request(const char *pattern) {
  FsScanInfo scan;
  check_equal(fsx_engine_scan(engine, root, &scan), FS_OK);
  return fsx_content_native_scope_request("file:", pattern);
}
static void submit(FsContentRequest *r) {
  check_equal(fsx_content_submit(engine, r, &query), FS_OK);
}
static void acquire(void) {
  check_equal(fsx_content_wait(query, cmeta_hrtime() + UINT64_C(10000000000)), FS_OK);
  check_equal(fsx_content_batch_acquire(query, &batch), FS_OK);
}
static FsContentInfo finish(void) {
  uint64_t end = cmeta_hrtime() + UINT64_C(10000000000);
  FsContentInfo info;
  for (;;) {
    fsx_content_poll(query, &info);
    if (info.finished) {
      check_equal(info.processes_started, UINT64_C(0));
      check_equal(info.running_processes, 0u);
      check_equal(info.matcher_backend, (uint32_t)FS_CONTENT_NATIVE);
      return info;
    }
    check_true(cmeta_hrtime() < end);
    cmeta_sleep_ms(1);
  }
}
static FsContentInfo collect(uint64_t *records) {
  *records = 0;
  uint64_t end = cmeta_hrtime() + UINT64_C(10000000000);
  FsContentInfo info;
  for (;;) {
    fsx_content_wait(query, end);
    FsStatus status;
    while ((status = fsx_content_batch_acquire(query, &batch)) == FS_OK) {
      FsContentFileView file;
      check_equal(fsx_content_batch_info(batch, &file), FS_OK);
      *records += file.record_count;
      fsx_content_batch_release(batch);
      batch = NULL;
    }
    fsx_content_poll(query, &info);
    if (info.finished && status == FS_END)
      break;
    check_true(status == FS_END || status == FS_WAIT);
    check_true(cmeta_hrtime() < end);
  }
  check_equal(info.processes_started, UINT64_C(0));
  check_equal(info.queued_candidates, 0u);
  check_equal(info.queued_path_bytes, 0u);
  check_equal(info.match_records, *records);
  return info;
}

suite("Embedded content matching") {
  before_each() { setup(); }
  after_each() { teardown(); }

  it("returns exact UTF8 full lines and repeated match offsets without processes") {
    put("text.txt", "zero\n猫 needle needle\n\"needle\"\t\\file\n");
    FsContentRequest r = request("needle");
    submit(&r);
    acquire();
    FsContentFileView file;
    check_equal(fsx_content_batch_info(batch, &file), FS_OK);
    check_equal(file.record_count, 3u);
    FsContentRecordView record;
    check_equal(fsx_content_batch_record(batch, 0, &record), FS_OK);
    check_equal(record.line, UINT64_C(2));
    check_equal(record.byte_offset, UINT64_C(9));
    check_equal(record.text, "猫 needle needle");
    check_equal(fsx_content_batch_record(batch, 1, &record), FS_OK);
    check_equal(record.byte_offset, UINT64_C(16));
    check_equal(fsx_content_batch_record(batch, 2, &record), FS_OK);
    check_equal(record.text, "\"needle\"\t\\file");
    check_true(finish().complete);
  }
  it("preserves a result window after its query and result handles are released") {
    put("first.txt", "needle\n");
    put("second.txt", "needle\n");
    (void)request("needle");
    FsQueryRequest search = {sizeof(search), FSX_API_VERSION, "file:", 5,
                              fsx_query_options()};
    search.options.limit = 1;
    check_equal(fsx_query_submit(engine, &search, &selection), FS_OK);
    check_equal(fsx_query_wait(selection, UINT64_MAX), FS_OK);
    check_equal(fsx_query_result_acquire(selection, &candidates), FS_OK);
    FsContentRequest r = fsx_content_native_request(candidates, "needle");
    submit(&r);
    fsx_result_release(candidates);
    candidates = NULL;
    fsx_query_release(selection);
    selection = NULL;
    acquire();
    FsContentInfo info = finish();
    check_true(info.complete);
    check_equal(info.candidate_count, UINT64_C(1));
    check_equal(info.match_records, UINT64_C(1));
  }
  it("keeps literal metacharacters literal and supports lookbehind across lines") {
    put("text.txt", "aXb\nprefixone\ntwo\n");
    FsContentRequest r = request("a.b");
    submit(&r);
    check_true(finish().complete);
    fsx_content_release(query);
    query = NULL;
    r = fsx_content_native_scope_request("file:", "(?<=prefix)one\\ntwo");
    r.mode = FS_CONTENT_PCRE2;
    submit(&r);
    acquire();
    FsContentRecordView record;
    check_equal(fsx_content_batch_record(batch, 0, &record), FS_OK);
    check_equal(record.line, UINT64_C(2));
    check_equal(record.byte_offset, UINT64_C(10));
    check_equal(record.text, "prefixone\ntwo");
    check_true(finish().complete);
  }
  it("matches a literal crossing a search window and a UTF8 read boundary") {
    size_t size = 131080;
    char *text = malloc(size);
    check_not_null(text);
    memset(text, 'x', size);
    text[0] = '\n';
    text[65520] = '\n';
    memcpy(text + 65535, "猫needle", 9);
    text[65550] = '\n';
    text[size - 1] = '\n';
    put_bytes("boundary.txt", text, size);
    free(text);
    FsContentRequest r = request("猫needle");
    submit(&r);
    acquire();
    FsContentRecordView record;
    check_equal(fsx_content_batch_record(batch, 0, &record), FS_OK);
    check_equal(record.byte_offset, UINT64_C(65535));
    check_equal(record.line, UINT64_C(3));
    check_equal(record.text_bytes, (size_t)29);
    FsContentInfo info = finish();
    check_equal(info.match_records, UINT64_C(1));
    check_true(info.complete);
  }
  it("handles overlapping literal candidates with nonoverlapping match results") {
    put("text.txt", "aaaaa\n");
    FsContentRequest r = request("aaa");
    submit(&r);
    acquire();
    check_equal(finish().match_records, UINT64_C(1));
  }
  it("matches empty literals once per real line including empty lines") {
    put("text.txt", "text\n\nx\n");
    FsContentRequest r = request("");
    submit(&r);
    acquire();
    FsContentRecordView record;
    check_equal(fsx_content_batch_record(batch, 1, &record), FS_OK);
    check_equal(record.line, UINT64_C(2));
    check_equal(record.byte_offset, UINT64_C(5));
    check_equal(record.text, "");
    check_equal(finish().match_records, UINT64_C(3));
  }
  it("advances zero-width regex matches without creating an EOF line") {
    put("text.txt", "text\n\nx\n");
    FsContentRequest r = request("^|$");
    r.mode = FS_CONTENT_PCRE2;
    submit(&r);
    acquire();
    uint64_t offsets[] = {0, 4, 5, 6, 7};
    for (uint32_t i = 0; i < 5; i++) {
      FsContentRecordView record;
      check_equal(fsx_content_batch_record(batch, i, &record), FS_OK);
      check_equal(record.byte_offset, offsets[i]);
    }
    check_equal(finish().match_records, UINT64_C(5));
  }
  it("strips BOM and rejects binary or incomplete UTF8 before any publication") {
    put("bom.txt", "\xef\xbb\xbfneedle\n");
    put_bytes("binary.txt", "needle\0", 7);
    put_bytes("invalid.txt", "needle\xe7\x8c", 8);
    FsContentRequest r = request("needle");
    submit(&r);
    acquire();
    FsContentRecordView record;
    check_equal(fsx_content_batch_record(batch, 0, &record), FS_OK);
    check_equal(record.byte_offset, UINT64_C(0));
    fsx_content_batch_release(batch);
    batch = NULL;
    FsContentInfo info = finish();
    check_equal(info.status, FS_OK);
    check_equal(info.skipped_files, UINT64_C(2));
    check_false(info.complete);
  }
  it("rejects malformed regex even when the pinned scope is empty") {
    FsContentRequest r = request("[");
    r.mode = FS_CONTENT_PCRE2;
    submit(&r);
    FsContentInfo info = finish();
    check_true(info.status != FS_OK);
    check_false(info.complete);
    check_false(info.candidate_count_known);
    check_true(info.diagnostic[0] != 0);
  }
  it("never publishes a file that exceeds its record or text budget") {
    put("text.txt", "needle needle\n");
    FsContentRequest r = request("needle");
    r.max_records = 1;
    submit(&r);
    check_equal(finish().status, FS_LIMIT);
    check_equal(fsx_content_batch_acquire(query, &batch), FS_END);
    fsx_content_release(query);
    query = NULL;
    char text[1100];
    memset(text, 'x', sizeof(text));
    memcpy(text, "needle", 6);
    put_bytes("text.txt", text, sizeof(text));
    r = fsx_content_native_scope_request("file:", "needle");
    r.max_output_bytes = 1024;
    submit(&r);
    check_equal(finish().status, FS_LIMIT);
    check_equal(fsx_content_batch_acquire(query, &batch), FS_END);
  }
  it("enforces the file limit and an already expired execution deadline") {
    put("text.txt", "needle\n");
    FsContentRequest r = request("needle");
    r.max_file_bytes = 3;
    submit(&r);
    check_equal(finish().status, FS_LIMIT);
    fsx_content_release(query);
    query = NULL;
    r = fsx_content_native_scope_request("file:", "needle");
    r.deadline_ns = 0;
    submit(&r);
    check_equal(finish().status, FS_TIMEOUT);
  }
  it("limits pathological PCRE2 work instead of returning a false zero-match") {
    char text[2001];
    memset(text, 'a', sizeof(text));
    text[2000] = '!';
    put_bytes("text.txt", text, sizeof(text));
    FsContentRequest r = request("^(a+)+$");
    r.mode = FS_CONTENT_PCRE2;
    submit(&r);
    FsContentInfo info = finish();
    check_equal(info.status, FS_LIMIT);
    check_false(info.complete);
    check_equal(info.match_records, UINT64_C(0));
  }
  it("shares a compiled pattern across two file workers with exact totals") {
    for (unsigned i = 0; i < 24; i++) {
      char name[64];
      snprintf(name, sizeof(name), "file-%u.txt", i);
      put(name, "needle needle\n");
    }
    FsContentRequest r = request("needle");
    r.parallel_files = 2;
    submit(&r);
    uint64_t records;
    FsContentInfo info = collect(&records);
    check_true(info.complete);
    check_equal(records, UINT64_C(48));
    check_equal(info.files_scanned, UINT64_C(24));
  }
  it("enforces the task-wide record budget across two publishing workers") {
    put("a.txt", "needle needle\n");
    put("b.txt", "needle needle\n");
    FsContentRequest r = request("needle");
    r.parallel_files = 2;
    r.max_records = 3;
    submit(&r);
    uint64_t records;
    FsContentInfo info = collect(&records);
    check_equal(info.status, FS_LIMIT);
    check_false(info.complete);
    check_equal(records, UINT64_C(2));
    check_equal(info.files_scanned, UINT64_C(1));
  }
  it("retains acquired native text while cancellation releases blocked work") {
    put("a.txt", "needle\n");
    put("b.txt", "needle\n");
    FsContentRequest r = request("needle");
    submit(&r);
    acquire();
    check_equal(fsx_engine_close(&engine, 0), FS_BUSY);
    check_equal(finish().status, FS_CANCELLED);
    fsx_content_release(query);
    query = NULL;
    FsContentRecordView record;
    check_equal(fsx_content_batch_record(batch, 0, &record), FS_OK);
    check_equal(record.text, "needle");
    check_equal(fsx_engine_close(&engine, 0), FS_BUSY);
  }
  it("rejects ambiguous native tool requests without falling back") {
    FsContentRequest r = request("needle");
    r.ugrep_executable = "/missing-tool";
    check_equal(fsx_content_submit(engine, &r, &query), FS_INVALID);
    check_null(query);
    r.ugrep_executable = NULL;
    r.discovery_backend = FS_DISCOVERY_BFTW;
    check_equal(fsx_content_submit(engine, &r, &query), FS_INVALID);
    check_null(query);
  }
}

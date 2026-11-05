#ifdef __APPLE__
#define _DARWIN_C_SOURCE
#endif
#define _XOPEN_SOURCE 700
#include <clock.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fsx/api.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <thread.h>
#include <tinytest.h>
#include <unistd.h>
static FsEngine *engine;
static FsQuery *selection;
static FsResult *candidates;
static FsContentQuery *query;
static FsContentBatch *batch, *held_batch;
static char root[1024];
static char live_root[1200];
static char gate[1400], gate_pattern[1600];
static void put_bytes(const char *name, const void *data, size_t n) {
  char path[1200];
  snprintf(path, sizeof(path), "%s/%s", root, name);
  FILE *file = fopen(path, "wb");
  check_not_null(file);
  check_equal(fwrite(data, 1, n, file), n);
  check_equal(fclose(file), 0);
}
static void put(const char *name, const char *text) {
  put_bytes(name, text, strlen(text));
}
static void setup(void) {
  strcpy(root, "./fsx-content-XXXXXX");
  check_not_null(mkdtemp(root));
  gate[0] = 0;
  engine = NULL;
  selection = NULL;
  candidates = NULL;
  query = NULL;
  batch = held_batch = NULL;
  FsEngineConfig c = fsx_engine_config(":memory:");
  check_equal(fsx_engine_open(&c, &engine), FS_OK);
}
static void remove_tree(const char *path) {
  DIR *dir = opendir(path);
  check_not_null(dir);
  struct dirent *entry;
  while ((entry = readdir(dir))) {
    if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
      continue;
    char child[4096];
    snprintf(child, sizeof(child), "%s/%s", path, entry->d_name);
    struct stat info;
    check_equal(lstat(child, &info), 0);
    if (S_ISDIR(info.st_mode))
      remove_tree(child);
    else
      check_equal(unlink(child), 0);
  }
  closedir(dir);
  check_equal(rmdir(path), 0);
}
static void teardown(void) {
  fsx_content_batch_release(held_batch);
  held_batch = NULL;
  fsx_content_batch_release(batch);
  batch = NULL;
  fsx_content_release(query);
  query = NULL;
  fsx_result_release(candidates);
  candidates = NULL;
  fsx_query_release(selection);
  selection = NULL;
  if (engine)
    check_equal(fsx_engine_close(&engine, UINT64_MAX), FS_OK);
  remove_tree(root);
  if (gate[0])
    remove_tree(gate);
}
static FsContentRequest live_request(const char *pattern) {
  char *absolute = realpath(root, NULL);
  check_not_null(absolute);
  check_true(strlen(absolute) < sizeof(live_root));
  strcpy(live_root, absolute);
  free(absolute);
  return fsx_content_live_request(live_root, FSX_TEST_BFS, FSX_TEST_UGREP,
                                  pattern);
}
static FsContentRequest request(const char *pattern) {
  FsScanInfo scanned;
  check_equal(fsx_engine_scan(engine, root, &scanned), FS_OK);
  FsQueryRequest r = {sizeof(r), FSX_API_VERSION, "file:", 5,
                      fsx_query_options()};
  check_equal(fsx_query_submit(engine, &r, &selection), FS_OK);
  check_equal(fsx_query_wait(selection, UINT64_MAX), FS_OK);
  check_equal(fsx_query_result_acquire(selection, &candidates), FS_OK);
  return fsx_content_request(candidates, FSX_TEST_UGREP, pattern);
}
static FsContentInfo finish(void) {
  FsContentInfo info;
  uint64_t end = cmeta_hrtime() + UINT64_C(5000000000);
  for (;;) {
    fsx_content_poll(query, &info);
    if (info.finished)
      return info;
    check_true(cmeta_hrtime() < end);
    cmeta_sleep_ms(1);
  }
}
static void acquire(void) {
  check_equal(fsx_content_wait(query, cmeta_hrtime() + UINT64_C(5000000000)),
              FS_OK);
  check_equal(fsx_content_batch_acquire(query, &batch), FS_OK);
}
static void gate_request(FsContentRequest *r, bool fail) {
  char relative[1200];
  snprintf(relative, sizeof(relative), "%s-gate", root);
  check_equal(mkdir(relative, 0700), 0);
  char *absolute = realpath(relative, NULL);
  check_not_null(absolute);
  snprintf(gate, sizeof(gate), "%s", absolute);
  free(absolute);
  snprintf(gate_pattern, sizeof(gate_pattern), "%s%s",
           fail ? "parallel-fail:" : "parallel:", gate);
  r->ugrep_executable = FSX_TEST_CONTENT_PEER;
  r->pattern = gate_pattern;
  r->parallel_files = 2;
  r->max_output_bytes = 1024;
}
static FsContentInfo two_ready(bool full_queue) {
  FsContentInfo info;
  uint64_t end = cmeta_hrtime() + UINT64_C(5000000000);
  for (;;) {
    fsx_content_poll(query, &info);
    check_false(info.finished);
    check_true(info.running_processes <= 3);
    check_true(info.queued_candidates <= 128);
    check_true(info.queued_path_bytes <= 1024 * 1024);
    DIR *dir = opendir(gate);
    check_not_null(dir);
    unsigned ready = 0;
    struct dirent *item;
    while ((item = readdir(dir)))
      if (strstr(item->d_name, ".ready"))
        ready++;
    closedir(dir);
    if (ready == 2 && (!full_queue || info.queued_candidates == 128))
      return info;
    check_true(cmeta_hrtime() < end);
    cmeta_sleep_ms(1);
  }
}
static void release_gate(void) {
  char path[1600];
  snprintf(path, sizeof(path), "%s/go", gate);
  FILE *file = fopen(path, "wb");
  check_not_null(file);
  check_equal(fclose(file), 0);
}
static FsContentInfo collect_all(uint64_t *records) {
  uint64_t end = cmeta_hrtime() + UINT64_C(10000000000);
  *records = 0;
  FsContentInfo info;
  for (;;) {
    fsx_content_wait(query, end);
    FsStatus acquired;
    while ((acquired = fsx_content_batch_acquire(query, &batch)) == FS_OK) {
      FsContentFileView file;
      check_equal(fsx_content_batch_info(batch, &file), FS_OK);
      check_true(file.read_end_ns >= file.read_start_ns);
      *records += file.record_count;
      fsx_content_batch_release(batch);
      batch = NULL;
    }
    check_true(acquired == FS_END || acquired == FS_WAIT);
    fsx_content_poll(query, &info);
    if (info.finished && acquired == FS_END)
      break;
    check_true(cmeta_hrtime() < end);
  }
  check_equal(info.running_processes, 0u);
  check_equal(info.queued_candidates, 0u);
  check_equal(info.queued_path_bytes, 0u);
  check_equal(info.match_records, *records);
  return info;
}
suite("Asynchronous native ugrep content") {
  before_each() { setup(); }
  after_each() { teardown(); }
  it("retains a selected snapshot and decodes exact UTF8 records") {
    put("text.txt", "zero\n猫 needle needle\n\"needle\"\t\\file\n");
    FsContentRequest r = request("needle");
    check_equal(fsx_content_submit(engine, &r, &query), FS_OK);
    fsx_result_release(candidates);
    candidates = NULL;
    fsx_query_release(selection);
    selection = NULL;
    acquire();
    FsContentFileView file;
    check_equal(fsx_content_batch_info(batch, &file), FS_OK);
    check_equal(file.record_count, 3u);
    check_true(file.read_end_ns >= file.read_start_ns);
    FsContentRecordView record;
    check_equal(fsx_content_batch_record(batch, 0, &record), FS_OK);
    check_equal(record.line, (uint64_t)2);
    check_equal(record.byte_offset, (uint64_t)9);
    check_equal(record.text, "猫 needle needle");
    check_equal(fsx_content_batch_record(batch, 1, &record), FS_OK);
    check_equal(record.byte_offset, (uint64_t)16);
    check_equal(fsx_content_batch_record(batch, 2, &record), FS_OK);
    check_equal(record.text, "\"needle\"\t\\file");
    FsContentInfo info = finish();
    check_true(info.complete);
    check_equal(info.match_records, (uint64_t)3);
    check_equal(info.running_processes, 0u);
  }
  it("uses explicit PCRE2 including lookbehind and multiline JSON") {
    put("text.txt", "prefixone\ntwo\n");
    FsContentRequest r = request("(?<=prefix)one\\ntwo");
    r.mode = FS_CONTENT_PCRE2;
    check_equal(fsx_content_submit(engine, &r, &query), FS_OK);
    acquire();
    FsContentRecordView record;
    check_equal(fsx_content_batch_record(batch, 0, &record), FS_OK);
    check_equal(record.text, "prefixone\ntwo");
    check_equal(record.byte_offset, (uint64_t)6);
    check_true(finish().complete);
  }
  it("reports complete no-match and leaves metadata indexing available") {
    put("text.txt", "aXb\n");
    FsContentRequest r = request("a.b");
    check_equal(fsx_content_submit(engine, &r, &query), FS_OK);
    FsContentInfo info = finish();
    check_equal(info.status, FS_OK);
    check_true(info.complete);
    check_equal(info.files_scanned, (uint64_t)1);
    check_equal(info.match_records, (uint64_t)0);
    check_equal(fsx_content_batch_acquire(query, &batch), FS_END);
    FsScanInfo scanned;
    check_equal(fsx_engine_scan(engine, root, &scanned), FS_OK);
  }
  it("strips UTF8 BOM and makes invalid text coverage explicit") {
    put("bom.txt", "\xef\xbb\xbfneedle\n");
    put_bytes("nul.txt", "needle\0", 7);
    put_bytes("invalid.txt", "\xff", 1);
    FsContentRequest r = request("needle");
    check_equal(fsx_content_submit(engine, &r, &query), FS_OK);
    acquire();
    FsContentRecordView record;
    fsx_content_batch_record(batch, 0, &record);
    check_equal(record.byte_offset, (uint64_t)0);
    fsx_content_batch_release(batch);
    batch = NULL;
    FsContentInfo info = finish();
    check_equal(info.status, FS_OK);
    check_false(info.complete);
    check_equal(info.skipped_files, (uint64_t)2);
  }
  it("rejects a replaced candidate identity without publishing its contents") {
    put("text.txt", "old");
    FsContentRequest r = request("needle");
    char old[1200], parked[1200];
    snprintf(old, sizeof(old), "%s/text.txt", root);
    snprintf(parked, sizeof(parked), "%s/parked", root);
    check_equal(rename(old, parked), 0);
    put("text.txt", "needle\n");
    check_equal(fsx_content_submit(engine, &r, &query), FS_OK);
    FsContentInfo info = finish();
    check_equal(info.status, FS_OK);
    check_false(info.complete);
    check_equal(info.changed_files, (uint64_t)1);
    check_equal(info.match_records, (uint64_t)0);
  }
  it("applies file batch backpressure and preserves an acquired batch on "
     "STOPPING") {
    put("a.txt", "needle\n");
    put("b.txt", "needle\n");
    FsContentRequest r = request("needle");
    check_equal(fsx_content_submit(engine, &r, &query), FS_OK);
    acquire();
    cmeta_sleep_ms(30);
    FsContentInfo info;
    check_equal(fsx_content_poll(query, &info), FS_WAIT);
    check_equal(info.files_scanned, (uint64_t)1);
    FsContentQuery *rejected = NULL;
    check_equal(fsx_content_submit(engine, &r, &rejected), FS_BUSY);
    check_null(rejected);
    check_equal(fsx_engine_close(&engine, 0), FS_BUSY);
    info = finish();
    check_equal(info.status, FS_CANCELLED);
    fsx_content_release(query);
    query = NULL;
    FsContentRecordView record;
    fsx_content_batch_record(batch, 0, &record);
    check_equal(record.text, "needle");
    check_equal(fsx_engine_close(&engine, 0), FS_BUSY);
  }
  it("reaps a running native process on cancellation") {
    char block[65536];
    memset(block, 'x', sizeof(block));
    char path[1200];
    snprintf(path, sizeof(path), "%s/large.txt", root);
    FILE *file = fopen(path, "wb");
    check_not_null(file);
    for (unsigned i = 0; i < 256; i++)
      check_equal(fwrite(block, 1, sizeof(block), file), sizeof(block));
    check_equal(fclose(file), 0);
    FsContentRequest r = request("missing");
    check_equal(fsx_content_submit(engine, &r, &query), FS_OK);
    uint64_t end = cmeta_hrtime() + UINT64_C(5000000000);
    FsContentInfo info;
    do {
      fsx_content_poll(query, &info);
      check_false(info.finished);
      check_true(cmeta_hrtime() < end);
    } while (!info.running_processes);
    fsx_content_cancel(query);
    info = finish();
    check_equal(info.status, FS_CANCELLED);
    check_equal(info.running_processes, 0u);
    check_equal(info.processes_started, (uint64_t)1);
  }
  it("bounds records and never publishes an incomplete file batch") {
    put("text.txt", "needle needle\n");
    FsContentRequest r = request("needle");
    r.max_records = 1;
    check_equal(fsx_content_submit(engine, &r, &query), FS_OK);
    FsContentInfo info = finish();
    check_equal(info.status, FS_LIMIT);
    check_false(info.complete);
    check_equal(fsx_content_batch_acquire(query, &batch), FS_END);
  }
  it("feeds and drains pipes beyond kernel buffers without deadlock") {
    char line[136];
    memset(line, 'x', sizeof(line));
    memcpy(line, "needle ", 7);
    line[sizeof(line) - 1] = '\n';
    char path[1200];
    snprintf(path, sizeof(path), "%s/matches.txt", root);
    FILE *file = fopen(path, "wb");
    check_not_null(file);
    for (unsigned i = 0; i < 2000; i++)
      check_equal(fwrite(line, 1, sizeof(line), file), sizeof(line));
    fclose(file);
    FsContentRequest r = request("needle");
    check_equal(fsx_content_submit(engine, &r, &query), FS_OK);
    acquire();
    FsContentFileView view;
    fsx_content_batch_info(batch, &view);
    check_equal(view.record_count, 2000u);
    FsContentRecordView record;
    fsx_content_batch_record(batch, 1999, &record);
    check_equal(record.line, (uint64_t)2000);
    check_equal(record.byte_offset, (uint64_t)1999 * sizeof(line));
    check_true(finish().complete);
  }
  it("validates UTF8 scalars across input buffers and bounded JSON frames") {
    char path[1200];
    snprintf(path, sizeof(path), "%s/boundary.txt", root);
    FILE *file = fopen(path, "wb");
    check_not_null(file);
    char block[65535];
    memset(block, 'x', sizeof(block));
    check_equal(fwrite(block, 1, sizeof(block), file), sizeof(block));
    const char tail[] = "猫\nneedle\x01\n";
    check_equal(fwrite(tail, 1, sizeof(tail) - 1, file), sizeof(tail) - 1);
    fclose(file);
    FsContentRequest r = request("needle");
    check_equal(fsx_content_submit(engine, &r, &query), FS_OK);
    acquire();
    FsContentRecordView record;
    fsx_content_batch_record(batch, 0, &record);
    check_equal(record.byte_offset, (uint64_t)65539);
    check_equal(record.text, "needle\x01");
    check_true(finish().complete);
  }
  it("reports missing tools and tool regex errors without a fallback") {
    put("text.txt", "needle\n");
    FsContentRequest r = request("[");
    r.mode = FS_CONTENT_PCRE2;
    check_equal(fsx_content_submit(engine, &r, &query), FS_OK);
    FsContentInfo info = finish();
    check_equal(info.status, FS_IO);
    check_true(info.tool_exit_code > 1);
    check_true(info.diagnostic[0] != 0);
    fsx_content_release(query);
    query = NULL;
    r.pattern = "needle";
    r.ugrep_executable = "/fsx-missing-native-ugrep";
    check_equal(fsx_content_submit(engine, &r, &query), FS_OK);
    check_equal(finish().status, FS_IO);
  }
  it("enforces raw output and file limits before publishing a batch") {
    char text[2048];
    memset(text, 'x', sizeof(text));
    memcpy(text, "needle", 6);
    put_bytes("text.txt", text, sizeof(text));
    FsContentRequest r = request("needle");
    r.max_output_bytes = 1024;
    check_equal(fsx_content_submit(engine, &r, &query), FS_OK);
    check_equal(finish().status, FS_LIMIT);
    check_equal(fsx_content_batch_acquire(query, &batch), FS_END);
    fsx_content_release(query);
    query = NULL;
    r.max_output_bytes = 8192;
    r.max_file_bytes = 1024;
    check_equal(fsx_content_submit(engine, &r, &query), FS_OK);
    FsContentInfo info = finish();
    check_equal(info.status, FS_LIMIT);
    check_equal(info.processes_started, (uint64_t)0);
  }
  it("searches exactly a caller-selected window rather than its total result") {
    put("a.txt", "needle\n");
    put("b.txt", "needle\n");
    (void)request("needle");
    fsx_result_release(candidates);
    candidates = NULL;
    fsx_query_release(selection);
    selection = NULL;
    FsQueryOptions options = fsx_query_options();
    options.offset = 1;
    options.limit = 1;
    FsQueryRequest select = {sizeof(select), FSX_API_VERSION, "file:", 5,
                             options};
    check_equal(fsx_query_submit(engine, &select, &selection), FS_OK);
    check_equal(fsx_query_wait(selection, UINT64_MAX), FS_OK);
    check_equal(fsx_query_result_acquire(selection, &candidates), FS_OK);
    FsResultInfo source;
    fsx_result_info(candidates, &source);
    check_equal(source.total_count, (uint64_t)2);
    FsContentRequest r =
        fsx_content_request(candidates, FSX_TEST_UGREP, "needle");
    check_equal(fsx_content_submit(engine, &r, &query), FS_OK);
    acquire();
    FsContentFileView file;
    fsx_content_batch_info(batch, &file);
    check_not_null(strstr(file.path, "/b.txt"));
    FsContentInfo info = finish();
    check_equal(info.candidate_count, (uint64_t)1);
    check_equal(info.match_records, (uint64_t)1);
    check_true(info.complete);
  }
  it("rejects invalid configuration without admitting a task") {
    put("text.txt", "needle\n");
    FsContentRequest r = request("needle");
    r.max_records = 0;
    check_equal(fsx_content_submit(engine, &r, &query), FS_INVALID);
    check_null(query);
    r.max_records = 100;
    r.ugrep_executable = "ugrep";
    check_equal(fsx_content_submit(engine, &r, &query), FS_INVALID);
    check_null(query);
    r.ugrep_executable = FSX_TEST_UGREP;
    r.pattern = "one\ntwo";
    check_equal(fsx_content_submit(engine, &r, &query), FS_INVALID);
    check_null(query);
  }
  it("honors execution deadlines independently of wait deadlines") {
    put("text.txt", "needle\n");
    FsContentRequest r = request("needle");
    r.deadline_ns = cmeta_hrtime();
    check_equal(fsx_content_submit(engine, &r, &query), FS_OK);
    FsContentInfo info = finish();
    check_equal(info.status, FS_TIMEOUT);
    check_false(info.complete);
    check_equal(info.processes_started, (uint64_t)0);
  }
  it("streams a complete scope beyond the engine query window") {
    check_equal(fsx_engine_close(&engine, UINT64_MAX), FS_OK);
    FsEngineConfig config = fsx_engine_config(":memory:");
    config.max_query_window = 1;
    check_equal(fsx_engine_open(&config, &engine), FS_OK);
    for (unsigned i = 0; i < 12; i++) {
      char name[40];
      snprintf(name, sizeof(name), "item%u.txt", i);
      put(name, "needle\n");
    }
    put("excluded.bin", "needle\n");
    put("empty.txt", "");
    FsScanInfo scan;
    check_equal(fsx_engine_scan(engine, root, &scan), FS_OK);
    char scope[] = "file: ext:txt size:>0 regex:\"^item[0-9]+[.]txt$\"";
    FsContentRequest r =
        fsx_content_scope_request(scope, FSX_TEST_UGREP, "needle");
    check_equal(fsx_content_submit(engine, &r, &query), FS_OK);
    memset(scope, 'x', strlen(scope));
    for (unsigned i = 0; i < 12; i++) {
      acquire();
      FsContentFileView file;
      check_equal(fsx_content_batch_info(batch, &file), FS_OK);
      check_equal(file.metadata_epoch, scan.snapshot_epoch);
      check_true(strstr(file.path, ".txt") != NULL);
      fsx_content_batch_release(batch);
      batch = NULL;
    }
    FsContentInfo info = finish();
    check_equal(info.status, FS_OK);
    check_true(info.complete);
    check_true(info.candidate_count_known);
    check_equal(info.candidate_count, (uint64_t)12);
    check_equal(info.files_scanned, (uint64_t)12);
    check_equal(info.match_records, (uint64_t)12);
  }
  it("pins a full scope snapshot while a new metadata revision commits") {
    put("a.txt", "needle\n");
    put("b.txt", "needle\n");
    FsScanInfo before, after;
    check_equal(fsx_engine_scan(engine, root, &before), FS_OK);
    FsContentRequest r =
        fsx_content_scope_request("file:", FSX_TEST_UGREP, "needle");
    check_equal(fsx_content_submit(engine, &r, &query), FS_OK);
    acquire();
    FsContentInfo info;
    check_equal(fsx_content_poll(query, &info), FS_WAIT);
    check_false(info.candidate_count_known);
    check_equal(info.candidate_count, (uint64_t)1);
    put("new.txt", "needle\n");
    check_equal(fsx_engine_scan(engine, root, &after), FS_OK);
    check_true(after.snapshot_epoch > before.snapshot_epoch);
    for (unsigned i = 0; i < 2; i++) {
      FsContentFileView file;
      check_equal(fsx_content_batch_info(batch, &file), FS_OK);
      check_equal(file.metadata_epoch, before.snapshot_epoch);
      check_null(strstr(file.path, "new.txt"));
      fsx_content_batch_release(batch);
      batch = NULL;
      if (!i)
        acquire();
    }
    info = finish();
    check_true(info.complete);
    check_true(info.candidate_count_known);
    check_equal(info.candidate_count, (uint64_t)2);
  }
  it("supports explicit case-sensitive scope predicates and an empty scope") {
    put("CASE.txt", "needle\n");
    FsScanInfo scan;
    check_equal(fsx_engine_scan(engine, root, &scan), FS_OK);
    FsContentRequest r =
        fsx_content_scope_request("case.txt", FSX_TEST_UGREP, "needle");
    r.scope_flags = FS_QUERY_CASE_SENSITIVE;
    check_equal(fsx_content_submit(engine, &r, &query), FS_OK);
    FsContentInfo info = finish();
    check_true(info.complete);
    check_true(info.candidate_count_known);
    check_equal(info.candidate_count, (uint64_t)0);
    check_equal(info.processes_started, (uint64_t)0);
    fsx_content_release(query);
    query = NULL;
    r = fsx_content_scope_request("", FSX_TEST_UGREP, "needle");
    check_equal(fsx_content_submit(engine, &r, &query), FS_OK);
    acquire();
    check_true(finish().complete);
  }
  it("reports malformed scope predicates before starting a content process") {
    put("a.txt", "needle\n");
    FsScanInfo scan;
    check_equal(fsx_engine_scan(engine, root, &scan), FS_OK);
    const char *scopes[] = {"<", "regex:\"[\""};
    for (unsigned i = 0; i < 2; i++) {
      FsContentRequest r =
          fsx_content_scope_request(scopes[i], FSX_TEST_UGREP, "needle");
      check_equal(fsx_content_submit(engine, &r, &query), FS_OK);
      FsContentInfo info = finish();
      check_equal(info.status, FS_INVALID);
      check_false(info.complete);
      check_false(info.candidate_count_known);
      check_equal(info.processes_started, (uint64_t)0);
      check_true(info.diagnostic[0] != 0);
      fsx_content_release(query);
      query = NULL;
    }
  }
  it("cancels scope enumeration under backpressure and retains acquired "
     "records") {
    put("a.txt", "needle\n");
    put("b.txt", "needle\n");
    FsScanInfo scan;
    check_equal(fsx_engine_scan(engine, root, &scan), FS_OK);
    FsContentRequest r =
        fsx_content_scope_request("file:", FSX_TEST_UGREP, "needle");
    check_equal(fsx_content_submit(engine, &r, &query), FS_OK);
    acquire();
    fsx_content_cancel(query);
    FsContentInfo info = finish();
    check_equal(info.status, FS_CANCELLED);
    check_false(info.complete);
    check_false(info.candidate_count_known);
    check_equal(info.candidate_count, (uint64_t)1);
    check_equal(info.processes_started, (uint64_t)1);
    fsx_content_release(query);
    query = NULL;
    FsContentRecordView record;
    check_equal(fsx_content_batch_record(batch, 0, &record), FS_OK);
    check_equal(record.text, "needle");
    check_equal(fsx_engine_close(&engine, 0), FS_BUSY);
  }
  it("validates explicit scope selection and deadlines without a fallback") {
    FsContentRequest r = request("needle");
    r.scope = "file:";
    check_equal(fsx_content_submit(engine, &r, &query), FS_INVALID);
    r.candidates = NULL;
    r.scope_flags = FS_QUERY_DESCENDING;
    check_equal(fsx_content_submit(engine, &r, &query), FS_INVALID);
    r.scope_flags = 0;
    r.timezone_offset_minutes = 841;
    check_equal(fsx_content_submit(engine, &r, &query), FS_INVALID);
    r.timezone_offset_minutes = 0;
    r.scope = "\xff";
    check_equal(fsx_content_submit(engine, &r, &query), FS_INVALID);
    r.scope = NULL;
    check_equal(fsx_content_submit(engine, &r, &query), FS_INVALID);
    r.scope = "file:";
    r.deadline_ns = 0;
    check_equal(fsx_content_submit(engine, &r, &query), FS_OK);
    FsContentInfo info = finish();
    check_equal(info.status, FS_TIMEOUT);
    check_false(info.candidate_count_known);
    check_equal(info.processes_started, (uint64_t)0);
  }
  it("discovers unindexed UTF8 paths with bfs and retains typed file "
     "identities") {
    put("-e [猫]\n.txt", "needle\n");
    put(".hidden", "needle\n");
    char sub[1200], link_path[1200];
    snprintf(sub, sizeof(sub), "%s/sub", root);
    check_equal(mkdir(sub, 0700), 0);
    put("sub/nested", "needle\n");
    snprintf(link_path, sizeof(link_path), "%s/link", root);
    check_equal(symlink("sub", link_path), 0);
    FsContentRequest r = live_request("needle");
    check_equal(fsx_content_submit(engine, &r, &query), FS_OK);
    memset(live_root, 'x', strlen(live_root));
    unsigned seen = 0;
    for (unsigned i = 0; i < 3; i++) {
      acquire();
      FsContentFileView file;
      check_equal(fsx_content_batch_info(batch, &file), FS_OK);
      check_equal(file.metadata_epoch, (uint64_t)0);
      check_equal(file.entry_key, (uint64_t)0);
      check_equal(file.key.source_id, 0u);
      check_true(file.key.file_id_lo != 0);
      seen |= strstr(file.path, "-e [猫]\n.txt") ? 1u
              : strstr(file.path, ".hidden")     ? 2u
                                                 : 4u;
      fsx_content_batch_release(batch);
      batch = NULL;
    }
    check_equal(seen, 7u);
    FsContentInfo info = finish();
    check_true(info.complete);
    check_true(info.candidate_count_known);
    check_equal(info.candidate_source, (uint32_t)FS_CONTENT_BFS_LIVE);
    check_equal(info.candidate_count, (uint64_t)3);
    check_equal(info.discovery_processes_started, (uint64_t)1);
    check_equal(info.processes_started, (uint64_t)4);
    check_equal(info.discovery_exit_code, 0);
    check_equal(info.running_processes, 0u);
    FsProgress progress;
    check_equal(fsx_engine_progress(engine, &progress), FS_OK);
    check_equal(progress.entries, (uint64_t)0);
    check_equal(progress.snapshot_epoch, (uint64_t)0);
  }
  it("keeps live hardlink aliases and owns the path after query release") {
    put("a.txt", "needle\n");
    char a[1200], b[1200];
    snprintf(a, sizeof(a), "%s/a.txt", root);
    snprintf(b, sizeof(b), "%s/b.txt", root);
    check_equal(link(a, b), 0);
    FsContentRequest r = live_request("needle");
    check_equal(fsx_content_submit(engine, &r, &query), FS_OK);
    acquire();
    FsContentFileView first, second;
    check_equal(fsx_content_batch_info(batch, &first), FS_OK);
    uint64_t inode = first.key.file_id_lo;
    char first_path[1400];
    strcpy(first_path, first.path);
    fsx_content_batch_release(batch);
    batch = NULL;
    acquire();
    check_equal(fsx_content_batch_info(batch, &second), FS_OK);
    check_equal(second.key.file_id_lo, inode);
    check_true(strcmp(first_path, second.path) != 0);
    check_true(finish().complete);
    fsx_content_release(query);
    query = NULL;
    check_equal(fsx_content_batch_info(batch, &second), FS_OK);
    check_true(strstr(second.path, ".txt") != NULL);
    FsContentRecordView record;
    check_equal(fsx_content_batch_record(batch, 0, &record), FS_OK);
    check_equal(record.text, "needle");
  }
  it("reports binary coverage and native invalid-name behavior") {
#ifdef __APPLE__
    char invalid[1200];
    snprintf(invalid, sizeof(invalid), "%s/invalid-\xff", root);
    FILE *rejected = fopen(invalid, "wb");
    check_null(rejected);
    check_equal(errno, EILSEQ);
#else
    put("invalid-\xff", "needle\n");
#endif
    put_bytes("binary", "needle\0", 7);
    char path[1200];
    snprintf(path, sizeof(path), "%s/fifo", root);
    check_equal(mkfifo(path, 0600), 0);
    snprintf(path, sizeof(path), "%s/link", root);
    check_equal(symlink("binary", path), 0);
    FsContentRequest r = live_request("needle");
    check_equal(fsx_content_submit(engine, &r, &query), FS_OK);
    FsContentInfo info = finish();
    check_equal(info.status, FS_OK);
    check_false(info.complete);
    check_true(info.candidate_count_known);
#ifdef __APPLE__
    check_equal(info.candidate_count, (uint64_t)1);
    check_equal(info.skipped_files, (uint64_t)1);
#else
    check_equal(info.candidate_count, (uint64_t)2);
    check_equal(info.skipped_files, (uint64_t)2);
#endif
    check_equal(info.match_records, (uint64_t)0);
  }
  it("kills and reaps discovery under file-batch backpressure on STOPPING") {
    put("a.txt", "needle\n");
    put("b.txt", "needle\n");
    FsContentRequest r = live_request("needle");
    check_equal(fsx_content_submit(engine, &r, &query), FS_OK);
    acquire();
    FsContentInfo info;
    check_equal(fsx_content_poll(query, &info), FS_WAIT);
    check_equal(info.candidate_count, (uint64_t)1);
    check_false(info.candidate_count_known);
    check_equal(fsx_engine_close(&engine, 0), FS_BUSY);
    info = finish();
    check_equal(info.status, FS_CANCELLED);
    check_false(info.complete);
    check_equal(info.running_processes, 0u);
    fsx_content_release(query);
    query = NULL;
    FsContentFileView file;
    check_equal(fsx_content_batch_info(batch, &file), FS_OK);
    check_true(strstr(file.path, ".txt") != NULL);
  }
  it("bounds live candidate count and discovery output without truncating "
     "success") {
    put("a.txt", "needle\n");
    put("b.txt", "needle\n");
    FsContentRequest r = live_request("needle");
    r.max_candidates = 1;
    check_equal(fsx_content_submit(engine, &r, &query), FS_OK);
    acquire();
    fsx_content_batch_release(batch);
    batch = NULL;
    FsContentInfo info = finish();
    check_equal(info.status, FS_LIMIT);
    check_false(info.candidate_count_known);
    check_false(info.complete);
    check_equal(info.candidate_count, (uint64_t)1);
    check_equal(info.running_processes, 0u);
    fsx_content_release(query);
    query = NULL;
    r.max_candidates = 100;
    r.max_discovery_bytes = 1;
    check_equal(fsx_content_submit(engine, &r, &query), FS_OK);
    info = finish();
    check_equal(info.status, FS_LIMIT);
    check_equal(info.candidate_count, (uint64_t)0);
    check_equal(info.running_processes, 0u);
    check_true(info.discovery_bytes > 1);
  }
  it("decodes NUL path frames crossing discovery pipe chunks") {
    for (unsigned i = 0; i < 80; i++) {
      char name[200];
      memset(name, 'x', 150);
      snprintf(name + 150, sizeof(name) - 150, "%u.txt", i);
      put(name, "absent\n");
    }
    FsContentRequest r = live_request("needle");
    check_equal(fsx_content_submit(engine, &r, &query), FS_OK);
    FsContentInfo info = finish();
    check_true(info.complete);
    check_true(info.candidate_count_known);
    check_equal(info.candidate_count, (uint64_t)80);
    check_equal(info.files_scanned, (uint64_t)80);
    check_true(info.discovery_bytes > 8192);
  }
  it("fails a live root replacement without reading the new root") {
    put("a.txt", "needle\n");
    put("b.txt", "needle\n");
    FsContentRequest r = live_request("needle");
    check_equal(fsx_content_submit(engine, &r, &query), FS_OK);
    acquire();
    char parked[1200];
    snprintf(parked, sizeof(parked), "%s-parked", root);
    check_equal(rename(root, parked), 0);
    check_equal(mkdir(root, 0700), 0);
    put("a.txt", "outside\n");
    fsx_content_batch_release(batch);
    batch = NULL;
    FsContentInfo info = finish();
    check_equal(info.status, FS_STALE);
    check_false(info.complete);
    check_false(info.candidate_count_known);
    check_equal(info.files_scanned, (uint64_t)1);
    remove_tree(root);
    check_equal(rename(parked, root), 0);
  }
  it("rejects invalid live configuration and reports missing native tools") {
    FsContentRequest r = live_request("needle");
    r.scope = "file:";
    check_equal(fsx_content_submit(engine, &r, &query), FS_INVALID);
    r.scope = NULL;
    r.bfs_executable = "bfs";
    check_equal(fsx_content_submit(engine, &r, &query), FS_INVALID);
    r.bfs_executable = FSX_TEST_BFS;
    r.root = "/tmp/../tree";
    check_equal(fsx_content_submit(engine, &r, &query), FS_INVALID);
    r.root = "relative";
    check_equal(fsx_content_submit(engine, &r, &query), FS_INVALID);
    r = live_request("needle");
    r.max_candidates = 0;
    check_equal(fsx_content_submit(engine, &r, &query), FS_INVALID);
    r = live_request("needle");
    r.bfs_executable = "/fsx-nonexistent-bfs";
    check_equal(fsx_content_submit(engine, &r, &query), FS_OK);
    FsContentInfo info = finish();
    check_equal(info.status, FS_IO);
    check_false(info.complete);
    check_false(info.candidate_count_known);
    check_equal(info.discovery_processes_started, (uint64_t)0);
  }
  it("honors live deadlines before discovery starts") {
    FsContentRequest r = live_request("needle");
    r.deadline_ns = 0;
    check_equal(fsx_content_submit(engine, &r, &query), FS_OK);
    FsContentInfo info = finish();
    check_equal(info.status, FS_TIMEOUT);
    check_equal(info.processes_started, (uint64_t)0);
    check_false(info.candidate_count_known);
  }
  it("rejects malformed discovery frames and native traversal failures") {
    const FsStatus expected[] = {FS_CORRUPT, FS_CORRUPT, FS_CORRUPT, FS_LIMIT,
                                 FS_IO,      FS_IO,      FS_CORRUPT};
    const unsigned modes[] = {1, 2, 3, 4, 5, 6, 8};
    for (unsigned i = 0; i < 7; i++) {
      char mode[10];
      snprintf(mode, sizeof(mode), "%u", modes[i]);
      put("protocol-case", mode);
      FsContentRequest r = live_request("needle");
      r.bfs_executable = FSX_TEST_CONTENT_PEER;
      check_equal(fsx_content_submit(engine, &r, &query), FS_OK);
      FsContentInfo info = finish();
      check_equal(info.status, expected[i]);
      check_false(info.complete);
      check_false(info.candidate_count_known);
      check_equal(info.running_processes, 0u);
      check_equal(info.processes_started, (uint64_t)1);
      check_equal(info.candidate_count, (uint64_t)0);
      if (modes[i] == 5)
        check_true(strstr(info.diagnostic, "permission") != NULL);
      if (modes[i] == 6)
        check_equal(info.discovery_exit_code, 7);
      fsx_content_release(query);
      query = NULL;
    }
  }
  it("interrupts and reaps a discovery process that makes no pipe progress") {
    put("protocol-case", "7");
    FsContentRequest r = live_request("needle");
    r.bfs_executable = FSX_TEST_CONTENT_PEER;
    r.deadline_ns = cmeta_hrtime() + UINT64_C(200000000);
    check_equal(fsx_content_submit(engine, &r, &query), FS_OK);
    FsContentInfo info = finish();
    check_equal(info.status, FS_TIMEOUT);
    check_false(info.complete);
    check_false(info.candidate_count_known);
    check_equal(info.processes_started, (uint64_t)1);
    check_equal(info.running_processes, 0u);
  }
  it("cancels and reaps both live discovery and a running matcher") {
    char path[1200], block[65536];
    memset(block, 'x', sizeof(block));
    snprintf(path, sizeof(path), "%s/large", root);
    FILE *file = fopen(path, "wb");
    check_not_null(file);
    for (unsigned i = 0; i < 256; i++)
      check_equal(fwrite(block, 1, sizeof(block), file), sizeof(block));
    check_equal(fclose(file), 0);
    FsContentRequest r = live_request("needle");
    check_equal(fsx_content_submit(engine, &r, &query), FS_OK);
    FsContentInfo info;
    uint64_t end = cmeta_hrtime() + UINT64_C(5000000000);
    do {
      fsx_content_poll(query, &info);
      check_false(info.finished);
      check_true(cmeta_hrtime() < end);
    } while (info.running_processes < 2);
    fsx_content_cancel(query);
    info = finish();
    check_equal(info.status, FS_CANCELLED);
    check_equal(info.processes_started, (uint64_t)2);
    check_equal(info.running_processes, 0u);
    check_false(info.complete);
  }
  it("excludes an unrelated non-CLOEXEC descriptor from POSIX children") {
    put("host-file", "private\n");
    char path[1200];
    snprintf(path, sizeof(path), "%s/host-file", root);
    int descriptor = open(path, O_RDONLY);
    check_true(descriptor >= 3);
    struct stat identity;
    check_equal(fstat(descriptor, &identity), 0);
    char mode[100];
    snprintf(mode, sizeof(mode), "9 %d %llu %llu", descriptor,
             (unsigned long long)identity.st_dev,
             (unsigned long long)identity.st_ino);
    put("protocol-case", mode);
    FsContentRequest r = live_request("needle");
    r.bfs_executable = FSX_TEST_CONTENT_PEER;
    check_equal(fsx_content_submit(engine, &r, &query), FS_OK);
    FsContentInfo info = finish();
    check_equal(close(descriptor), 0);
    check_equal(info.status, FS_OK);
    check_true(info.complete);
    check_equal(info.discovery_exit_code, 0);
  }
  it("survives a matcher closing stdin before a large input is written") {
    char *data = malloc(1024 * 1024);
    check_not_null(data);
    memset(data, 'x', 1024 * 1024);
    put_bytes("input", data, 1024 * 1024);
    free(data);
    FsContentRequest r = request("needle");
    r.ugrep_executable = FSX_TEST_CONTENT_PEER;
    check_equal(fsx_content_submit(engine, &r, &query), FS_OK);
    FsContentInfo info = finish();
    check_equal(info.status, FS_IO);
    check_equal(info.running_processes, 0u);
    FsScanInfo scan;
    check_equal(fsx_engine_scan(engine, root, &scan), FS_OK);
  }
  it("requires explicit one or two file workers") {
    put("a.txt", "needle\n");
    FsContentRequest r = request("needle");
    check_equal(r.parallel_files, 1u);
    r.parallel_files = 0;
    check_equal(fsx_content_submit(engine, &r, &query), FS_INVALID);
    check_null(query);
    r.parallel_files = 3;
    check_equal(fsx_content_submit(engine, &r, &query), FS_INVALID);
    check_null(query);
  }
  it("runs two native matcher roots while metadata queries and indexing advance") {
    put("a.txt", "A");
    put("b.txt", "B");
    FsContentRequest r = request("needle");
    gate_request(&r, false);
    check_equal(fsx_content_submit(engine, &r, &query), FS_OK);
    FsContentInfo held = two_ready(false);
    check_equal(held.running_processes, 2u);
    check_equal(held.processes_started, (uint64_t)2);
    put("new.txt", "new");
    FsScanInfo scanned;
    check_equal(fsx_engine_scan(engine, root, &scanned), FS_OK);
    check_true(scanned.snapshot_epoch > held.metadata_epoch);
    FsQueryRequest names = {sizeof(names), FSX_API_VERSION, "new", 3,
                            fsx_query_options()};
    FsQuery *metadata = NULL;
    FsResult *result = NULL;
    check_equal(fsx_query_submit(engine, &names, &metadata), FS_OK);
    check_equal(fsx_query_wait(metadata, cmeta_hrtime() + UINT64_C(5000000000)), FS_OK);
    check_equal(fsx_query_result_acquire(metadata, &result), FS_OK);
    FsResultInfo info;
    check_equal(fsx_result_info(result, &info), FS_OK);
    check_equal(info.total_count, (uint64_t)1);
    fsx_result_release(result);
    fsx_query_release(metadata);
    release_gate();
    uint64_t records;
    FsContentInfo done = collect_all(&records);
    check_equal(done.status, FS_OK);
    check_true(done.complete);
    check_equal(done.metadata_epoch, held.metadata_epoch);
    check_equal(records, (uint64_t)2);
  }
  it("bounds a full metadata candidate queue and cancels both native matchers") {
    for (unsigned i = 0; i < 140; i++) {
      char name[40];
      snprintf(name, sizeof(name), "item-%03u.txt", i);
      put(name, "A");
    }
    FsScanInfo scan;
    check_equal(fsx_engine_scan(engine, root, &scan), FS_OK);
    FsContentRequest r = fsx_content_scope_request("file:", FSX_TEST_UGREP, "needle");
    gate_request(&r, false);
    check_equal(fsx_content_submit(engine, &r, &query), FS_OK);
    FsContentInfo info = two_ready(true);
    check_equal(info.running_processes, 2u);
    check_equal(info.queued_path_bytes, 0u);
    fsx_content_cancel(query);
    info = finish();
    check_equal(info.status, FS_CANCELLED);
    check_equal(info.running_processes, 0u);
    check_equal(info.queued_candidates, 0u);
  }
  it("owns queued BFS paths and joins discovery and two workers on STOPPING") {
    for (unsigned i = 0; i < 140; i++) {
      char name[40];
      snprintf(name, sizeof(name), "item-%03u.txt", i);
      put(name, "A");
    }
    FsContentRequest r = live_request("needle");
    gate_request(&r, false);
    check_equal(fsx_content_submit(engine, &r, &query), FS_OK);
    FsContentInfo info = two_ready(true);
    check_equal(info.running_processes, 3u);
    check_true(info.queued_path_bytes > 0);
    check_equal(fsx_engine_close(&engine, 0), FS_BUSY);
    info = finish();
    check_equal(info.status, FS_CANCELLED);
    check_equal(info.running_processes, 0u);
    check_equal(info.queued_candidates, 0u);
    check_equal(info.queued_path_bytes, 0u);
  }
  it("propagates one matcher error and kills its blocked sibling") {
    put("a.txt", "A");
    put("b.txt", "B");
    FsContentRequest r = request("needle");
    gate_request(&r, true);
    check_equal(fsx_content_submit(engine, &r, &query), FS_OK);
    two_ready(false);
    release_gate();
    FsContentInfo info = finish();
    check_equal(info.status, FS_IO);
    check_equal(info.running_processes, 0u);
    check_not_null(strstr(info.diagnostic, "injected parallel failure"));
  }
  it("keeps the BFS root alive after producer exhaustion with real parallel ugrep") {
    for (unsigned i = 0; i < 20; i++) {
      char name[40];
      snprintf(name, sizeof(name), "item-%03u.txt", i);
      put(name, "needle needle\n");
    }
    FsContentRequest r = live_request("needle");
    r.parallel_files = 2;
    check_equal(fsx_content_submit(engine, &r, &query), FS_OK);
    uint64_t records;
    FsContentInfo info = collect_all(&records);
    check_equal(info.status, FS_OK);
    check_true(info.complete);
    check_true(info.candidate_count_known);
    check_equal(info.candidate_count, (uint64_t)20);
    check_equal(info.files_scanned, (uint64_t)20);
    check_equal(info.processes_started, (uint64_t)21);
    check_equal(records, (uint64_t)40);
  }
  it("applies the record limit globally across parallel file results") {
    put("a.txt", "needle\n");
    put("b.txt", "needle\n");
    FsContentRequest r = request("needle");
    r.parallel_files = 2;
    r.max_records = 1;
    check_equal(fsx_content_submit(engine, &r, &query), FS_OK);
    uint64_t records;
    FsContentInfo info = collect_all(&records);
    check_equal(info.status, FS_LIMIT);
    check_false(info.complete);
    check_equal(records, (uint64_t)1);
  }
  it("bounds retained parallel batches and preserves them across query release") {
    put("a.txt", "needle\n");
    put("b.txt", "needle\n");
    put("c.txt", "needle\n");
    FsContentRequest r = request("needle");
    r.parallel_files = 2;
    check_equal(fsx_content_submit(engine, &r, &query), FS_OK);
    acquire();
    held_batch = batch;
    batch = NULL;
    acquire();
    FsContentInfo info;
    fsx_content_poll(query, &info);
    check_equal(info.processes_started, (uint64_t)2);
    check_false(info.finished);
    fsx_content_cancel(query);
    info = finish();
    check_equal(info.status, FS_CANCELLED);
    fsx_content_release(query);
    query = NULL;
    FsContentRecordView record;
    check_equal(fsx_content_batch_record(held_batch, 0, &record), FS_OK);
    check_equal(record.text, "needle");
    fsx_content_batch_release(held_batch);
    held_batch = NULL;
    check_equal(fsx_content_batch_record(batch, 0, &record), FS_OK);
    check_equal(record.text, "needle");
  }

}

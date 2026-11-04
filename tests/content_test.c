#define _XOPEN_SOURCE 700
#include <clock.h>
#include <dirent.h>
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
static FsContentBatch *batch;
static char root[1024];
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
  engine = NULL;
  selection = NULL;
  candidates = NULL;
  query = NULL;
  batch = NULL;
  FsEngineConfig c = fsx_engine_config(":memory:");
  check_equal(fsx_engine_open(&c, &engine), FS_OK);
}
static void teardown(void) {
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
  DIR *dir = opendir(root);
  check_not_null(dir);
  struct dirent *entry;
  while ((entry = readdir(dir))) {
    if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
      continue;
    char path[1400];
    snprintf(path, sizeof(path), "%s/%s", root, entry->d_name);
    check_equal(unlink(path), 0);
  }
  closedir(dir);
  check_equal(rmdir(root), 0);
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
}

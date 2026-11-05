#define WIN32_LEAN_AND_MEAN
#include <clock.h>
#include <fsx/api.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <thread.h>
#include <tinytest.h>
#include <wchar.h>
#include <windows.h>
static FsEngine *engine;
static FsContentQuery *query;
static FsContentBatch *batch, *held_batch;
static FsQuery *selection;
static FsResult *candidates;
static wchar_t root_wide[2048];
static char root[4096], gate_pattern[4300];
static wchar_t gate[2048];
static void path(const char *name, wchar_t out[4096]) {
  wchar_t wide[1024];
  check_true(MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name, -1, wide,
                                 1024) > 0);
  swprintf(out, 4096, L"%ls\\%ls", root_wide, wide);
}
static void put_bytes(const char *name, const void *data, DWORD length) {
  wchar_t target[4096];
  path(name, target);
  HANDLE file =
      CreateFileW(target, GENERIC_WRITE,
                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                  CREATE_ALWAYS, 0, NULL);
  check_true(file != INVALID_HANDLE_VALUE);
  DWORD written;
  check_true(WriteFile(file, data, length, &written, NULL));
  check_equal(written, length);
  CloseHandle(file);
}
static void put(const char *name, const char *text) {
  put_bytes(name, text, (DWORD)strlen(text));
}
static void setup(void) {
  wchar_t unique[MAX_PATH], full[2048];
  check_true(GetTempFileNameW(L".", L"fsc", 0, unique) != 0);
  check_true(DeleteFileW(unique));
  check_true(CreateDirectoryW(unique, NULL));
  check_true(GetFullPathNameW(unique, 2048, full, NULL) > 0);
  wcscpy(root_wide, full);
  check_true(WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, full, -1, root,
                                 sizeof(root), NULL, NULL) > 0);
  engine = NULL;
  query = NULL;
  batch = held_batch = NULL;
  gate[0] = 0;
  selection = NULL;
  candidates = NULL;
  FsEngineConfig config = fsx_engine_config(":memory:");
  check_equal(fsx_engine_open(&config, &engine), FS_OK);
}
static void remove_flat(const wchar_t *directory) {
  wchar_t glob[4096];
  swprintf(glob, 4096, L"%ls\\*", directory);
  WIN32_FIND_DATAW item;
  HANDLE listing = FindFirstFileW(glob, &item);
  check_true(listing != INVALID_HANDLE_VALUE);
  do {
    if (!wcscmp(item.cFileName, L".") || !wcscmp(item.cFileName, L".."))
      continue;
    wchar_t child[4096];
    swprintf(child, 4096, L"%ls\\%ls", directory, item.cFileName);
    check_true(DeleteFileW(child));
  } while (FindNextFileW(listing, &item));
  FindClose(listing);
  check_true(RemoveDirectoryW(directory));
}
static void teardown(void) {
  fsx_content_batch_release(held_batch);
  fsx_content_batch_release(batch);
  fsx_content_release(query);
  fsx_result_release(candidates);
  fsx_query_release(selection);
  if (engine)
    check_equal(fsx_engine_close(&engine, UINT64_MAX), FS_OK);
  remove_flat(root_wide);
  if (gate[0])
    remove_flat(gate);
}
static FsContentRequest request(const char *pattern) {
  FsScanInfo scan;
  check_equal(fsx_engine_scan(engine, root, &scan), FS_OK);
  return fsx_content_scope_request("file:", FSX_TEST_UGREP, pattern);
}
static void acquire(void) {
  check_equal(fsx_content_wait(query, cmeta_hrtime() + UINT64_C(10000000000)),
              FS_OK);
  check_equal(fsx_content_batch_acquire(query, &batch), FS_OK);
}
static FsContentInfo finish(void) {
  uint64_t end = cmeta_hrtime() + UINT64_C(10000000000);
  FsContentInfo info;
  do {
    fsx_content_poll(query, &info);
    if (info.finished)
      return info;
    check_true(cmeta_hrtime() < end);
    cmeta_sleep_ms(1);
  } while (true);
}

static void gate_request(FsContentRequest *r, bool fail) {
  swprintf(gate, 2048, L"%ls-gate", root_wide);
  check_true(CreateDirectoryW(gate, NULL));
  char utf8[4200];
  check_true(WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, gate, -1, utf8,
                                 sizeof(utf8), NULL, NULL) > 0);
  snprintf(gate_pattern, sizeof(gate_pattern), "%s%s",
           fail ? "parallel-fail:" : "parallel:", utf8);
  r->pattern = gate_pattern;
  r->ugrep_executable = FSX_TEST_CONTENT_PEER;
  r->parallel_files = 2;
  r->max_output_bytes = 1024;
}
static FsContentInfo two_ready(bool full_queue) {
  uint64_t end = cmeta_hrtime() + UINT64_C(10000000000);
  for (;;) {
    FsContentInfo info;
    fsx_content_poll(query, &info);
    check_false(info.finished);
    check_true(info.running_processes <= 2);
    check_true(info.queued_candidates <= 128);
    check_equal(info.queued_path_bytes, 0u);
    wchar_t glob[4096];
    swprintf(glob, 4096, L"%ls\\*.ready", gate);
    WIN32_FIND_DATAW item;
    HANDLE listing = FindFirstFileW(glob, &item);
    unsigned ready = 0;
    if (listing != INVALID_HANDLE_VALUE) {
      do {
        ready++;
      } while (FindNextFileW(listing, &item));
      FindClose(listing);
    }
    if (ready == 2 && (!full_queue || info.queued_candidates == 128))
      return info;
    check_true(cmeta_hrtime() < end);
    cmeta_sleep_ms(1);
  }
}
static void release_gate(void) {
  wchar_t path[4096];
  swprintf(path, 4096, L"%ls\\go", gate);
  HANDLE file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                            CREATE_ALWAYS, 0, NULL);
  check_true(file != INVALID_HANDLE_VALUE);
  CloseHandle(file);
}
static FsContentInfo collect_all(uint64_t *records) {
  *records = 0;
  uint64_t end = cmeta_hrtime() + UINT64_C(10000000000);
  for (;;) {
    FsStatus status = fsx_content_wait(query, end);
    check_true(status != FS_TIMEOUT);
    while (fsx_content_batch_acquire(query, &batch) == FS_OK) {
      FsContentFileView file;
      check_equal(fsx_content_batch_info(batch, &file), FS_OK);
      *records += file.record_count;
      fsx_content_batch_release(batch);
      batch = NULL;
    }
    FsContentInfo info;
    fsx_content_poll(query, &info);
    if (info.finished) {
      /* A worker can publish between the drain and the terminal observation. */
      FsStatus acquired = fsx_content_batch_acquire(query, &batch);
      if (acquired == FS_OK) {
        FsContentFileView file;
        check_equal(fsx_content_batch_info(batch, &file), FS_OK);
        *records += file.record_count;
        fsx_content_batch_release(batch);
        batch = NULL;
      } else
        check_equal(acquired, FS_END);
      check_equal(info.match_records, *records);
      check_equal(info.running_processes, 0u);
      check_equal(info.queued_candidates, 0u);
      return info;
    }
    check_true(cmeta_hrtime() < end);
  }
}

suite("Windows native content processes") {
  before_each() { setup(); }
  after_each() { teardown(); }
  it("retains Unicode result windows and exact UTF8 match offsets") {
    put("猫 space.txt", "zero\n猫 needle needle\n");
    FsContentRequest r = request("needle");
    FsQueryRequest selected = {sizeof(selected), FSX_API_VERSION, "file:", 5,
                               fsx_query_options()};
    check_equal(fsx_query_submit(engine, &selected, &selection), FS_OK);
    check_equal(fsx_query_wait(selection, UINT64_MAX), FS_OK);
    check_equal(fsx_query_result_acquire(selection, &candidates), FS_OK);
    r = fsx_content_request(candidates, FSX_TEST_UGREP, "needle");
    check_equal(fsx_content_submit(engine, &r, &query), FS_OK);
    fsx_result_release(candidates);
    candidates = NULL;
    fsx_query_release(selection);
    selection = NULL;
    acquire();
    FsContentFileView file;
    check_equal(fsx_content_batch_info(batch, &file), FS_OK);
    check_true(strstr(file.path, "猫 space.txt") != NULL);
    check_equal(file.record_count, 2u);
    FsContentRecordView record;
    check_equal(fsx_content_batch_record(batch, 0, &record), FS_OK);
    check_equal(record.byte_offset, (uint64_t)9);
    check_equal(fsx_content_batch_record(batch, 1, &record), FS_OK);
    check_equal(record.byte_offset, (uint64_t)16);
    check_true(finish().complete);
  }
  it("quotes Unicode patterns with embedded quotes and trailing backslashes") {
    const char *text = "\"猫 needle\\tail\\";
    put("text", text);
    FsContentRequest r = request(text);
    check_equal(fsx_content_submit(engine, &r, &query), FS_OK);
    acquire();
    FsContentRecordView record;
    check_equal(fsx_content_batch_record(batch, 0, &record), FS_OK);
    check_equal(record.text, text);
    check_true(finish().complete);
  }
  it("executes actual PCRE2 lookbehind and multiline frames") {
    put("text", "prefixone\ntwo\n");
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
  it("distinguishes no-match from native tool regex failures") {
    put("text", "aXb\n");
    FsContentRequest r = request("a.b");
    check_equal(fsx_content_submit(engine, &r, &query), FS_OK);
    FsContentInfo info = finish();
    check_true(info.complete);
    check_equal(info.match_records, (uint64_t)0);
    fsx_content_release(query);
    query = NULL;
    r.pattern = "[";
    r.mode = FS_CONTENT_PCRE2;
    check_equal(fsx_content_submit(engine, &r, &query), FS_OK);
    info = finish();
    check_equal(info.status, FS_IO);
    check_false(info.complete);
    check_true(info.tool_exit_code > 1);
  }
  it("strips BOM and skips invalid or binary UTF8 input") {
    put("bom", "\xef\xbb\xbfneedle\n");
    put("invalid", "\xff");
    put_bytes("binary", "needle\0", 7);
    FsContentRequest r = request("needle");
    check_equal(fsx_content_submit(engine, &r, &query), FS_OK);
    acquire();
    fsx_content_batch_release(batch);
    batch = NULL;
    FsContentInfo info = finish();
    check_equal(info.status, FS_OK);
    check_false(info.complete);
    check_equal(info.skipped_files, (uint64_t)2);
    check_equal(info.files_scanned, (uint64_t)1);
  }
  it("rejects replacement identity before starting the matcher") {
    put("text", "old\n");
    FsContentRequest r = request("needle");
    wchar_t original[4096], parked[4096];
    path("text", original);
    path("parked", parked);
    check_true(MoveFileW(original, parked));
    put("text", "needle\n");
    check_equal(fsx_content_submit(engine, &r, &query), FS_OK);
    FsContentInfo info = finish();
    check_false(info.complete);
    check_equal(info.changed_files, (uint64_t)1);
    check_equal(info.processes_started, (uint64_t)0);
  }
  it("applies file backpressure and retains acquired batches during STOPPING") {
    put("a", "needle\n");
    put("b", "needle\n");
    FsContentRequest r = request("needle");
    check_equal(fsx_content_submit(engine, &r, &query), FS_OK);
    acquire();
    FsContentInfo info;
    check_equal(fsx_content_poll(query, &info), FS_WAIT);
    check_equal(info.files_scanned, (uint64_t)1);
    check_equal(fsx_engine_close(&engine, 0), FS_BUSY);
    check_equal(finish().status, FS_CANCELLED);
    fsx_content_release(query);
    query = NULL;
    FsContentRecordView record;
    fsx_content_batch_record(batch, 0, &record);
    check_equal(record.text, "needle");
  }
  it("feeds and drains overlapped pipes beyond kernel capacity") {
    char *data = malloc(2000 * 80);
    check_not_null(data);
    for (unsigned i = 0; i < 2000; i++) {
      memcpy(data + i * 80, "needle", 6);
      memset(data + i * 80 + 6, 'x', 73);
      data[i * 80 + 79] = '\n';
    }
    put_bytes("many", data, 2000 * 80);
    free(data);
    FsContentRequest r = request("needle");
    check_equal(fsx_content_submit(engine, &r, &query), FS_OK);
    acquire();
    FsContentFileView file;
    fsx_content_batch_info(batch, &file);
    check_equal(file.record_count, 2000u);
    check_true(finish().complete);
  }
  it("bounds records output and file bytes before publishing") {
    put("text", "needle needle\n");
    FsContentRequest r = request("needle");
    r.max_records = 1;
    check_equal(fsx_content_submit(engine, &r, &query), FS_OK);
    check_equal(finish().status, FS_LIMIT);
    fsx_content_release(query);
    query = NULL;
    r.max_records = 100;
    r.max_file_bytes = 1;
    check_equal(fsx_content_submit(engine, &r, &query), FS_OK);
    check_equal(finish().status, FS_LIMIT);
    fsx_content_release(query);
    query = NULL;
    char text[3000];
    memset(text, 'x', sizeof(text));
    put_bytes("text", text, sizeof(text));
    r.max_file_bytes = 4096;
    r.max_output_bytes = 1024;
    r.pattern = "x";
    check_equal(fsx_content_submit(engine, &r, &query), FS_OK);
    check_equal(finish().status, FS_LIMIT);
  }
  it("streams full scopes beyond the configured metadata query window") {
    check_equal(fsx_engine_close(&engine, UINT64_MAX), FS_OK);
    FsEngineConfig config = fsx_engine_config(":memory:");
    config.max_query_window = 1;
    check_equal(fsx_engine_open(&config, &engine), FS_OK);
    put("a", "needle\n");
    put("b", "needle\n");
    put("c", "needle\n");
    FsContentRequest r = request("needle");
    check_equal(fsx_content_submit(engine, &r, &query), FS_OK);
    for (unsigned i = 0; i < 3; i++) {
      acquire();
      fsx_content_batch_release(batch);
      batch = NULL;
    }
    FsContentInfo info = finish();
    check_true(info.complete);
    check_true(info.candidate_count_known);
    check_equal(info.candidate_count, (uint64_t)3);
  }
  it("rejects relative tools and unsupported live discovery and honors "
     "deadlines") {
    FsContentRequest r = request("needle");
    r.ugrep_executable = "ugrep.exe";
    check_equal(fsx_content_submit(engine, &r, &query), FS_INVALID);
    r.ugrep_executable = FSX_TEST_UGREP;
    r.deadline_ns = 0;
    check_equal(fsx_content_submit(engine, &r, &query), FS_OK);
    check_equal(finish().status, FS_TIMEOUT);
    fsx_content_release(query);
    query = NULL;
    r = fsx_content_live_request(root, "bfs", FSX_TEST_UGREP, "needle");
    check_equal(fsx_content_submit(engine, &r, &query), FS_UNSUPPORTED);
  }
  it("excludes unrelated inheritable host handles from the actual child") {
    put("text", "data\n");
    SECURITY_ATTRIBUTES inherit = {sizeof(inherit), NULL, TRUE};
    HANDLE event = CreateEventW(&inherit, TRUE, FALSE, NULL);
    check_not_null(event);
    char pattern[80];
    snprintf(pattern, sizeof(pattern), "handle:%llx",
             (unsigned long long)(uintptr_t)event);
    FsContentRequest r = request(pattern);
    r.ugrep_executable = FSX_TEST_CONTENT_PEER;
    check_equal(fsx_content_submit(engine, &r, &query), FS_OK);
    acquire();
    check_true(finish().complete);
    check_equal(WaitForSingleObject(event, 0), (DWORD)WAIT_TIMEOUT);
    CloseHandle(event);
  }
  it("terminates and joins a child process tree on cancellation") {
    put("text", "data\n");
    char pattern[4300];
    snprintf(pattern, sizeof(pattern), "tree:%s/pid", root);
    FsContentRequest r = request(pattern);
    r.ugrep_executable = FSX_TEST_CONTENT_PEER;
    check_equal(fsx_content_submit(engine, &r, &query), FS_OK);
    wchar_t pid_path[4096];
    path("pid", pid_path);
    DWORD pid = 0;
    uint64_t end = cmeta_hrtime() + UINT64_C(10000000000);
    while (!pid) {
      HANDLE file = CreateFileW(pid_path, GENERIC_READ,
                                FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                                OPEN_EXISTING, 0, NULL);
      if (file != INVALID_HANDLE_VALUE) {
        DWORD read = 0;
        ReadFile(file, &pid, sizeof(pid), &read, NULL);
        CloseHandle(file);
        if (read != sizeof(pid))
          pid = 0;
      }
      check_true(cmeta_hrtime() < end);
      cmeta_sleep_ms(1);
    }
    HANDLE descendant = OpenProcess(SYNCHRONIZE, FALSE, pid);
    check_not_null(descendant);
    fsx_content_cancel(query);
    FsContentInfo info = finish();
    check_equal(info.status, FS_CANCELLED);
    check_equal(info.running_processes, 0u);
    check_equal(WaitForSingleObject(descendant, 0), (DWORD)WAIT_OBJECT_0);
    CloseHandle(descendant);
  }
  it("requires explicit one or two file workers") {
    put("a", "needle\n");
    FsContentRequest r = request("needle");
    check_equal(r.parallel_files, 1u);
    r.parallel_files = 0;
    check_equal(fsx_content_submit(engine, &r, &query), FS_INVALID);
    check_null(query);
    r.parallel_files = 3;
    check_equal(fsx_content_submit(engine, &r, &query), FS_INVALID);
    check_null(query);
  }
  it("runs two native matchers while metadata indexing and queries advance") {
    put("a", "A");
    put("b", "B");
    FsContentRequest r = request("needle");
    gate_request(&r, false);
    check_equal(fsx_content_submit(engine, &r, &query), FS_OK);
    FsContentInfo held = two_ready(false);
    check_equal(held.running_processes, 2u);
    check_equal(held.processes_started, (uint64_t)2);
    put("new", "new");
    FsScanInfo scanned;
    check_equal(fsx_engine_scan(engine, root, &scanned), FS_OK);
    check_true(scanned.snapshot_epoch > held.metadata_epoch);
    FsQueryRequest names = {sizeof(names), FSX_API_VERSION, "new", 3,
                            fsx_query_options()};
    check_equal(fsx_query_submit(engine, &names, &selection), FS_OK);
    check_equal(
        fsx_query_wait(selection, cmeta_hrtime() + UINT64_C(5000000000)),
        FS_OK);
    check_equal(fsx_query_result_acquire(selection, &candidates), FS_OK);
    FsResultInfo selected;
    check_equal(fsx_result_info(candidates, &selected), FS_OK);
    check_equal(selected.total_count, (uint64_t)1);
    release_gate();
    uint64_t records;
    FsContentInfo info = collect_all(&records);
    check_true(info.complete);
    check_equal(info.metadata_epoch, held.metadata_epoch);
    check_equal(records, (uint64_t)2);
  }
  it("bounds metadata candidates and joins two native jobs on STOPPING") {
    for (unsigned i = 0; i < 140; i++) {
      char name[40];
      snprintf(name, sizeof(name), "item-%03u", i);
      put(name, "A");
    }
    FsContentRequest r = request("needle");
    gate_request(&r, false);
    check_equal(fsx_content_submit(engine, &r, &query), FS_OK);
    two_ready(true);
    check_equal(fsx_engine_close(&engine, 0), FS_BUSY);
    FsContentInfo info = finish();
    check_equal(info.status, FS_CANCELLED);
    check_equal(info.running_processes, 0u);
    check_equal(info.queued_candidates, 0u);
  }
  it("propagates a native matcher failure and joins its blocked sibling") {
    put("a", "A");
    put("b", "B");
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
  it("streams actual parallel ugrep across a full metadata scope") {
    for (unsigned i = 0; i < 20; i++) {
      char name[40];
      snprintf(name, sizeof(name), "item-%03u", i);
      put(name, "needle needle\n");
    }
    FsContentRequest r = request("needle");
    r.parallel_files = 2;
    check_equal(fsx_content_submit(engine, &r, &query), FS_OK);
    uint64_t records;
    FsContentInfo info = collect_all(&records);
    check_true(info.complete);
    check_true(info.candidate_count_known);
    check_equal(info.candidate_count, (uint64_t)20);
    check_equal(info.files_scanned, (uint64_t)20);
    check_equal(info.processes_started, (uint64_t)20);
    check_equal(records, (uint64_t)40);
  }
  it("enforces a global record limit across two native matchers") {
    put("a", "needle\n");
    put("b", "needle\n");
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
  it("retains two bounded batches through cancellation and query release") {
    put("a", "needle\n");
    put("b", "needle\n");
    put("c", "needle\n");
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
    check_equal(finish().status, FS_CANCELLED);
    fsx_content_release(query);
    query = NULL;
    FsContentRecordView record;
    check_equal(fsx_content_batch_record(held_batch, 0, &record), FS_OK);
    check_equal(record.text, "needle");
    check_equal(fsx_content_batch_record(batch, 0, &record), FS_OK);
    check_equal(record.text, "needle");
  }
}

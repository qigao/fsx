#ifdef __APPLE__
#define _DARWIN_C_SOURCE
#endif
#define _XOPEN_SOURCE 700
#include "../src/content/content.h"
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <tinytest.h>
#include <unistd.h>

static FsEngine *engine;
static FsContentQuery *query;
static FsContentBatch *batch, *held;
static char root[1200], absolute_root[1400];

static void put_bytes(const char *name, const void *data, size_t bytes) {
  char path[1600];
  snprintf(path, sizeof(path), "%s/%s", root, name);
  FILE *file = fopen(path, "wb");
  check_not_null(file);
  check_equal(fwrite(data, 1, bytes, file), bytes);
  check_equal(fclose(file), 0);
}

static void put(const char *name, const char *text) {
  put_bytes(name, text, strlen(text));
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
  check_equal(closedir(dir), 0);
  check_equal(rmdir(path), 0);
}

static void setup(void) {
  strcpy(root, "./fsx-native-discovery-XXXXXX");
  check_not_null(mkdtemp(root));
  char *resolved = realpath(root, NULL);
  check_not_null(resolved);
  check_true(strlen(resolved) < sizeof(absolute_root));
  strcpy(absolute_root, resolved);
  free(resolved);
  engine = NULL;
  query = NULL;
  batch = held = NULL;
  FsEngineConfig config = fsx_engine_config(":memory:");
  check_equal(fsx_engine_open(&config, &engine), FS_OK);
}

static void teardown(void) {
  fsx_content_batch_release(held);
  held = NULL;
  fsx_content_batch_release(batch);
  batch = NULL;
  fsx_content_release(query);
  query = NULL;
  if (engine)
    check_equal(fsx_engine_close(&engine, UINT64_MAX), FS_OK);
  remove_tree(root);
}

static FsContentRequest request(void) {
  FsContentRequest r = fsx_content_native_live_request(absolute_root, "needle");
  /* Keep each file small so these cases exercise discovery, not matcher limits. */
  r.max_output_bytes = 1024;
  r.max_records = 512;
  r.max_file_bytes = 4096;
  return r;
}

static FsContentInfo finish(void) {
  uint64_t end = cmeta_hrtime() + UINT64_C(5000000000);
  FsContentInfo info;
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

static FsContentInfo collect(uint64_t *records) {
  uint64_t end = cmeta_hrtime() + UINT64_C(10000000000);
  FsContentInfo info;
  *records = 0;
  for (;;) {
    fsx_content_wait(query, end);
    FsStatus status;
    while ((status = fsx_content_batch_acquire(query, &batch)) == FS_OK) {
      FsContentFileView file;
      check_equal(fsx_content_batch_info(batch, &file), FS_OK);
      *records += file.record_count;
      check_true(file.read_end_ns >= file.read_start_ns);
      fsx_content_batch_release(batch);
      batch = NULL;
    }
    check_true(status == FS_END || status == FS_WAIT);
    fsx_content_poll(query, &info);
    if (info.finished && status == FS_END)
      break;
    check_true(cmeta_hrtime() < end);
  }
  check_equal(info.match_records, *records);
  check_equal(info.processes_started, UINT64_C(0));
  check_equal(info.discovery_processes_started, UINT64_C(0));
  check_equal(info.running_processes, 0u);
  check_equal(info.queued_candidates, 0u);
  check_equal(info.queued_path_bytes, 0u);
  return info;
}

static void release_query(void) {
  fsx_content_release(query);
  query = NULL;
}

static void wait_idle(void) {
  uint64_t end = cmeta_hrtime() + UINT64_C(5000000000);
  for (;;) {
    cmeta_mutex_lock(&engine->mutex);
    bool idle = engine->active_jobs == 0;
    cmeta_mutex_unlock(&engine->mutex);
    if (idle)
      return;
    check_true(cmeta_hrtime() < end);
    cmeta_sleep_ms(1);
  }
}

static void wait_owned(uint64_t expected) {
  uint64_t end = cmeta_hrtime() + UINT64_C(5000000000);
  FsProgress progress;
  for (;;) {
    check_equal(fsx_engine_progress(engine, &progress), FS_OK);
    if (progress.owned_bytes == expected)
      return;
    check_true(cmeta_hrtime() < end);
    cmeta_sleep_ms(1);
  }
}

suite("Embedded POSIX bfs discovery") {
  before_each() { setup(); }
  after_each() { teardown(); }

  it("discovers unindexed hidden and UTF8 newline paths with native identities") {
    put("-e [猫]\n.txt", "needle\n");
    put(".hidden", "needle\n");
    char sub[1600];
    snprintf(sub, sizeof(sub), "%s/sub", root);
    check_equal(mkdir(sub, 0700), 0);
    put("sub/nested", "needle\n");
    FsContentRequest r = request();
    check_equal(fsx_content_submit(engine, &r, &query), FS_OK);
    /* The request is copied before the producer starts. */
    memset(absolute_root, 'x', strlen(absolute_root));
    unsigned seen = 0;
    for (unsigned i = 0; i < 3; i++) {
      acquire();
      FsContentFileView file;
      check_equal(fsx_content_batch_info(batch, &file), FS_OK);
      check_equal(file.entry_key, UINT64_C(0));
      check_equal(file.metadata_epoch, UINT64_C(0));
      check_equal(file.key.source_id, 0u);
      check_true(file.key.file_id_lo != 0);
      seen |= strstr(file.path, "-e [猫]\n.txt") ? 1u
              : strstr(file.path, ".hidden") ? 2u : 4u;
      fsx_content_batch_release(batch);
      batch = NULL;
    }
    check_equal(seen, 7u);
    FsContentInfo info = finish();
    check_true(info.complete);
    check_true(info.candidate_count_known);
    check_equal(info.candidate_count, UINT64_C(3));
    check_equal(info.processes_started, UINT64_C(0));
    FsProgress progress;
    check_equal(fsx_engine_progress(engine, &progress), FS_OK);
    check_equal(progress.entries, UINT64_C(0));
    check_equal(progress.snapshot_epoch, UINT64_C(0));
  }

  it("preserves hardlink aliases and excludes symlinks and special files") {
    put("a.txt", "needle\n");
    char a[1600], other[1600];
    snprintf(a, sizeof(a), "%s/a.txt", root);
    snprintf(other, sizeof(other), "%s/b.txt", root);
    check_equal(link(a, other), 0);
    snprintf(other, sizeof(other), "%s/link-file", root);
    check_equal(symlink("a.txt", other), 0);
    snprintf(other, sizeof(other), "%s/link-dir", root);
    check_equal(symlink(".", other), 0);
    snprintf(other, sizeof(other), "%s/fifo", root);
    check_equal(mkfifo(other, 0600), 0);
    FsContentRequest r = request();
    check_equal(fsx_content_submit(engine, &r, &query), FS_OK);
    acquire();
    FsContentFileView first, second;
    check_equal(fsx_content_batch_info(batch, &first), FS_OK);
    uint64_t inode = first.key.file_id_lo;
    char saved[1800];
    strcpy(saved, first.path);
    fsx_content_batch_release(batch);
    batch = NULL;
    acquire();
    check_equal(fsx_content_batch_info(batch, &second), FS_OK);
    check_equal(second.key.file_id_lo, inode);
    check_true(strcmp(saved, second.path) != 0);
    check_true(finish().complete);
    release_query();
    check_equal(fsx_content_batch_info(batch, &second), FS_OK);
    check_not_null(strstr(second.path, ".txt"));
    FsContentRecordView record;
    check_equal(fsx_content_batch_record(batch, 0, &record), FS_OK);
    check_equal(record.text, "needle");
  }

  it("accounts for invalid UTF8 names and binary files without inventing paths") {
#ifdef __APPLE__
    char invalid[1600];
    snprintf(invalid, sizeof(invalid), "%s/invalid-\xff", root);
    FILE *rejected = fopen(invalid, "wb");
    check_null(rejected);
    check_equal(errno, EILSEQ);
#else
    put("invalid-\xff", "needle\n");
#endif
    put_bytes("binary", "needle\0", 7);
    FsContentRequest r = request();
    check_equal(fsx_content_submit(engine, &r, &query), FS_OK);
    FsContentInfo info = finish();
    check_equal(info.status, FS_OK);
    check_false(info.complete);
    check_true(info.candidate_count_known);
#ifdef __APPLE__
    check_equal(info.skipped_files, UINT64_C(1));
#else
    check_equal(info.skipped_files, UINT64_C(2));
#endif
    check_equal(info.candidate_count, info.skipped_files);
    check_equal(info.match_records, UINT64_C(0));
  }

  it("counts long candidate paths without a pipe framing boundary") {
    uint64_t path_bytes = 0;
    for (unsigned i = 0; i < 80; i++) {
      char name[200];
      memset(name, 'x', 150);
      snprintf(name + 150, sizeof(name) - 150, "%u.txt", i);
      put(name, "absent\n");
      path_bytes += strlen(absolute_root) + 1 + strlen(name) + 1;
    }
    FsContentRequest r = request();
    r.parallel_files = 2;
    check_equal(fsx_content_submit(engine, &r, &query), FS_OK);
    uint64_t records;
    FsContentInfo info = collect(&records);
    check_true(info.complete);
    check_equal(info.candidate_count, UINT64_C(80));
    check_equal(info.files_scanned, UINT64_C(80));
    check_equal(info.discovery_bytes, path_bytes);
    check_true(info.discovery_bytes > 8192);
  }

  it("fails at the candidate limit and preserves earlier confirmed batches") {
    put("a.txt", "needle\n");
    put("b.txt", "needle\n");
    FsContentRequest r = request();
    r.max_candidates = 1;
    check_equal(fsx_content_submit(engine, &r, &query), FS_OK);
    uint64_t records;
    FsContentInfo info = collect(&records);
    check_equal(info.status, FS_LIMIT);
    check_false(info.complete);
    check_false(info.candidate_count_known);
    check_equal(info.candidate_count, UINT64_C(1));
    check_equal(records, UINT64_C(1));
  }

  it("counts the terminating NUL against the native discovery byte budget") {
    put("file.txt", "absent\n");
    FsContentRequest r = request();
    uint64_t bytes = strlen(absolute_root) + 1 + strlen("file.txt") + 1;
    r.max_discovery_bytes = bytes - 1;
    check_equal(fsx_content_submit(engine, &r, &query), FS_OK);
    FsContentInfo info = finish();
    check_equal(info.status, FS_LIMIT);
    check_false(info.candidate_count_known);
    check_equal(info.candidate_count, UINT64_C(0));
    release_query();
    r.max_discovery_bytes = bytes;
    check_equal(fsx_content_submit(engine, &r, &query), FS_OK);
    info = finish();
    check_true(info.complete);
    check_equal(info.candidate_count, UINT64_C(1));
    check_equal(info.discovery_bytes, bytes);
  }

  it("owns queued paths and retains batches while cancelling a full queue") {
    for (unsigned i = 0; i < 140; i++) {
      char name[40];
      snprintf(name, sizeof(name), "item-%03u.txt", i);
      put(name, "needle\n");
    }
    FsContentRequest r = request();
    r.parallel_files = 2;
    check_equal(fsx_content_submit(engine, &r, &query), FS_OK);
    acquire();
    held = batch;
    batch = NULL;
    acquire();
    uint64_t end = cmeta_hrtime() + UINT64_C(5000000000);
    FsContentInfo info;
    do {
      fsx_content_poll(query, &info);
      check_false(info.finished);
      check_true(info.queued_candidates <= 128);
      check_true(cmeta_hrtime() < end);
      cmeta_sleep_ms(1);
    } while (info.queued_candidates != 128);
    check_true(info.queued_path_bytes > 0);
    fsx_content_cancel(query);
    info = finish();
    check_equal(info.status, FS_CANCELLED);
    check_false(info.candidate_count_known);
    check_equal(info.queued_candidates, 0u);
    check_equal(info.queued_path_bytes, 0u);
    check_equal(info.processes_started, UINT64_C(0));
    release_query();
    FsContentFileView file;
    check_equal(fsx_content_batch_info(held, &file), FS_OK);
    check_not_null(strstr(file.path, "item-"));
    FsContentRecordView record;
    check_equal(fsx_content_batch_record(batch, 0, &record), FS_OK);
    check_equal(record.text, "needle");
  }

  it("rejects a replaced root after a confirmed batch") {
    put("a.txt", "needle\n");
    put("b.txt", "needle\n");
    FsContentRequest r = request();
    check_equal(fsx_content_submit(engine, &r, &query), FS_OK);
    acquire();
    char parked[1500];
    snprintf(parked, sizeof(parked), "%s-parked", root);
    check_equal(rename(root, parked), 0);
    check_equal(mkdir(root, 0700), 0);
    put("replacement", "needle\n");
    fsx_content_batch_release(batch);
    batch = NULL;
    FsContentInfo info = finish();
    check_equal(info.status, FS_STALE);
    check_false(info.complete);
    check_false(info.candidate_count_known);
    check_equal(info.files_scanned, UINT64_C(1));
    remove_tree(root);
    check_equal(rename(parked, root), 0);
  }

  it("honors a deadline before native discovery and reports missing roots") {
    FsContentRequest r = request();
    r.deadline_ns = 0;
    check_equal(fsx_content_submit(engine, &r, &query), FS_OK);
    FsContentInfo info = finish();
    check_equal(info.status, FS_TIMEOUT);
    check_false(info.candidate_count_known);
    check_equal(info.discovery_bytes, UINT64_C(0));
    release_query();
    char missing[1600];
    snprintf(missing, sizeof(missing), "%s/missing", absolute_root);
    r = fsx_content_native_live_request(missing, "needle");
    check_equal(fsx_content_submit(engine, &r, &query), FS_OK);
    info = finish();
    check_equal(info.status, FS_STALE);
    check_false(info.complete);
    check_false(info.candidate_count_known);
  }

  it("charges walker allocations to the engine budget and releases them on failure") {
    /* Prime the content lifecycle, then hold the engine quiescent while setting
     * a small managed-memory budget. Directories alone isolate walker storage
     * from per-file matching/output storage. */
    FsContentRequest r = request();
    check_equal(fsx_content_submit(engine, &r, &query), FS_OK);
    check_true(finish().complete);
    release_query();
    wait_idle();
    FsProgress before;
    check_equal(fsx_engine_progress(engine, &before), FS_OK);
    for (unsigned i = 0; i < 512; i++) {
      char path[1800], name[150];
      memset(name, 'd', 128);
      snprintf(name + 128, sizeof(name) - 128, "%u", i);
      snprintf(path, sizeof(path), "%s/%s", root, name);
      check_equal(mkdir(path, 0700), 0);
    }
    uint64_t budget = engine->config.owned_memory_budget;
    engine->config.owned_memory_budget = before.owned_bytes + 32768;
    check_equal(fsx_content_submit(engine, &r, &query), FS_OK);
    FsContentInfo info = finish();
    check_equal(info.status, FS_NOMEM);
    check_false(info.complete);
    check_false(info.candidate_count_known);
    check_equal(info.candidate_count, UINT64_C(0));
    release_query();
    wait_idle();
    engine->config.owned_memory_budget = budget;
    wait_owned(before.owned_bytes);
    check_equal(fsx_content_submit(engine, &r, &query), FS_OK);
    info = finish();
    check_true(info.complete);
    check_equal(info.candidate_count, UINT64_C(0));
    release_query();
    wait_owned(before.owned_bytes);
  }
}

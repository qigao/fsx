#ifdef __APPLE__
#define _DARWIN_C_SOURCE
#endif
#ifndef __APPLE__
#define _GNU_SOURCE
#endif
#define _XOPEN_SOURCE 700
#include "../src/content/content.h"
#include <clock.h>
#include <inttypes.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <thread.h>
#include <tinytest.h>
#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#define make_dir(p) _mkdir(p)
#define remove_dir(p) _rmdir(p)
#else
#include <errno.h>
#include <fcntl.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#define make_dir(p) mkdir(p, 0700)
#define remove_dir(p) rmdir(p)
#ifdef __APPLE__
#include <sys/mount.h>
#else
#include <sys/vfs.h>
#endif
#endif

enum { QUERY_CAPACITY = 8192, SMALL_FILES = 64, LARGE_FILES = 4 };
typedef struct Sample {
  uint64_t start, end, epoch, matches;
  FsStatus status;
} Sample;
typedef struct Monitor {
  cmeta_thread_t thread;
  atomic_bool stop, ready;
  unsigned count;
  Sample samples[QUERY_CAPACITY];
} Monitor;
typedef struct Corpus {
  const char *name;
  unsigned files, bytes, stride;
} Corpus;
static const Corpus corpora[] = {
    {"small", SMALL_FILES, 1024, 64},
    {"large", LARGE_FILES, 4 * 1024 * 1024, 65536}};
static FsEngine *engine;
static FsContentQuery *query;
static FsContentBatch *batch;
static Monitor *monitor;
static bool monitoring;
static FILE *measurements, *query_measurements;
static char fixture[160], root[4096];
static const Corpus *current;
static uint64_t input_hash, scan_ns, epoch;
static unsigned operation, created;

static uint64_t hash_bytes(uint64_t hash, const void *bytes, size_t n) {
  const unsigned char *p = bytes;
  for (size_t i = 0; i < n; i++)
    hash = (hash ^ p[i]) * UINT64_C(1099511628211);
  return hash;
}
static void line_bytes(char line[64], unsigned offset) {
  memset(line, 'x', 63);
  if (offset % current->stride == 0) {
    const char *prefix = offset == 0 || current->stride > 64
                             ? "\xe7\x8c\xab needle anchor "
                             : "\xe7\x8c\xab needle ";
    memcpy(line, prefix, strlen(prefix));
  }
  line[63] = '\n';
}
static void file_path(char path[256], unsigned file) {
  snprintf(path, 256, "%s/file-%03u.txt", fixture, file);
}
static void filesystem(FILE *out) {
#ifdef _WIN32
  wchar_t cwd[4096], volume[4096], name[100];
  check_true(GetCurrentDirectoryW(4096, cwd) > 0);
  check_true(GetVolumePathNameW(cwd, volume, 4096));
  check_true(
      GetVolumeInformationW(volume, NULL, 0, NULL, NULL, NULL, name, 100));
  char text[400];
  check_true(WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, name, -1, text,
                                 sizeof(text), NULL, NULL) > 0);
  fprintf(out, "# filesystem=%s\n", text);
#else
  struct statfs info;
  check_equal(statfs(".", &info), 0);
#ifdef __APPLE__
  fprintf(out, "# filesystem=%s\n", info.f_fstypename);
#else
  fprintf(out, "# filesystem_magic=0x%lx\n", (unsigned long)info.f_type);
#endif
#endif
}
static void query_worker(void *context) {
  Monitor *m = context;
  while (!atomic_load(&m->stop) && m->count < QUERY_CAPACITY) {
    Sample *s = &m->samples[m->count++];
    FsQuery *selected = NULL;
    FsResult *result = NULL;
    FsQueryOptions options = fsx_query_options();
    options.limit = 1;
    FsQueryRequest r = {sizeof(r), FSX_API_VERSION, "file-", 5, options};
    FsResultInfo info = {0};
    s->start = cmeta_hrtime();
    s->status = fsx_query_submit(engine, &r, &selected);
    if (s->status == FS_OK)
      s->status = fsx_query_wait(selected, s->start + UINT64_C(10000000000));
    if (s->status == FS_OK)
      s->status = fsx_query_result_acquire(selected, &result);
    if (s->status == FS_OK)
      s->status = fsx_result_info(result, &info);
    s->end = cmeta_hrtime();
    s->epoch = info.snapshot_epoch;
    s->matches = info.total_count;
    fsx_result_release(result);
    fsx_query_release(selected);
    atomic_store(&m->ready, true);
    cmeta_sleep_ms(1);
  }
}
static void monitor_start(void) {
  monitor->count = 0;
  atomic_init(&monitor->stop, false);
  atomic_init(&monitor->ready, false);
  check_equal(cmeta_thread_create(&monitor->thread, query_worker, monitor), 0);
  monitoring = true;
  while (!atomic_load(&monitor->ready))
    cmeta_sleep_ms(1);
}
static void monitor_stop(void) {
  atomic_store(&monitor->stop, true);
  check_equal(cmeta_thread_join(&monitor->thread), 0);
  cmeta_thread_destroy(&monitor->thread);
  monitoring = false;
}
static void queries_report(void) {
  check_true(monitor->count > 0);
  check_true(monitor->count < QUERY_CAPACITY);
  for (unsigned i = 0; i < monitor->count; i++) {
    Sample *s = &monitor->samples[i];
    check_equal(s->status, FS_OK);
    check_equal(s->epoch, epoch);
    check_equal(s->matches, (uint64_t)current->files);
    fprintf(query_measurements,
            "%u,%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%d\n",
            operation, s->start, s->end, s->epoch, s->matches, (int)s->status);
  }
  check_equal(fflush(query_measurements), 0);
}
static void setup(void) {
  engine = NULL;
  query = NULL;
  batch = NULL;
  monitoring = false;
  current = NULL;
  created = operation = 0;
  fixture[0] = root[0] = 0;
  monitor = calloc(1, sizeof(*monitor));
  check_not_null(monitor);
  measurements = fopen("content-benchmark.csv", "wb");
  query_measurements = fopen("content-benchmark-queries.csv", "wb");
  check_not_null(measurements);
  check_not_null(query_measurements);
  fprintf(measurements,
          "# format=fsx-content-benchmark-v2\n# build_type=%s\n# platform=%s\n"
          "# compiler=%s\n# sanitizers=%s\n# benchmark_source_sha256=%s\n"
          "# cache=warm-uncontrolled-os-cache\n"
          "# ugrep=%s\n",
          FSX_TEST_BUILD_TYPE, FSX_TEST_PLATFORM, FSX_TEST_COMPILER,
          FSX_TEST_SANITIZERS, FSX_TEST_SOURCE_SHA256, FSX_TEST_UGREP);
#ifndef _WIN32
  fprintf(measurements, "# bfs=%s\n", FSX_TEST_BFS);
#endif
  filesystem(measurements);
  fputs("operation,corpus,source,pattern,phase,round,position,parallel_files,"
        "files,"
        "bytes,input_hash,scan_ns,metadata_epoch,start_ns,end_ns,submit_ns,"
        "first_batch_ns,cancel_ns,records,files_scanned,candidates,processes,"
        "running_sampled_peak,queued_sampled_peak,owned_base,owned_sampled_"
        "peak,status,spawn_call_ns,child_lifetime_ns,collect_decode_ns\n",
        measurements);
  fputs("operation,start_ns,end_ns,metadata_epoch,matches,status\n",
        query_measurements);
}
static void clear_corpus(void) {
  if (engine)
    check_equal(fsx_engine_close(&engine, UINT64_MAX), FS_OK);
  for (unsigned i = 0; i < created; i++) {
    char path[256];
    file_path(path, i);
    check_equal(remove(path), 0);
  }
  created = 0;
  if (fixture[0]) {
    check_equal(remove_dir(fixture), 0);
    fixture[0] = 0;
  }
}
static void teardown(void) {
  fsx_content_batch_release(batch);
  batch = NULL;
  fsx_content_release(query);
  query = NULL;
  if (monitoring)
    monitor_stop();
  clear_corpus();
  if (measurements)
    fclose(measurements);
  if (query_measurements)
    fclose(query_measurements);
  measurements = query_measurements = NULL;
  free(monitor);
  monitor = NULL;
}
static void build_corpus(const Corpus *corpus) {
  current = corpus;
#ifdef _WIN32
  snprintf(fixture, sizeof(fixture), "content-bench-%lu-%" PRIu64,
           (unsigned long)GetCurrentProcessId(), cmeta_hrtime());
  check_equal(make_dir(fixture), 0);
  wchar_t relative[200], absolute[4096];
  check_true(MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, fixture, -1,
                                 relative, 200) > 0);
  check_true(GetFullPathNameW(relative, 4096, absolute, NULL) > 0);
  check_true(WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, absolute, -1,
                                 root, sizeof(root), NULL, NULL) > 0);
#else
  strcpy(fixture, "content-bench-XXXXXX");
  check_not_null(mkdtemp(fixture));
  check_not_null(realpath(fixture, root));
#endif
  input_hash = UINT64_C(14695981039346656037);
  char block[65536];
  for (unsigned offset = 0; offset < sizeof(block); offset += 64)
    line_bytes(block + offset, offset);
  for (unsigned i = 0; i < corpus->files; i++) {
    char path[256];
    file_path(path, i);
    FILE *file = fopen(path, "wb");
    check_not_null(file);
    created++;
    for (unsigned offset = 0; offset < corpus->bytes;) {
      unsigned n = corpus->bytes - offset;
      if (n > sizeof(block))
        n = sizeof(block);
      check_equal(fwrite(block, 1, n, file), n);
      input_hash = hash_bytes(input_hash, block, n);
      offset += n;
    }
    check_equal(fclose(file), 0);
  }
  FsEngineConfig config = fsx_engine_config(":memory:");
  check_equal(fsx_engine_open(&config, &engine), FS_OK);
  FsScanInfo info;
  uint64_t start = cmeta_hrtime();
  check_equal(fsx_engine_scan(engine, root, &info), FS_OK);
  scan_ns = cmeta_hrtime() - start;
  check_true(info.coverage_complete);
  epoch = info.snapshot_epoch;
}
static unsigned matches_per_file(const char *pattern) {
  return !strcmp(pattern, "absent") ? 0
         : !strcmp(pattern, "anchor") && current->stride == 64
             ? 1
             : current->bytes / current->stride;
}
static bool records_match(const FsContentBatch *result, const char *pattern) {
  unsigned count = matches_per_file(pattern);
  if (result->info.record_count != count)
    return false;
  for (unsigned i = 0; i < count; i++) {
    unsigned offset = i * current->stride;
    char text[64];
    line_bytes(text, offset);
    FsContentRecordView record;
    if (fsx_content_batch_record(result, i, &record) != FS_OK ||
        record.line != (uint64_t)(offset / 64 + 1) ||
        record.byte_offset !=
            (uint64_t)offset + (!strcmp(pattern, "needle") ? 4 : 11) ||
        record.text_bytes != 63 || memcmp(record.text, text, 63))
      return false;
  }
  return true;
}
#ifndef _WIN32
/* Immutable-fixture control only. ugrep opens the selected pathname itself;
 * regular temporary output files avoid a pipe pump and waitpid blocks. These
 * semantics deliberately omit FSX admission, identity/text/post-read checks,
 * consumer backpressure and cooperative cancellation. No production backend. */
typedef struct Direct {
  const char *pattern;
  atomic_uint next, done, started, running, peak;
  atomic_int status;
  atomic_uint_fast64_t records, first, spawn_ns, child_ns, collect_ns;
} Direct;
static FsStatus direct_file(Direct *job, unsigned id) {
  FILE *input = fopen("/dev/null", "rb");
  FILE *output = tmpfile(), *errors = tmpfile();
  FsStatus status = FS_IO;
  FsContentBatch result = {0};
  result.wire = fsx_alloc(engine, 65537);
  result.records = fsx_alloc(engine, 10000 * sizeof(*result.records));
  if (!result.wire || !result.records) {
    status = FS_NOMEM;
    goto cleanup;
  }
  if (!input || !output || !errors)
    goto cleanup;
  char path[256];
  file_path(path, id);
  char *argv[] = {FSX_TEST_UGREP,
                  "-J1",
                  "-F",
                  "--color=never",
                  "--line-buffered",
                  "--label=fsx-input",
                  "--format={\"line\":%n,\"offset\":%b,\"text\":%J}%~",
                  "-e",
                  (char *)job->pattern,
                  "--",
                  path,
                  NULL};
  posix_spawn_file_actions_t actions;
  if (posix_spawn_file_actions_init(&actions))
    goto cleanup;
  posix_spawnattr_t attributes;
  if (posix_spawnattr_init(&attributes)) {
    posix_spawn_file_actions_destroy(&actions);
    goto cleanup;
  }
  int rc = posix_spawn_file_actions_adddup2(&actions, fileno(input), 0);
  rc |= posix_spawn_file_actions_adddup2(&actions, fileno(output), 1);
  rc |= posix_spawn_file_actions_adddup2(&actions, fileno(errors), 2);
#ifdef __APPLE__
  rc |= posix_spawnattr_setflags(&attributes, POSIX_SPAWN_CLOEXEC_DEFAULT);
#else
  rc |= posix_spawn_file_actions_addclosefrom_np(&actions, 3);
#endif
  char *environment[] = {"LANG=C", "LC_ALL=C", NULL};
  pid_t pid;
  uint64_t start = cmeta_hrtime();
  if (!rc)
    rc = posix_spawn(&pid, FSX_TEST_UGREP, &actions, &attributes, argv,
                     environment);
  uint64_t spawned = cmeta_hrtime();
  posix_spawnattr_destroy(&attributes);
  posix_spawn_file_actions_destroy(&actions);
  if (rc)
    goto cleanup;
  atomic_fetch_add(&job->started, 1);
  unsigned running = atomic_fetch_add(&job->running, 1) + 1;
  unsigned peak = atomic_load(&job->peak);
  while (running > peak &&
         !atomic_compare_exchange_weak(&job->peak, &peak, running)) {
  }
  int exit_status;
  pid_t waited;
  do {
    waited = waitpid(pid, &exit_status, 0);
  } while (waited < 0 && errno == EINTR);
  uint64_t reaped = cmeta_hrtime();
  atomic_fetch_sub(&job->running, 1);
  atomic_fetch_add(&job->spawn_ns, spawned - start);
  atomic_fetch_add(&job->child_ns, reaped - start);
  if (waited != pid || !WIFEXITED(exit_status) || WEXITSTATUS(exit_status) > 1)
    goto cleanup;
  if (fseek(output, 0, SEEK_SET) || fseek(errors, 0, SEEK_SET))
    goto cleanup;
  result.used = fread(result.wire, 1, 65537, output);
  if (result.used > 65536) {
    status = FS_LIMIT;
    goto cleanup;
  }
  if (ferror(output) || fgetc(errors) != EOF || ferror(errors) ||
      (WEXITSTATUS(exit_status) == 1) != (result.used == 0))
    goto cleanup;
  status = fsx_content_decode(&result, 10000);
  if (status == FS_OK && !records_match(&result, job->pattern))
    status = FS_CORRUPT;
  uint64_t checked = cmeta_hrtime();
  atomic_fetch_add(&job->collect_ns, checked - reaped);
  if (status == FS_OK) {
    atomic_fetch_add(&job->records, result.info.record_count);
    if (result.info.record_count) {
      uint_fast64_t first = atomic_load(&job->first);
      while ((!first || checked < first) &&
             !atomic_compare_exchange_weak(&job->first, &first, checked)) {
      }
    }
  }
cleanup:
  fsx_free(result.wire);
  fsx_free(result.records);
  if (input)
    fclose(input);
  if (output)
    fclose(output);
  if (errors)
    fclose(errors);
  return status;
}
static void direct_worker(void *context) {
  Direct *job = context;
  while (atomic_load(&job->status) == FS_OK) {
    unsigned id = atomic_fetch_add(&job->next, 1);
    if (id >= current->files)
      break;
    FsStatus status = direct_file(job, id);
    if (status != FS_OK) {
      int expected = FS_OK;
      atomic_compare_exchange_strong(&job->status, &expected, status);
      break;
    }
  }
  atomic_fetch_add(&job->done, 1);
}
static FsStatus direct_start(Direct *job, cmeta_thread_t workers[2],
                             unsigned *started, const char *pattern,
                             unsigned parallel) {
  job->pattern = pattern;
  atomic_init(&job->next, 0);
  atomic_init(&job->done, 0);
  atomic_init(&job->started, 0);
  atomic_init(&job->running, 0);
  atomic_init(&job->peak, 0);
  atomic_init(&job->status, FS_OK);
  atomic_init(&job->records, 0);
  atomic_init(&job->first, 0);
  atomic_init(&job->spawn_ns, 0);
  atomic_init(&job->child_ns, 0);
  atomic_init(&job->collect_ns, 0);
  *started = 0;
  while (*started < parallel) {
    if (cmeta_thread_create(&workers[*started], direct_worker, job)) {
      atomic_store(&job->status, FS_NOMEM);
      return FS_NOMEM;
    }
    (*started)++;
  }
  return FS_OK;
}
#endif
static void validate_batch(const char *pattern, bool seen[SMALL_FILES],
                           uint64_t *records) {
  FsContentFileView file;
  check_equal(fsx_content_batch_info(batch, &file), FS_OK);
  const char *name = strrchr(file.path, '/');
#ifdef _WIN32
  const char *backslash = strrchr(file.path, '\\');
  if (!name || (backslash && backslash > name))
    name = backslash;
#endif
  check_not_null(name);
  unsigned id;
  char extra;
  check_equal(sscanf(name + 1, "file-%u.txt%c", &id, &extra), 1);
  check_true(id < current->files);
  check_false(seen[id]);
  seen[id] = true;
  check_true(records_match(batch, pattern));
  *records += file.record_count;
}

static void run(const char *source, const char *pattern, const char *phase,
                unsigned round, unsigned position, unsigned parallel) {
  operation++;
  bool baseline = !strcmp(phase, "baseline"), cancel = !strcmp(phase, "cancel");
  FsContentRequest r =
      fsx_content_scope_request("file:", FSX_TEST_UGREP, pattern);
#ifndef _WIN32
  if (!strcmp(source, "live"))
    r = fsx_content_live_request(root, FSX_TEST_BFS, FSX_TEST_UGREP, pattern);
#endif
  r.parallel_files = parallel;
  r.max_output_bytes = 65536;
  r.deadline_ns = cmeta_hrtime() + UINT64_C(60000000000);
  FsProgress progress;
  check_equal(fsx_engine_progress(engine, &progress), FS_OK);
  uint64_t owned_base = progress.owned_bytes, peak = owned_base;
  monitor_start();
  uint64_t start = cmeta_hrtime(), submitted = 0, first = 0, cancelled = 0;
  uint64_t records = 0, end;
  unsigned running_peak = 0, queued_peak = 0;
  uint64_t spawn_ns = 0, child_ns = 0, collect_ns = 0;
  FsContentInfo info = {0};
  bool seen[SMALL_FILES] = {0};
  if (baseline) {
    cmeta_sleep_ms(50);
    end = cmeta_hrtime();
  }
#ifndef _WIN32
  else if (!strcmp(source, "direct")) {
    Direct job;
    cmeta_thread_t workers[2] = {0};
    unsigned started;
    FsStatus admitted =
        direct_start(&job, workers, &started, pattern, parallel);
    submitted = cmeta_hrtime() - start;
    while (atomic_load(&job.done) < started) {
      check_equal(fsx_engine_progress(engine, &progress), FS_OK);
      if (progress.owned_bytes > peak)
        peak = progress.owned_bytes;
      cmeta_sleep_ms(1);
    }
    for (unsigned i = 0; i < started; i++) {
      cmeta_thread_join(&workers[i]);
      cmeta_thread_destroy(&workers[i]);
    }
    end = cmeta_hrtime();
    check_equal(admitted, FS_OK);
    check_equal(atomic_load(&job.status), FS_OK);
    check_equal(atomic_load(&job.running), 0u);
    check_equal(atomic_load(&job.started), current->files);
    records = atomic_load(&job.records);
    check_equal(records, (uint64_t)current->files * matches_per_file(pattern));
    first = atomic_load(&job.first);
    if (first)
      first -= start;
    running_peak = atomic_load(&job.peak);
    spawn_ns = atomic_load(&job.spawn_ns);
    child_ns = atomic_load(&job.child_ns);
    collect_ns = atomic_load(&job.collect_ns);
    info.status = FS_OK;
    info.files_scanned = info.candidate_count = info.processes_started =
        current->files;
  }
#endif
  else {
    check_equal(fsx_content_submit(engine, &r, &query), FS_OK);
    submitted = cmeta_hrtime() - start;
    for (;;) {
      fsx_content_wait(query, cmeta_hrtime() + UINT64_C(1000000));
      FsStatus acquired;
      while ((acquired = fsx_content_batch_acquire(query, &batch)) == FS_OK) {
        if (!first)
          first = cmeta_hrtime() - start;
        validate_batch(pattern, seen, &records);
        if (cancel && !cancelled) {
          cancelled = cmeta_hrtime();
          fsx_content_cancel(query);
        }
        fsx_content_batch_release(batch);
        batch = NULL;
      }
      check_true(acquired == FS_END || acquired == FS_WAIT);
      fsx_content_poll(query, &info);
      check_equal(fsx_engine_progress(engine, &progress), FS_OK);
      if (progress.owned_bytes > peak)
        peak = progress.owned_bytes;
      if (info.running_processes > running_peak)
        running_peak = info.running_processes;
      if (info.queued_candidates > queued_peak)
        queued_peak = info.queued_candidates;
      check_true(info.running_processes <=
                 parallel + (!strcmp(source, "live") ? 1u : 0u));
      check_true(info.queued_candidates <= 128);
      check_true(info.queued_path_bytes <= 1024 * 1024);
      if (info.finished && acquired == FS_END)
        break;
      check_true(cmeta_hrtime() < r.deadline_ns);
    }
    end = cmeta_hrtime();
    check_equal(info.status, cancel ? FS_CANCELLED : FS_OK);
    check_equal(info.complete, !cancel);
    check_equal(info.running_processes, 0u);
    check_equal(info.queued_candidates, 0u);
    check_equal(info.queued_path_bytes, 0u);
    check_equal(info.match_records, records);
    if (!cancel) {
      check_true(info.candidate_count_known);
      check_equal(info.candidate_count, (uint64_t)current->files);
      check_equal(info.files_scanned, (uint64_t)current->files);
      check_equal(info.processes_started,
                  (uint64_t)current->files + (!strcmp(source, "live") ? 1 : 0));
      check_equal(records,
                  (uint64_t)current->files * matches_per_file(pattern));
    } else {
      check_true(cancelled > 0);
      check_true(records > 0);
    }
    fsx_content_release(query);
    query = NULL;
  }
  monitor_stop();
  queries_report();
  fprintf(
      measurements,
      "%u,%s,%s,%s,%s,%u,%u,%u,%u,%" PRIu64 ",%016" PRIx64 ",%" PRIu64
      ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64
      ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%u,%u,%" PRIu64
      ",%" PRIu64 ",%d,%" PRIu64 ",%" PRIu64 ",%" PRIu64 "\n",
      operation, current->name, source, pattern, phase, round, position,
      parallel, current->files, (uint64_t)current->files * current->bytes,
      input_hash, scan_ns, epoch, start, end, submitted, first,
      cancelled ? end - cancelled : 0, records, info.files_scanned,
      info.candidate_count, info.processes_started, running_peak, queued_peak,
      owned_base, peak, (int)info.status, spawn_ns, child_ns, collect_ns);
  check_equal(fflush(measurements), 0);
}
suite("Real content throughput and concurrent metadata latency") {
  before_each() { setup(); }
  after_each() { teardown(); }
  it("measures balanced serial and parallel warm-cache jobs on identical UTF8 "
     "input") {
    const char *sources[] = {"scope"
#ifndef _WIN32
                             ,
                             "live"
#endif
    };
    const char *patterns[] = {"anchor", "needle", "absent"};
    for (unsigned c = 0; c < 2; c++) {
      build_corpus(&corpora[c]);
      run("none", "none", "baseline", 0, 0, 0);
      for (unsigned s = 0; s < sizeof(sources) / sizeof(*sources); s++) {
        for (unsigned p = 0; p < 3; p++) {
          if (c && p == 1)
            continue;
          const char *variants[] = {sources[s], sources[s]
#ifndef _WIN32
                                    ,
                                    "direct", "direct"
#endif
          };
          unsigned count = 2;
#ifndef _WIN32
          if (!s)
            count = 4;
#endif
          for (unsigned variant = 0; variant < count; variant++)
            run(variants[variant], patterns[p], "warmup", 0, variant,
                variant % 2 + 1);
          for (unsigned round = 1; round <= 4; round++)
            for (unsigned position = 0; position < count; position++) {
              unsigned variant = (round - 1 + position) % count;
              run(variants[variant], patterns[p], "measure", round, position,
                  variant % 2 + 1);
            }
        }
        for (unsigned mode = 1; mode <= 2; mode++)
          run(sources[s], "anchor", "cancel", 0, mode - 1, mode);
      }
      clear_corpus();
    }
    fprintf(measurements, "# acceptance=passed operations=%u\n", operation);
  }
}

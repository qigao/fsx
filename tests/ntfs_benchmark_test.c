#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#include "ntfs_measure.h"
#include <inttypes.h>
#include <stdlib.h>
#include <tinytest.h>
#ifdef _WIN32
#include <direct.h>
#define getcwd _getcwd
#else
#include <unistd.h>
#endif

/* A modeled, immutable-per-operation provider, not a raw-device benchmark.
 * Each mode regenerates the same directory snapshot and exact V2 USN bytes.
 * The production parser, queue, transaction, FTS and snapshots are exercised.
 */
typedef struct Node {
  uint64_t id, parent, size;
  char name[40];
  unsigned char raw[80];
  uint16_t bytes;
  bool directory;
} Node;
typedef struct Replay {
  FsNtfsIdentity identity;
  Node *nodes;
  unsigned count, directories, first, events;
  int64_t event_usn;
} Replay;
typedef struct Mode {
  const char *name;
  bool async;
  uint32_t queue, pages;
} Mode;
static const Mode modes[] = {{"serial-file", false, 0, 0},
                             {"job-file", true, 0, 0},
                             {"pipeline-file", true, 4, 0},
                             {"pipeline-memory", true, 4, 4096}};
static Replay replay;
static FsEngine *engine;
static FsNtfs *source;
static FsIndexJob *job;
static FsTestQueries *queries;
static FsNtfsMetrics timing;
static FILE *measurements, *query_measurements;
static char database[160], root[2048];
static bool vfs_started, query_running;
static unsigned operation;
static uint64_t expected_inputs[32];
static uint64_t hash_bytes(uint64_t hash, const void *bytes, size_t count) {
  const unsigned char *p = bytes;
  for (size_t i = 0; i < count; i++)
    hash = (hash ^ p[i]) * UINT64_C(1099511628211);
  return hash;
}
static void put16(unsigned char *p, uint16_t n) {
  p[0] = (unsigned char)n;
  p[1] = (unsigned char)(n >> 8);
}
static void put32(unsigned char *p, uint32_t n) {
  for (unsigned i = 0; i < 4; i++)
    p[i] = (unsigned char)(n >> (i * 8));
}
static uint32_t record_bytes(const Node *node) {
  return (60u + node->bytes + 7u) & ~7u;
}
static uint32_t encode(unsigned char *p, const Node *node, int64_t usn) {
  uint32_t bytes = record_bytes(node);
  memset(p, 0, bytes);
  put32(p, bytes);
  put16(p + 4, 2);
  fsx_ntfs_put64(p + 8, node->id);
  fsx_ntfs_put64(p + 16, node->parent);
  fsx_ntfs_put64(p + 24, (uint64_t)usn);
  put32(p + 40, 1); /* DATA_OVERWRITE, metadata-only refresh. */
  put32(p + 52, node->directory ? FSX_NTFS_DIR : 0);
  put16(p + 56, node->bytes);
  put16(p + 58, 60);
  memcpy(p + 60, node->raw, node->bytes);
  return bytes;
}
static FsStatus identity(void *ctx, FsNtfsIdentity *out) {
  *out = ((Replay *)ctx)->identity;
  return FS_OK;
}
static FsStatus read_events(void *ctx, int64_t from, uint64_t journal,
                            void *data, size_t capacity, size_t *bytes) {
  Replay *r = ctx;
  if (journal != r->identity.journal_id || from < r->event_usn)
    return FS_STALE;
  *bytes = 8;
  int64_t cursor = r->event_usn;
  for (unsigned i = 0; i < r->events; i++) {
    Node *n = &r->nodes[r->first + i];
    uint32_t length = record_bytes(n);
    if (cursor >= from) {
      if (*bytes + length > capacity)
        break;
      *bytes += encode((unsigned char *)data + *bytes, n, cursor);
    }
    cursor += length;
  }
  fsx_ntfs_put64(data, (uint64_t)cursor);
  return FS_OK;
}
static FsStatus mft(void *ctx, uint64_t from, void *data, size_t capacity,
                    size_t *bytes) {
  Replay *r = ctx;
  *bytes = 8;
  uint64_t next = from;
  for (unsigned i = 0; i < r->count; i++) {
    Node *n = &r->nodes[i];
    if (n->id < from)
      continue;
    if (*bytes + record_bytes(n) > capacity)
      break;
    *bytes += encode((unsigned char *)data + *bytes, n, 0);
    next = n->id + 1;
  }
  fsx_ntfs_put64(data, next);
  return *bytes > 8 ? FS_OK : FS_END;
}
static void observed(Node *n, FsObserved *out) {
  *out = (FsObserved){
      .name = n->name,
      .raw_name = n->raw,
      .raw_bytes = n->bytes,
      .file_lo = n->id,
      .size = n->size,
      .mtime = 100,
      .flags = FS_ENTRY_MTIME_VALID |
               (n->directory ? FS_ENTRY_DIRECTORY : FS_ENTRY_SIZE_VALID)};
}
static FsStatus list(void *ctx, uint64_t parent, FsObserve visit, void *out) {
  Replay *r = ctx;
  for (unsigned i = 1; i < r->count; i++) {
    Node *n = &r->nodes[i];
    if (n->parent != parent)
      continue;
    FsObserved item;
    observed(n, &item);
    FsStatus status = visit(out, &item);
    if (status != FS_OK)
      return status;
  }
  return FS_OK;
}
static FsStatus stat_node(void *ctx, uint64_t id, FsObserved *out) {
  Replay *r = ctx;
  if (!id || id > r->count)
    return FS_END;
  observed(&r->nodes[id - 1], out);
  return FS_OK;
}
static FsStatus links(void *ctx, uint64_t id, FsNtfsParentFn visit, void *out) {
  Replay *r = ctx;
  return id && id <= r->count ? visit(out, r->nodes[id - 1].parent) : FS_END;
}
static FsStatus inventory(void *ctx, FsNtfsRecordFn visit, void *out,
                          FsNtfsRawStats *stats) {
  Replay *r = ctx;
  *stats = (FsNtfsRawStats){.slots = r->count, .records = r->count};
  for (unsigned i = 0; i <= r->directories; i++) {
    Node *n = &r->nodes[i];
    FsNtfsRecord item = {.frn = n->id,
                         .parent = n->parent,
                         .attributes = FSX_NTFS_DIR,
                         .name = n->raw,
                         .name_bytes = n->bytes};
    FsStatus status = visit(out, &item);
    if (status != FS_OK)
      return status;
  }
  return FS_OK;
}
static uint32_t error(void *ctx) {
  (void)ctx;
  return 0;
}
static void close_provider(void *ctx) { (void)ctx; }
static const FsNtfsOps ops = {.query = identity,
                              .mft = mft,
                              .read = read_events,
                              .list = list,
                              .stat = stat_node,
                              .error = error,
                              .close = close_provider,
                              .links = links,
                              .inventory = inventory};
static void reset_nodes(unsigned dirs, unsigned files) {
  free(replay.nodes);
  replay = (Replay){0};
  replay.directories = dirs;
  replay.count = 1 + dirs * (files + 1);
  replay.nodes = calloc(replay.count, sizeof(Node));
  check_not_null(replay.nodes);
  for (unsigned i = 0; i < replay.count; i++) {
    Node *n = &replay.nodes[i];
    n->id = i + 1;
    n->directory = i <= dirs;
    n->parent = !i          ? UINT64_C(0x100000)
                : i <= dirs ? 1
                            : 2 + (i - dirs - 1) / files;
    n->size = 7;
    snprintf(n->name, sizeof(n->name),
             n->directory ? "dir%05u" : "file%05u.txt", i);
    n->bytes = (uint16_t)(strlen(n->name) * 2);
    for (unsigned j = 0; j < n->bytes / 2; j++)
      put16(n->raw + 2 * j, (unsigned char)n->name[j]);
  }
  replay.identity = (FsNtfsIdentity){.volume_serial = 1234,
                                     .root_frn = 1,
                                     .volume_root_frn = UINT64_C(0x100000),
                                     .journal_id = 5678,
                                     .next_usn = 1024};
  strcpy(replay.identity.volume_guid, "modeled-replay-volume");
  replay.event_usn = 1024;
}
static void event_batch(unsigned count, unsigned sample) {
  replay.first = replay.directories + 1;
  replay.events = count;
  replay.event_usn = replay.identity.next_usn;
  for (unsigned i = 0; i < count; i++) {
    Node *n = &replay.nodes[replay.first + i];
    n->size = 100 + sample;
    replay.identity.next_usn += record_bytes(n);
  }
}
static uint64_t input_hash(void) {
  uint64_t hash = UINT64_C(14695981039346656037);
  unsigned char bytes[256];
  for (unsigned i = 0; i < replay.count; i++) {
    Node *n = &replay.nodes[i];
    hash = hash_bytes(hash, bytes, encode(bytes, n, 0));
    fsx_ntfs_put64(bytes, n->size);
    hash = hash_bytes(hash, bytes, 8);
  }
  int64_t usn = replay.event_usn;
  for (unsigned i = 0; i < replay.events; i++) {
    Node *n = &replay.nodes[replay.first + i];
    hash = hash_bytes(hash, bytes, encode(bytes, n, usn));
    usn += record_bytes(n);
  }
  fsx_ntfs_put64(bytes, (uint64_t)replay.identity.next_usn);
  return hash_bytes(hash, bytes, 8);
}
static void close_engine(void) {
  if (query_running) {
    fsx_test_queries_stop(queries);
    query_running = false;
  }
  if (job) {
    fsx_index_cancel(job);
    fsx_index_wait(job, FSX_DEADLINE_INFINITE);
    fsx_index_release(job);
    job = NULL;
  }
  if (source)
    check_equal(fsx_ntfs_close(&source), FS_OK);
  if (engine)
    check_equal(fsx_engine_close(&engine, FSX_DEADLINE_INFINITE), FS_OK);
  if (database[0]) {
    check_equal(remove(database), 0);
    database[0] = 0;
  }
}
static void open_engine(uint32_t pages) {
  snprintf(database, sizeof(database), "ntfs-replay-%d-%" PRIu64 ".db",
           cmeta_getpid(), cmeta_hrtime());
  FsEngineConfig config = fsx_engine_config(database);
  config.work_memory_pages = pages;
  check_equal(fsx_engine_open(&config, &engine), FS_OK);
}
static void setup(void) {
  operation = 0;
  check_not_null(getcwd(root, sizeof(root)));
  check_equal(fsx_test_vfs_start(), SQLITE_OK);
  vfs_started = true;
  queries = calloc(1, sizeof(*queries));
  check_not_null(queries);
}
static void teardown(void) {
  close_engine();
  if (measurements)
    fclose(measurements);
  if (query_measurements)
    fclose(query_measurements);
  measurements = query_measurements = NULL;
  free(queries);
  queries = NULL;
  free(replay.nodes);
  replay.nodes = NULL;
  if (vfs_started)
    check_equal(fsx_test_vfs_stop(), SQLITE_OK);
  vfs_started = false;
}
static void validate(void) {
  sqlite3_stmt *s = NULL;
  check_equal(sqlite3_prepare_v2(engine->db,
                                 "SELECT o.file_id,o.size,e.name,e.path FROM "
                                 "objects o JOIN entries e USING(object_key)",
                                 -1, &s, NULL),
              SQLITE_OK);
  unsigned count = 0;
  int rc;
  while ((rc = sqlite3_step(s)) == SQLITE_ROW) {
    uint64_t id = fsx_ntfs_get64(sqlite3_column_blob(s, 0));
    check_true(id > 1 && id <= replay.count);
    Node *n = &replay.nodes[id - 1];
    check_equal((const char *)sqlite3_column_text(s, 2), n->name);
    char path[2304];
    if (n->directory)
      snprintf(path, sizeof(path), "%s/%s", source->root, n->name);
    else
      snprintf(path, sizeof(path), "%s/%s/%s", source->root,
               replay.nodes[n->parent - 1].name, n->name);
    check_equal((const char *)sqlite3_column_text(s, 3), path);
    if (!n->directory)
      check_equal(fsx_ntfs_get64(sqlite3_column_blob(s, 1)), n->size);
    count++;
  }
  sqlite3_finalize(s);
  check_equal(rc, SQLITE_DONE);
  check_equal(count, replay.count - 1);
}
static void run(const Mode *mode, unsigned round, unsigned position,
                const char *phase, unsigned sample, bool rebuild) {
  unsigned input = rebuild ? 0 : replay.events > 1 ? 31 : sample + 1;
  uint64_t hash = input_hash();
  if (!round && !position)
    expected_inputs[input] = hash;
  check_equal(hash, expected_inputs[input]);
  FsTestIo before = fsx_test_work_io(engine);
  check_equal(fsx_test_queries_start(queries, engine), 0);
  query_running = true;
  uint64_t start = cmeta_hrtime(), submit_ns = 0;
  uint64_t records = 0;
  FsNtfsMetrics total = {0};
  FsNtfsInfo info = {0};
  FsStatus status;
  unsigned calls = 0;
  do {
    uint64_t call_start = cmeta_hrtime();
    if (mode->async) {
      status = rebuild ? fsx_ntfs_rebuild_async(source, &job)
                       : fsx_ntfs_sync_async(source, &job);
      submit_ns += cmeta_hrtime() - call_start;
      if (status == FS_OK) {
        status = fsx_index_wait(job, FSX_DEADLINE_INFINITE);
        FsIndexInfo done;
        check_equal(fsx_index_poll(job, &done), status);
        info = done.ntfs;
        fsx_index_release(job);
        job = NULL;
      }
    } else {
      status = rebuild ? fsx_ntfs_rebuild(source, &info)
                       : fsx_ntfs_sync(source, &info);
      submit_ns += cmeta_hrtime() - call_start;
    }
    records += info.journal_records;
    fsx_test_metrics_add(&total, &timing);
    calls++;
  } while (status == FS_OK && !info.caught_up && calls < 1024);
  timing = total;
  info.journal_records = records;
  uint64_t end = cmeta_hrtime();
  fsx_test_queries_stop(queries);
  query_running = false;
  check_equal(status, FS_OK);
  check_true(info.caught_up);
  check_equal(info.next_usn, replay.identity.next_usn);
  check_equal(info.journal_records, (uint64_t)(rebuild ? 0 : replay.events));
  check_true(info.pipeline_peak_batches <= mode->queue);
  uint64_t stages = timing.initialize_ns + timing.inventory_ns +
                    timing.journal_ns + timing.reconcile_ns +
                    timing.projection_ns + timing.snapshot_ns +
                    timing.commit_ns + timing.publish_ns;
  check_true(stages <= timing.total_ns);
  check_true(timing.total_ns <= end - start);
  if (!mode->queue) {
    check_equal(timing.producer_waits, UINT64_C(0));
    check_equal(timing.consumer_waits, UINT64_C(0));
  }
  FsTestIo after = fsx_test_work_io(engine);
  if (mode->pages)
    check_equal(after.bytes, UINT64_C(0));
  fprintf(measurements,
          "%s,%u,%u,%s,%u,%" PRIu64 ",%" PRIu64 ",%" PRId64 ",%u,%" PRIu64
          ",%" PRId64 ",%" PRIu64 ",%" PRIu64 ",%016" PRIx64 "\n",
          mode->name, round, position, phase, sample, end - start, submit_ns,
          info.next_usn, replay.count - 1, info.journal_records,
          fsx_test_work_pages(engine), after.writes - before.writes,
          after.bytes - before.bytes, hash);
  fprintf(measurements, "# stages mode=%s round=%u phase=%s sample=%u",
          mode->name, round, phase, sample);
  fsx_test_metrics_report(measurements, &timing, &(FsNtfsMetrics){0});
  char label[96];
  snprintf(label, sizeof(label), "%s/%u/%s", mode->name, round, phase);
  fsx_test_queries_report(queries, query_measurements, label, sample, start,
                          end);
  fprintf(measurements,
          "# concurrent mode=%s round=%u phase=%s sample=%u samples=%u "
          "capacity=%u\n",
          mode->name, round, phase, sample, queries->count,
          FSX_TEST_QUERY_CAPACITY);
  unsigned files = replay.count - replay.directories - 1;
  for (unsigned i = 0; i < queries->count; i++) {
    FsTestQuerySample *s = &queries->samples[i];
    check_equal(s->status, FS_OK);
    check_true(s->matches == files || (rebuild && !s->epoch && !s->matches));
  }
  validate();
  operation++;
}
suite("NTFS controlled replay and diagnostics") {
  before_each() { setup(); }
  after_each() { teardown(); }
  it("attributes work-pager writes and detects actual spill") {
    for (unsigned memory = 0; memory < 2; memory++) {
      open_engine(memory ? 4096 : 0);
      check_equal(sqlite3_exec(engine->db,
                               "PRAGMA fsx_work.cache_size=2;"
                               "CREATE TABLE fsx_work.payload(data BLOB);"
                               "BEGIN;WITH RECURSIVE n(x) AS(VALUES(1) UNION "
                               "ALL SELECT x+1 FROM n WHERE x<1024)"
                               " INSERT INTO fsx_work.payload SELECT "
                               "zeroblob(4096) FROM n;COMMIT;",
                               NULL, NULL, NULL),
                  SQLITE_OK);
      FsTestIo before = fsx_test_work_io(engine);
      check_equal(before.bytes != 0, !memory);
      check_equal(sqlite3_exec(engine->db,
                               "CREATE TABLE main.vfs_control(x);INSERT INTO "
                               "main.vfs_control VALUES(1)",
                               NULL, NULL, NULL),
                  SQLITE_OK);
      FsTestIo after = fsx_test_work_io(engine);
      check_equal(before.bytes, after.bytes);
      check_true(fsx_test_work_pages(engine) > 1024);
      close_engine();
    }
  }
  it("replays identical snapshots and USN batches in balanced mode order") {
    const char *setting = getenv("FSX_NTFS_BENCHMARK");
    bool full = setting && !strcmp(setting, "1");
    unsigned dirs = full ? 100 : 4, files = full ? 100 : 16;
    unsigned updates = full ? 20 : 2;
    char name[160];
    snprintf(name, sizeof(name), "ntfs-replay-%d-%" PRIu64 ".csv",
             cmeta_getpid(), cmeta_hrtime());
    measurements = fopen(name, "wb");
    check_not_null(measurements);
    snprintf(name, sizeof(name), "ntfs-replay-%d-%" PRIu64 "-queries.csv",
             cmeta_getpid(), cmeta_hrtime());
    query_measurements = fopen(name, "wb");
    check_not_null(query_measurements);
    fprintf(measurements,
            "# modeled provider v1 dirs=%u files_per_dir=%u rounds=4 "
            "updates=%u query_interval_ms=1\n",
            dirs, files, updates);
    fprintf(measurements,
            "# build=%s compiler=%s sqlite=%s query_capacity=%u\n",
            FSX_TEST_BUILD_TYPE, __clang_version__, sqlite3_libversion(),
            FSX_TEST_QUERY_CAPACITY);
    fputs("# wall includes concurrent query load; submit_ns is caller API "
          "duration; successful work VFS writes are not physical I/O\n",
          measurements);
    fputs("mode,round,position,phase,sample,wall_ns,submit_ns,next_usn,entries,"
          "journal_records,work_pages,work_write_calls,work_write_bytes,input_"
          "hash\n",
          measurements);
    fputs("phase,sample,index_start_ns,index_end_ns,query_start_ns,query_end_"
          "ns,epoch,matches,status\n",
          query_measurements);
    /* Cyclic Latin square: each mode occupies every position exactly once. */
    for (unsigned round = 0; round < 4; round++) {
      for (unsigned position = 0; position < 4; position++) {
        const Mode *mode = &modes[(round + position) % 4];
        reset_nodes(dirs, files);
        open_engine(mode->pages);
        check_equal(
            fsx_native_volume(engine, root, &replay.identity.common_volume),
            FS_OK);
        FsNtfsConfig config = fsx_ntfs_config();
        config.mft_reader = FS_NTFS_MFT_RAW;
        config.pipeline_batches = mode->queue;
        config.buffer_bytes = 65536;
        config.max_batch_records = 65536;
        check_equal(fsx_ntfs_open_provider(engine, root, &config, &ops, &replay,
                                           &source),
                    FS_OK);
        source->metrics = &timing;
        run(mode, round, position, "build", 0, true);
        for (unsigned sample = 0; sample < updates; sample++) {
          event_batch(1, sample);
          run(mode, round, position, "metadata", sample, false);
        }
        event_batch(dirs * files, 100);
        run(mode, round, position, "bulk_metadata", 0, false);
        close_engine();
      }
    }
    fprintf(measurements, "# acceptance=passed operations=%u\n", operation);
  }
}

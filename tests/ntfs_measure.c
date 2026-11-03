#include "ntfs_measure.h"
#include <inttypes.h>
#include <stddef.h>
#include <stdlib.h>
#define TINYTEST_NO_MAIN
#include <tinytest.h>

typedef struct MeasuredFile {
  sqlite3_file base;
  sqlite3_io_methods methods;
  FsTestIo io;
  max_align_t alignment;
  /* Parent file follows this aligned header. */
} MeasuredFile;
static sqlite3_vfs measured_vfs;
static sqlite3_vfs *parent_vfs;
static sqlite3_file *under(sqlite3_file *f) {
  return (sqlite3_file *)((MeasuredFile *)f + 1);
}
static int io_close(sqlite3_file *f) {
  return under(f)->pMethods->xClose(under(f));
}
static int io_read(sqlite3_file *f, void *p, int n, sqlite3_int64 at) {
  return under(f)->pMethods->xRead(under(f), p, n, at);
}
static int io_write(sqlite3_file *f, const void *p, int n, sqlite3_int64 at) {
  int rc = under(f)->pMethods->xWrite(under(f), p, n, at);
  if (rc == SQLITE_OK) {
    ((MeasuredFile *)f)->io.writes++;
    ((MeasuredFile *)f)->io.bytes += (uint64_t)n;
  }
  return rc;
}
static int io_truncate(sqlite3_file *f, sqlite3_int64 n) {
  return under(f)->pMethods->xTruncate(under(f), n);
}
static int io_sync(sqlite3_file *f, int flags) {
  return under(f)->pMethods->xSync(under(f), flags);
}
static int io_size(sqlite3_file *f, sqlite3_int64 *n) {
  return under(f)->pMethods->xFileSize(under(f), n);
}
static int io_lock(sqlite3_file *f, int flags) {
  return under(f)->pMethods->xLock(under(f), flags);
}
static int io_unlock(sqlite3_file *f, int flags) {
  return under(f)->pMethods->xUnlock(under(f), flags);
}
static int io_reserved(sqlite3_file *f, int *out) {
  return under(f)->pMethods->xCheckReservedLock(under(f), out);
}
static int io_control(sqlite3_file *f, int op, void *out) {
  return under(f)->pMethods->xFileControl(under(f), op, out);
}
static int io_sector(sqlite3_file *f) {
  return under(f)->pMethods->xSectorSize(under(f));
}
static int io_characteristics(sqlite3_file *f) {
  return under(f)->pMethods->xDeviceCharacteristics(under(f));
}
static int io_shm_map(sqlite3_file *f, int page, int size, int extend,
                      void volatile **out) {
  return under(f)->pMethods->xShmMap(under(f), page, size, extend, out);
}
static int io_shm_lock(sqlite3_file *f, int offset, int n, int flags) {
  return under(f)->pMethods->xShmLock(under(f), offset, n, flags);
}
static void io_shm_barrier(sqlite3_file *f) {
  under(f)->pMethods->xShmBarrier(under(f));
}
static int io_shm_unmap(sqlite3_file *f, int remove) {
  return under(f)->pMethods->xShmUnmap(under(f), remove);
}
static int io_fetch(sqlite3_file *f, sqlite3_int64 at, int n, void **out) {
  return under(f)->pMethods->xFetch(under(f), at, n, out);
}
static int io_unfetch(sqlite3_file *f, sqlite3_int64 at, void *p) {
  return under(f)->pMethods->xUnfetch(under(f), at, p);
}
static int v_open(sqlite3_vfs *v, const char *name, sqlite3_file *f, int flags,
                  int *out) {
  (void)v;
  MeasuredFile *m = (MeasuredFile *)f;
  memset(m, 0, sizeof(*m));
  int rc = parent_vfs->xOpen(parent_vfs, name, under(f), flags, out);
  if (rc != SQLITE_OK)
    return rc;
  m->methods = *under(f)->pMethods;
  m->methods.xClose = io_close;
  m->methods.xRead = io_read;
  m->methods.xWrite = io_write;
  m->methods.xTruncate = io_truncate;
  m->methods.xSync = io_sync;
  m->methods.xFileSize = io_size;
  m->methods.xLock = io_lock;
  m->methods.xUnlock = io_unlock;
  m->methods.xCheckReservedLock = io_reserved;
  m->methods.xFileControl = io_control;
  m->methods.xSectorSize = io_sector;
  m->methods.xDeviceCharacteristics = io_characteristics;
  if (m->methods.iVersion >= 2) {
    m->methods.xShmMap = io_shm_map;
    m->methods.xShmLock = io_shm_lock;
    m->methods.xShmBarrier = io_shm_barrier;
    m->methods.xShmUnmap = io_shm_unmap;
  }
  if (m->methods.iVersion >= 3) {
    m->methods.xFetch = io_fetch;
    m->methods.xUnfetch = io_unfetch;
  }
  f->pMethods = &m->methods;
  return SQLITE_OK;
}
static int v_delete(sqlite3_vfs *v, const char *p, int sync) {
  (void)v;
  return parent_vfs->xDelete(parent_vfs, p, sync);
}
static int v_access(sqlite3_vfs *v, const char *p, int flags, int *out) {
  (void)v;
  return parent_vfs->xAccess(parent_vfs, p, flags, out);
}
static int v_path(sqlite3_vfs *v, const char *p, int n, char *out) {
  (void)v;
  return parent_vfs->xFullPathname(parent_vfs, p, n, out);
}
static void *v_dl_open(sqlite3_vfs *v, const char *p) {
  (void)v;
  return parent_vfs->xDlOpen(parent_vfs, p);
}
static void v_dl_error(sqlite3_vfs *v, int n, char *p) {
  (void)v;
  parent_vfs->xDlError(parent_vfs, n, p);
}
static void (*v_dl_sym(sqlite3_vfs *v, void *h, const char *p))(void) {
  (void)v;
  return parent_vfs->xDlSym(parent_vfs, h, p);
}
static void v_dl_close(sqlite3_vfs *v, void *p) {
  (void)v;
  parent_vfs->xDlClose(parent_vfs, p);
}
static int v_random(sqlite3_vfs *v, int n, char *p) {
  (void)v;
  return parent_vfs->xRandomness(parent_vfs, n, p);
}
static int v_sleep(sqlite3_vfs *v, int n) {
  (void)v;
  return parent_vfs->xSleep(parent_vfs, n);
}
static int v_time(sqlite3_vfs *v, double *out) {
  (void)v;
  return parent_vfs->xCurrentTime(parent_vfs, out);
}
static int v_error(sqlite3_vfs *v, int n, char *out) {
  (void)v;
  return parent_vfs->xGetLastError(parent_vfs, n, out);
}
int fsx_test_vfs_start(void) {
  parent_vfs = sqlite3_vfs_find(NULL);
  measured_vfs = (sqlite3_vfs){.iVersion = 1,
                               .szOsFile = (int)sizeof(MeasuredFile) +
                                           parent_vfs->szOsFile,
                               .mxPathname = parent_vfs->mxPathname,
                               .zName = "fsx-test-measured",
                               .xOpen = v_open,
                               .xDelete = v_delete,
                               .xAccess = v_access,
                               .xFullPathname = v_path,
                               .xDlOpen = v_dl_open,
                               .xDlError = v_dl_error,
                               .xDlSym = v_dl_sym,
                               .xDlClose = v_dl_close,
                               .xRandomness = v_random,
                               .xSleep = v_sleep,
                               .xCurrentTime = v_time,
                               .xGetLastError = v_error};
  return sqlite3_vfs_register(&measured_vfs, 1);
}
int fsx_test_vfs_stop(void) {
  int rc = sqlite3_vfs_unregister(&measured_vfs);
  if (rc == SQLITE_OK)
    rc = sqlite3_vfs_register(parent_vfs, 1);
  return rc;
}
FsTestIo fsx_test_work_io(FsEngine *e) {
  sqlite3_file *file = NULL;
  check_equal(
      sqlite3_file_control(e->db, "fsx_work", SQLITE_FCNTL_FILE_POINTER, &file),
      SQLITE_OK);
  /* Memory databases and unopened temporary pagers have no VFS file. */
  if (!file || !file->pMethods)
    return (FsTestIo){0};
  check_true(file->pMethods == &((MeasuredFile *)file)->methods);
  return ((MeasuredFile *)file)->io;
}
int64_t fsx_test_work_pages(FsEngine *e) {
  sqlite3_stmt *stmt = NULL;
  int rc =
      sqlite3_prepare_v2(e->db, "PRAGMA fsx_work.page_count", -1, &stmt, NULL);
  if (rc == SQLITE_OK)
    rc = sqlite3_step(stmt);
  int64_t pages = rc == SQLITE_ROW ? sqlite3_column_int64(stmt, 0) : -1;
  sqlite3_finalize(stmt);
  return pages;
}
static void query_worker(void *context) {
  FsTestQueries *q = context;
  while (!atomic_load(&q->stop) && q->count < FSX_TEST_QUERY_CAPACITY) {
    FsTestQuerySample *s = &q->samples[q->count++];
    FsQuery *query = NULL;
    FsResult *result = NULL;
    FsQueryOptions options = fsx_query_options();
    options.limit = 10;
    FsQueryRequest request = {sizeof(request), FSX_API_VERSION, "file", 4,
                              options};
    FsResultInfo info = {0};
    s->start = cmeta_hrtime();
    s->status = fsx_query_submit(q->engine, &request, &query);
    if (s->status == FS_OK)
      s->status = fsx_query_wait(query, FSX_DEADLINE_INFINITE);
    if (s->status == FS_OK)
      s->status = fsx_query_result_acquire(query, &result);
    if (s->status == FS_OK)
      s->status = fsx_result_info(result, &info);
    s->end = cmeta_hrtime();
    s->epoch = info.snapshot_epoch;
    s->matches = info.total_count;
    fsx_result_release(result);
    fsx_query_release(query);
    cmeta_sleep_ms(1);
  }
}
int fsx_test_queries_start(FsTestQueries *q, FsEngine *e) {
  q->engine = e;
  q->count = 0;
  atomic_init(&q->stop, false);
  return cmeta_thread_create(&q->thread, query_worker, q);
}
void fsx_test_queries_stop(FsTestQueries *q) {
  atomic_store(&q->stop, true);
  cmeta_thread_join(&q->thread);
}
void fsx_test_queries_report(FsTestQueries *q, FILE *file, const char *phase,
                             unsigned sample, uint64_t start, uint64_t end) {
  for (unsigned i = 0; i < q->count; i++) {
    FsTestQuerySample *s = &q->samples[i];
    fprintf(file,
            "%s,%u,%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64
            ",%" PRIu64 ",%d\n",
            phase, sample, start, end, s->start, s->end, s->epoch, s->matches,
            (int)s->status);
  }
  fflush(file);
}
#define METRICS(X)                                                             \
  X(initialize_ns)                                                             \
  X(inventory_ns)                                                              \
  X(journal_ns) X(reconcile_ns) X(projection_ns) X(snapshot_ns) X(commit_ns)   \
      X(publish_ns) X(total_ns) X(producer_waits) X(producer_wait_ns)          \
          X(consumer_waits) X(consumer_wait_ns) X(consumer_callback_ns)
void fsx_test_metrics_add(FsNtfsMetrics *out, const FsNtfsMetrics *in) {
#define ADD(name) out->name += in->name;
  METRICS(ADD)
#undef ADD
}
void fsx_test_metrics_report(FILE *file, const FsNtfsMetrics *end,
                             const FsNtfsMetrics *start) {
#define REPORT(name)                                                           \
  fprintf(file, " " #name "=%" PRIu64, end->name - start->name);
  METRICS(REPORT)
#undef REPORT
  fputc('\n', file);
}

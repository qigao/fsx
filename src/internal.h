#ifndef FSX_INTERNAL_H
#define FSX_INTERNAL_H
#include <clock.h>
#include <fsx/api.h>
#include <limits.h>
#include <sqlite3.h>
#include <stdatomic.h>
#include <string.h>
#include <thread.h>
#include <thread_pool.h>
#include <unicode.h>

typedef struct FsEntry {
  FsFileKey key;
  uint64_t entry_key, size, mtime;
  uint32_t flags;
  char *name, *path, *name_fold, *path_fold;
} FsEntry;
enum { FSX_PAGE_ENTRIES = 4096 };
typedef struct FsPage {
  atomic_uint refs;
  uint32_t count, capacity;
  FsEntry entries[];
} FsPage;
typedef struct FsSnapshot {
  atomic_uint refs;
  FsEngine *engine;
  uint64_t epoch, unsupported;
  uint32_t count, sources, incomplete;
  uint32_t page_count, reused_pages;
  FsPage **pages;
} FsSnapshot;
static inline const FsEntry *fsx_snapshot_entry(const FsSnapshot *s,
                                                uint32_t id) {
  return &s->pages[id / FSX_PAGE_ENTRIES]->entries[id % FSX_PAGE_ENTRIES];
}
struct FsEngine {
  FsEngineConfig config;
  cmeta_mutex_t mutex, budget_mutex;
  cmeta_cond_t changed;
  cmeta_threadpool_t *pool;
  cmeta_threadpool_t *index_pool;
  _Atomic(FsIndexJob *) index_job;
  atomic_bool writer_cancelled;
  sqlite3 *db;
  /* Set only with a quiescent writer; callback must not mutate db. */
  int (*sql_observer)(void *);
  void *sql_observer_context;
  FsSnapshot *current;
  FsQuery *jobs;
  FsWatch *watches;
  FsNtfs *ntfs;
  uint64_t owned;
  uint32_t handles, active_jobs;
  uint32_t watch_count, dirty_watches, dirty_ntfs;
  bool writer, needs_rescan, unscoped_failure;
  struct FsRootHealth *root_health;
  uint32_t root_health_count;
  atomic_bool stopping;
  char *database_path, *reader_path;
  unsigned char database_identity[16];
  bool has_database_identity;
};
typedef struct FsRootHealth {
  struct FsRootHealth *next;
  char *root;
  bool pending;
} FsRootHealth;
struct FsResult {
  atomic_uint refs;
  FsEngine *engine;
  FsSnapshot *snapshot;
  FsResultInfo info;
  uint32_t *ids;
};
struct FsQuery {
  atomic_uint refs;
  atomic_bool cancelled;
  FsEngine *engine;
  FsSnapshot *snapshot;
  FsQuery *next;
  cmeta_mutex_t mutex;
  cmeta_cond_t changed;
  FsQueryInfo info;
  FsQueryOptions options;
  bool needs_rescan;
  char *text;
  size_t text_bytes;
  FsResult *result;
};
typedef struct FsObserved {
  const char *name, *path;
  const void *raw_name;
  size_t raw_bytes;
  uint64_t file_lo, file_hi, size, mtime;
  uint32_t flags;
} FsObserved;
typedef FsStatus (*FsObserve)(void *, const FsObserved *);
typedef struct FsScanHooks {
  FsStatus (*directory)(void *, int descriptor, unsigned depth);
  FsStatus (*checkpoint)(void *);
  void *context;
} FsScanHooks;
typedef struct FsWalk {
  FsEngine *engine;
  FsObserve observe;
  void *context;
  uint64_t skipped, unsupported;
  const FsScanHooks *hooks;
} FsWalk;

bool fsx_reserve(FsEngine *, size_t);
void fsx_unreserve(FsEngine *, size_t);
void *fsx_alloc(FsEngine *, size_t);
void fsx_free(void *);
char *fsx_copy(FsEngine *, const char *, size_t);
FsStatus fsx_fold(FsEngine *, const char *, char **);
bool fsx_utf8(const char *, size_t);
void fsx_snapshot_release(FsSnapshot *);
void fsx_page_release(FsPage *);
FsStatus fsx_page_retain(FsPage *);
FsPage *fsx_page_create(FsEngine *, uint32_t capacity);
FsStatus fsx_page_append(FsEngine *, FsPage *, const FsEntry *);
FsStatus fsx_snapshot_load(FsEngine *, FsSnapshot **);
FsStatus fsx_snapshot_patch(FsEngine *, uint64_t, sqlite3_stmt *,
                            FsSnapshot **);
FsStatus fsx_db_open(FsEngine *);
typedef struct FsCandidates {
  sqlite3 *connection;
  sqlite3_stmt *rows;
  FsQuery *query;
  FsCandidateStrategy strategy;
  uint32_t next_entry;
} FsCandidates;
FsStatus fsx_candidates_open(FsQuery *, const char *, FsCandidates *);
FsStatus fsx_candidates_next(FsCandidates *, uint32_t *);
void fsx_candidates_close(FsCandidates *);
FsStatus fsx_db_scan(FsEngine *, const char *, FsScanInfo *, const FsScanHooks *);
FsStatus fsx_engine_scan_watched(FsEngine *, const char *, FsScanInfo *,
                                const FsScanHooks *);
/* Caller owns the writer admission, including an optional joined producer. */
bool fsx_writer_stopping(FsEngine *);
FsStatus fsx_engine_scan_admitted(FsEngine *, const char *, FsScanInfo *);
FsStatus fsx_db_status(int);
FsStatus fsx_native_root(FsEngine *, const char *, char **);
FsStatus fsx_walk(FsWalk *, const char *);
FsStatus fsx_native_volume(FsEngine *, const char *, uint64_t *);
FsStatus fsx_query_execute(FsQuery *, FsResult **);
FsStatus fsx_query_stop(FsQuery *);
FsStatus fsx_health_prepare(FsEngine *, const char *, FsRootHealth **);
FsStatus fsx_health_load(FsEngine *);
void fsx_health_destroy(FsEngine *);
/* The following control-state updates require the engine mutex. */
void fsx_health_refresh(FsEngine *);
void fsx_health_invalidate(FsEngine *, const char *root);
void fsx_health_validate(FsEngine *, FsRootHealth *);
/* Caller holds engine mutex; only signals cancellation, never destroys watches.
 */
void fsx_watch_cancel_all(FsEngine *);
FsStatus fsx_ntfs_invalidate_database(FsEngine *, uint32_t source);
void fsx_ntfs_mark_invalid(FsEngine *, uint32_t source);
void fsx_result_unref(FsResult *);

#ifdef _WIN32
#include <wchar.h>
wchar_t *fsx_wide(FsEngine *, const char *);
char *fsx_utf8_from_wide(FsEngine *, const wchar_t *);
#endif
#endif

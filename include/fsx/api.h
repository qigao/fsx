#ifndef FSX_API_H
#define FSX_API_H
#include <fsx/types.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct FsEngine FsEngine;
typedef struct FsQuery FsQuery;
typedef struct FsResult FsResult;
typedef struct FsWatch FsWatch;
typedef struct FsNtfs FsNtfs;
typedef struct FsIndexJob FsIndexJob;
typedef struct FsContentQuery FsContentQuery;
typedef struct FsContentBatch FsContentBatch;
enum { FS_CONTENT_LITERAL, FS_CONTENT_PCRE2 };
enum { FS_CONTENT_WINDOW, FS_CONTENT_SCOPE, FS_CONTENT_BFS_LIVE };
typedef struct FsContentRequest {
  uint32_t struct_size, api_version;
  const FsResult *candidates; /* Exactly this retained result window. */
  const char *ugrep_executable, *pattern;
  uint32_t mode, max_records, max_output_bytes;
  uint64_t max_file_bytes, deadline_ns;
  /* Indexed input selects candidates or scope. Scope visits every matching
   * entry in the submission snapshot, in snapshot order, without a UI window.
   */
  const char *scope;
  uint32_t scope_flags; /* CASE_SENSITIVE | ALLOW_PARTIAL_TEXT only. */
  int32_t timezone_offset_minutes;
  /* BFS_LIVE is mutually exclusive with candidates/scope. Root must be an
   * absolute UTF-8 path with no '.', '..', empty or trailing components. */
  const char *root, *bfs_executable;
  uint32_t max_candidates;
  uint64_t max_discovery_bytes;
} FsContentRequest;
typedef struct FsContentInfo {
  FsStatus status;
  /* Scope counts grow as candidates are visited; known only after enumeration
   * finishes successfully. Window counts are known at submission. */
  bool finished, complete, candidate_count_known;
  uint64_t metadata_epoch, candidate_count, files_scanned, changed_files;
  uint64_t skipped_files, match_records, processes_started;
  uint32_t running_processes;
  int tool_exit_code;
  uint32_t candidate_source;
  uint64_t discovery_bytes, discovery_processes_started;
  int discovery_exit_code;
  char diagnostic[192];
} FsContentInfo;
typedef struct FsContentFileView {
  FsFileKey key;
  uint64_t entry_key, metadata_epoch, read_start_ns, read_end_ns;
  const char *path;
  uint32_t record_count;
} FsContentFileView;
typedef struct FsContentRecordView {
  uint64_t line, byte_offset;
  const char *text;
  size_t text_bytes;
} FsContentRecordView;
typedef struct FsOpenError {
  FsStatus status;
  int native_code;
  char diagnostic[192];
} FsOpenError;
enum { FS_NTFS_MFT_ENUM = 0, FS_NTFS_MFT_RAW = 1 };
enum { FS_NTFS_RAW_PREFETCH = 1u, FS_NTFS_RAW_BATCH_DIRS = 2u };
typedef struct FsNtfsConfig {
  uint32_t struct_size, api_version, buffer_bytes;
  uint32_t max_mft_records, max_batch_records;
  FsOpenError *open_error; /* Optional diagnostic, borrowed only during open. */
  /* ENUM by default; RAW is explicit and never downgraded. */
  uint32_t mft_reader;
  uint64_t raw_flags; /* PREFETCH | BATCH_DIRS by default; 0 selects direct
                         serial ingestion. */
  /* Owned batches between the native reader and SQL writer; 0 is serial. */
  uint32_t pipeline_batches;
} FsNtfsConfig;
typedef struct FsNtfsInfo {
  FsNtfsState state;
  FsStatus status;
  uint32_t native_error, source_id;
  uint64_t snapshot_epoch, volume_serial, root_frn, journal_id;
  int64_t next_usn, target_usn;
  uint64_t mft_records, journal_records, directories_reconciled;
  uint64_t mft_slots, raw_read_bytes, raw_read_calls, mft_kernel_records;
  bool needs_rebuild, caught_up;
  uint32_t pipeline_peak_batches;
  uint64_t pipeline_batches;
} FsNtfsInfo;
typedef struct FsEngineConfig {
  uint32_t struct_size, api_version, flags, worker_threads;
  const char *database_path; /* UTF-8, copied by open; :memory: is supported. */
  /* fsx/PCRE2 heap allocations; see docs/runtime-contract.md. */
  uint64_t owned_memory_budget;
  uint32_t max_entries, max_handles, max_query_window, max_scan_depth;
  /* Safe FTS admission floor; default 65,536; 0 admits any safe plan. */
  uint32_t fts_min_entries;
  FsOpenError *open_error; /* Optional output, borrowed only during open. */
  uint32_t sqlite_cache_kib;
  /* 4096-byte pages for small NTFS work tables; 0 selects spillable storage.
   * SQLite allocations are separate from owned_memory_budget. */
  uint32_t work_memory_pages;
} FsEngineConfig;
typedef struct FsQueryRequest {
  uint32_t struct_size, api_version;
  const char *text;
  size_t text_bytes;
  FsQueryOptions options;
} FsQueryRequest;
typedef struct FsProgress {
  uint64_t snapshot_epoch, entries, owned_bytes;
  uint32_t source_count, incomplete_sources;
  bool scanning, stopping, needs_rescan;
  uint32_t snapshot_pages, reused_snapshot_pages;
  uint32_t active_watches, dirty_watches;
} FsProgress;
typedef struct FsWatchConfig {
  /* macOS buffer_bytes bounds copied native event flags, without path storage. */
  uint32_t struct_size, api_version, buffer_bytes;
  /* Linux directory registrations per reconciliation, including the root.
   * Windows/macOS use one native recursive watch/stream. */
  uint32_t max_directories;
} FsWatchConfig;
typedef struct FsWatchInfo {
  FsWatchState state;
  /* Last scan/monitor status; the wait return value separately reports timeout.
   */
  FsStatus status;
  /* Last observed native error, including recovered notification interruptions.
   */
  uint32_t native_error, source_id;
  uint64_t snapshot_epoch, change_batches, overflows, interruptions, rescans;
  bool needs_rescan, coverage_complete;
} FsWatchInfo;
typedef struct FsScanInfo {
  uint64_t snapshot_epoch, entries, skipped_entries, unsupported_names;
  uint32_t source_id;
  bool coverage_complete;
} FsScanInfo;
enum { FS_INDEX_SCAN, FS_INDEX_NTFS_REBUILD, FS_INDEX_NTFS_SYNC };
enum {
  FS_INDEX_QUEUED,
  FS_INDEX_RUNNING,
  FS_INDEX_READY,
  FS_INDEX_FAILED,
  FS_INDEX_CANCELLED
};
typedef struct FsIndexInfo {
  uint32_t operation, state;
  FsStatus status;
  FsScanInfo scan;
  FsNtfsInfo ntfs;
} FsIndexInfo;
typedef struct FsQueryInfo {
  uint32_t state;
  FsStatus status;
  size_t error_offset;
  uint64_t snapshot_epoch;
  char diagnostic[192];
} FsQueryInfo;
typedef struct FsResultInfo {
  uint64_t snapshot_epoch, total_count, unsupported_names;
  uint32_t window_offset, window_count;
  bool total_known, coverage_complete, text_coverage_complete, regex_jit;
  bool needs_rescan;
  char core_unicode_version[16], regex_version[32], regex_unicode_version[32];
  FsCandidateStrategy candidate_strategy;
  /* Entries evaluated, including rejected ones. */
  uint64_t candidates_examined;
} FsResultInfo;
typedef struct FsEntryView {
  FsFileKey key;
  uint64_t entry_key, size, mtime_ticks;
  uint32_t flags;
  const char *name, *path; /* Borrowed until this result is released. */
  size_t name_bytes, path_bytes;
} FsEntryView;

FsEngineConfig fsx_engine_config(const char *database_path);
FsContentRequest fsx_content_request(const FsResult *, const char *ugrep,
                                     const char *pattern);
FsContentRequest fsx_content_scope_request(const char *scope, const char *ugrep,
                                           const char *pattern);
FsContentRequest fsx_content_live_request(const char *root, const char *bfs,
                                          const char *ugrep, const char *pattern);
/* Linux / macOS / Windows native backends. BFS_LIVE requires Linux or macOS.
 * Other platforms explicitly return UNSUPPORTED.
 * One active content task per engine, independent of query/index pools.
 * LIVE_READ observations, not content at metadata_epoch. Only UTF-8 text
 * without NUL is searched; a leading UTF-8 BOM is removed before matching.
 * Wait wakes for a batch or termination; consume/release batches before waiting
 * again. One outstanding file batch applies backpressure until released.
 * Release cancels and discards an unacquired batch; acquired batches remain
 * live.
 * Serialize release against other calls using the same query handle.
 */
FsStatus fsx_content_submit(FsEngine *, const FsContentRequest *,
                            FsContentQuery **);
FsStatus fsx_content_poll(FsContentQuery *, FsContentInfo *);
FsStatus fsx_content_wait(FsContentQuery *, uint64_t deadline_ns);
FsStatus fsx_content_batch_acquire(FsContentQuery *, FsContentBatch **);
FsStatus fsx_content_batch_info(const FsContentBatch *, FsContentFileView *);
FsStatus fsx_content_batch_record(const FsContentBatch *, uint32_t,
                                  FsContentRecordView *);
void fsx_content_batch_release(FsContentBatch *);
void fsx_content_cancel(FsContentQuery *);
void fsx_content_release(FsContentQuery *);
FsQueryOptions fsx_query_options(void);
const char *fsx_status_name(FsStatus);
FsStatus fsx_engine_open(const FsEngineConfig *, FsEngine **out);
uint64_t fsx_engine_now_ns(const FsEngine *);
FsStatus fsx_engine_progress(FsEngine *, FsProgress *out);
/* Synchronous single-writer scan; queries continue using retained snapshots.
 * Roots are absolute/canonicalized; links are indexed but never traversed.
 * Partial coverage is explicit in out; fatal errors preserve the old revision.
 */
FsStatus fsx_engine_scan(FsEngine *, const char *root_utf8, FsScanInfo *out);
/* One admitted writer job per engine, on a worker independent of queries.
 * Submission copies the root and returns BUSY while another writer is admitted.
 * NTFS jobs pin their session until finalization; source close returns BUSY.
 * Cancellation before commit rolls back; a committed operation returns OK.
 * Wait uses the engine clock: 0 polls, UINT64_MAX waits. A wait timeout does
 * not cancel the operation. Release never cancels implicitly. */
FsStatus fsx_engine_scan_async(FsEngine *, const char *root_utf8,
                               FsIndexJob **);
FsStatus fsx_ntfs_rebuild_async(FsNtfs *, FsIndexJob **);
FsStatus fsx_ntfs_sync_async(FsNtfs *, FsIndexJob **);
FsStatus fsx_index_poll(FsIndexJob *, FsIndexInfo *out);
FsStatus fsx_index_wait(FsIndexJob *, uint64_t deadline_ns);
void fsx_index_cancel(FsIndexJob *);
void fsx_index_release(FsIndexJob *);
FsWatchConfig fsx_watch_config(void);
/* Windows RDC / Linux inotify / macOS FSEvents. Host serializes calls per watch.
 * Open arms the root before the first scan; Linux registers descendants before enumeration.
 * Wait pumps notifications; 0 polls, UINT64_MAX waits. Scan reconciles one
 * pass. Repeat wait/scan while dirty. macOS native callbacks only enqueue bounded
 * flags; wait/scan consume them. No FSX watch worker or durable journal cursor. */
FsStatus fsx_watch_open(FsEngine *, const char *root_utf8,
                        const FsWatchConfig *, FsWatch **out);
FsStatus fsx_watch_wait(FsWatch *, uint64_t deadline_ns, FsWatchInfo *out);
FsStatus fsx_watch_scan(FsWatch *, FsScanInfo *out);
/* Join watch callers before close. Engine STOPPING wakes outstanding waits. */
FsStatus fsx_watch_close(FsWatch **);
FsNtfsConfig fsx_ntfs_config(void);
/* Local NTFS + V2 USN. No privilege changes or automatic RDC fallback.
 * Host serializes session callers. Rebuild performs MFT/USN handoff; sync
 * commits one bounded journal batch. Repeat sync until caught_up. */
FsStatus fsx_ntfs_open(FsEngine *, const char *root_utf8, const FsNtfsConfig *,
                       FsNtfs **out);
FsStatus fsx_ntfs_rebuild(FsNtfs *, FsNtfsInfo *out);
FsStatus fsx_ntfs_sync(FsNtfs *, FsNtfsInfo *out);
FsStatus fsx_ntfs_close(FsNtfs **);
FsStatus fsx_query_submit(FsEngine *, const FsQueryRequest *, FsQuery **out);
FsStatus fsx_query_poll(FsQuery *, FsQueryInfo *out);
FsStatus fsx_query_wait(FsQuery *, uint64_t deadline_ns);
void fsx_query_cancel(FsQuery *);
void fsx_query_release(FsQuery *);
FsStatus fsx_query_result_acquire(FsQuery *, FsResult **out);
FsStatus fsx_result_info(const FsResult *, FsResultInfo *out);
FsStatus fsx_result_page(const FsResult *, uint32_t offset, uint32_t limit,
                         FsHit *dst, size_t capacity, size_t *written);
FsStatus fsx_result_entry(const FsResult *, FsHit, FsEntryView *out);
void fsx_result_release(FsResult *);
/* Caller serializes final close against API entry. 0 polls; UINT64_MAX waits.
 * BUSY/TIMEOUT retains a STOPPING engine; release handles and retry. */
FsStatus fsx_engine_close(FsEngine **engine, uint64_t deadline_ns);
#ifdef __cplusplus
}
#endif
#endif

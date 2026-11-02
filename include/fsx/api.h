#ifndef FSX_API_H
#define FSX_API_H
#include <fsx/types.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct FsEngine FsEngine;
typedef struct FsQuery FsQuery;
typedef struct FsResult FsResult;
typedef struct FsOpenError {
  FsStatus status;
  int native_code;
  char diagnostic[192];
} FsOpenError;
typedef struct FsEngineConfig {
  uint32_t struct_size, api_version, flags, worker_threads;
  const char *database_path; /* UTF-8, copied by open; :memory: is supported. */
  uint64_t
      owned_memory_budget; /* fsx/PCRE2 heap allocations; see ownership.md. */
  uint32_t max_entries, max_handles, max_query_window, max_scan_depth;
  FsOpenError *open_error; /* Optional output, borrowed only during open. */
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
} FsProgress;
typedef struct FsScanInfo {
  uint64_t snapshot_epoch, entries, skipped_entries, unsupported_names;
  uint32_t source_id;
  bool coverage_complete;
} FsScanInfo;
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
} FsResultInfo;
typedef struct FsEntryView {
  FsFileKey key;
  uint64_t entry_key, size, mtime_ticks;
  uint32_t flags;
  const char *name, *path; /* Borrowed until this result is released. */
  size_t name_bytes, path_bytes;
} FsEntryView;

FsEngineConfig fsx_engine_config(const char *database_path);
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

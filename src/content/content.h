#ifndef FSX_CONTENT_H
#define FSX_CONTENT_H
#include "../internal.h"
#include <errno.h>
enum { FSX_CONTENT_QUEUE = 128, FSX_CONTENT_PATH_BYTES = 1024 * 1024 };
typedef struct FsContentCandidate {
  FsEntry entry;
  char *owned_path;
} FsContentCandidate;
/* Private, opt-in counters. Configure the engine sink only while quiescent;
 * a submitted job borrows it through terminal publication. Per-file staging is
 * worker-owned, then added under the content query mutex. Wall spans can
 * overlap across file workers; worker_cpu_ns includes only those workers, not
 * children. */
#define FSX_CONTENT_METRICS(X)                                                 \
  X(files)                                                                     \
  X(read_bytes)                                                                \
  X(write_bytes) X(read_calls) X(write_calls) X(poll_calls) X(open_ns)         \
      X(spawn_ns) X(read_ns) X(text_ns) X(poll_ns) X(write_ns) X(output_ns)    \
          X(finish_ns) X(postcheck_ns) X(file_ns) X(worker_cpu_ns)             \
              X(decode_ns)
struct FsContentMetrics {
#define FIELD(name) uint64_t name;
  FSX_CONTENT_METRICS(FIELD)
#undef FIELD
};
static inline uint64_t fsx_content_tick(bool enabled) {
  return enabled ? cmeta_hrtime() : 0;
}
static inline void fsx_content_tock(bool enabled, uint64_t start,
                                    uint64_t *out) {
  if (enabled) {
    int error = errno;
    *out += cmeta_hrtime() - start;
    errno = error;
  }
}
struct FsContentQuery {
  atomic_uint refs;
  atomic_bool cancelled;
  atomic_int failure;
  FsEngine *engine;
  FsResult *candidates;
  FsSnapshot *snapshot;
  FsQuery scope_query;
  bool coverage_complete, needs_rescan;
  FsContentRequest request;
  char *tool, *pattern;
  char *root, *bfs;
  int root_fd;
  uint64_t root_device, root_inode;
  cmeta_mutex_t mutex;
  cmeta_cond_t changed;
  FsContentInfo info;
  FsContentMetrics *metrics;
  FsContentBatch *pending;
  uint32_t in_flight;
  FsContentCandidate *queue;
  uint32_t queue_head, queue_count, queue_bytes;
  bool producer_done;
};
struct FsContentBatch {
  FsContentQuery *query;
  FsContentFileView info;
  char *wire;
  char *owned_path;
  size_t used;
  FsContentRecordView *records;
  bool acquired, profiled;
  FsContentMetrics timing;
  char diagnostic[192];
};
FsStatus fsx_content_stop(FsContentQuery *);
FsStatus fsx_content_file(FsContentQuery *, const FsEntry *, FsContentBatch *);
FsStatus fsx_content_decode(FsContentBatch *, uint32_t limit);
FsStatus fsx_content_visit(void *, const FsEntry *);
FsStatus fsx_content_process(FsContentQuery *, const FsEntry *, bool);
FsStatus fsx_content_count_candidate(FsContentQuery *, const FsEntry *);
FsStatus fsx_content_enqueue(FsContentQuery *, const FsEntry *);
FsStatus fsx_content_parallel(FsContentQuery *, FsStatus (*)(FsContentQuery *));
FsStatus fsx_content_discover(FsContentQuery *);
typedef struct FsTextState {
  uint32_t scalar, minimum;
  unsigned remaining;
} FsTextState;
bool fsx_content_text(FsTextState *, const unsigned char *, size_t);
bool fsx_content_tool_path(const char *);
#endif

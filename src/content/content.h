#ifndef FSX_CONTENT_H
#define FSX_CONTENT_H
#include "../internal.h"
enum { FSX_CONTENT_QUEUE = 128, FSX_CONTENT_PATH_BYTES = 1024 * 1024 };
typedef struct FsContentCandidate {
  FsEntry entry;
  char *owned_path;
} FsContentCandidate;
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
  bool acquired;
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

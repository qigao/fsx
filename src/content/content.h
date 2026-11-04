#ifndef FSX_CONTENT_H
#define FSX_CONTENT_H
#include "../internal.h"
struct FsContentQuery {
  atomic_uint refs;
  atomic_bool cancelled;
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
  bool outstanding;
};
struct FsContentBatch {
  FsContentQuery *query;
  FsContentFileView info;
  char *wire;
  char *owned_path;
  size_t used;
  FsContentRecordView *records;
  bool acquired;
};
FsStatus fsx_content_stop(FsContentQuery *);
FsStatus fsx_content_file(FsContentQuery *, const FsEntry *, FsContentBatch *);
FsStatus fsx_content_decode(FsContentBatch *, uint32_t limit);
FsStatus fsx_content_visit(void *, const FsEntry *);
FsStatus fsx_content_discover(FsContentQuery *);
#endif

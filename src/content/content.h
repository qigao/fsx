#ifndef FSX_CONTENT_H
#define FSX_CONTENT_H
#include "../internal.h"
struct FsContentQuery {
  atomic_uint refs;
  atomic_bool cancelled;
  FsEngine *engine;
  FsResult *candidates;
  FsContentRequest request;
  char *tool, *pattern;
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
  size_t used;
  FsContentRecordView *records;
  bool acquired;
};
FsStatus fsx_content_stop(FsContentQuery *);
FsStatus fsx_content_file(FsContentQuery *, const FsEntry *, FsContentBatch *);
FsStatus fsx_content_decode(FsContentBatch *, uint32_t limit);
#endif

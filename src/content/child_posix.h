#ifndef FSX_CHILD_POSIX_H
#define FSX_CHILD_POSIX_H
#include "content.h"
#include <sys/types.h>
typedef struct FsChild {
  pid_t pid;
  int input, output, error;
  bool discovery;
} FsChild;
FsStatus fsx_child_spawn(FsContentQuery *, const char *, char *const *, bool,
                         FsChild *);
FsStatus fsx_child_finish(FsContentQuery *, FsChild *, FsStatus, int *);
FsStatus fsx_content_open_root(FsContentQuery *, int *);
FsStatus fsx_content_check_root(FsContentQuery *);
FsStatus fsx_content_os_status(int);
bool fsx_content_path(const char *);
#endif

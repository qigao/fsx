#ifndef FSX_TEST_NTFS_MEASURE_H
#define FSX_TEST_NTFS_MEASURE_H
#include "../src/source/ntfs/ntfs.h"
#include <stdio.h>

typedef struct FsTestIo {
  uint64_t writes, bytes;
} FsTestIo;
/* Process-local VFS, registered before opening any engines, unregistered after
 * closing all of them. Counts successful VFS xWrite calls, not device I/O. */
int fsx_test_vfs_start(void);
int fsx_test_vfs_stop(void);
FsTestIo fsx_test_work_io(FsEngine *);
int64_t fsx_test_work_pages(FsEngine *);

typedef struct FsTestQuerySample {
  uint64_t start, end, epoch, matches;
  FsStatus status;
} FsTestQuerySample;
enum { FSX_TEST_QUERY_CAPACITY = 65536 };
typedef struct FsTestQueries {
  FsEngine *engine;
  cmeta_thread_t thread;
  atomic_bool stop;
  unsigned count;
  FsTestQuerySample samples[FSX_TEST_QUERY_CAPACITY];
} FsTestQueries;
int fsx_test_queries_start(FsTestQueries *, FsEngine *);
void fsx_test_queries_stop(FsTestQueries *);
void fsx_test_queries_report(FsTestQueries *, FILE *, const char *, unsigned,
                             uint64_t, uint64_t);
void fsx_test_metrics_add(FsNtfsMetrics *, const FsNtfsMetrics *);
void fsx_test_metrics_report(FILE *, const FsNtfsMetrics *,
                             const FsNtfsMetrics *);
#endif

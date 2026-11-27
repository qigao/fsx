#ifndef FSX_BFS_BRIDGE_H
#define FSX_BFS_BRIDGE_H

#include <stddef.h>

/* Private C11 boundary. All callbacks run synchronously on the calling thread.
 * Paths are borrowed for the duration of visit(). A nonzero stop()/visit()
 * return stops enumeration; the caller retains its own terminal status. */
typedef struct FsxBfsHost {
  void *context;
  void *(*allocate)(void *context, size_t bytes);
  void (*deallocate)(void *context, void *memory);
  int (*stop)(void *context);
  int (*visit)(void *context, const char *path);
} FsxBfsHost;

/* Breadth-first, no-follow, regular-file discovery on one device. Returns an
 * errno value for a traversal failure, or zero for exhaustion/callback stop.
 * The fd limit applies to the walker's cached descriptors, not the caller's.
 * No background I/O threads, CLI state, signals, or process-global allocators. */
int fsx_bfs_walk(const char *root, int descriptor_limit, const FsxBfsHost *host);

#endif

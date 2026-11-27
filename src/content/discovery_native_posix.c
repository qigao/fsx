#ifdef __APPLE__
#define _DARWIN_C_SOURCE
#else
#define _GNU_SOURCE
#endif
#include "child_posix.h"
#include "../../third_party/bfs/bridge.h"
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct FsNativeDiscovery {
  FsContentQuery *query;
  FsStatus status;
} FsNativeDiscovery;

static void *allocate(void *context, size_t bytes) {
  FsNativeDiscovery *discovery = context;
  return fsx_alloc(discovery->query->engine, bytes);
}

static void deallocate(void *context, void *memory) {
  (void)context;
  fsx_free(memory);
}

static int stop(void *context) {
  FsNativeDiscovery *discovery = context;
  if (discovery->status == FS_OK)
    discovery->status = fsx_content_stop(discovery->query);
  return discovery->status != FS_OK;
}

static int visit(void *context, const char *path) {
  FsNativeDiscovery *discovery = context;
  FsContentQuery *q = discovery->query;
  if (stop(context))
    return 1;
  size_t length = strlen(path), root_length = strlen(q->root);
  if (!length || !fsx_content_path(path) ||
      (root_length > 1 &&
       (strncmp(path, q->root, root_length) || path[root_length] != '/' ||
        !path[root_length + 1])) ||
      (root_length == 1 && !path[1])) {
    discovery->status = FS_CORRUPT;
    return 1;
  }
  if (length > 65535) {
    discovery->status = FS_LIMIT;
    return 1;
  }
  cmeta_mutex_lock(&q->mutex);
  uint64_t bytes = (uint64_t)length + 1;
  if (bytes > q->request.max_discovery_bytes - q->info.discovery_bytes)
    discovery->status = FS_LIMIT;
  else
    q->info.discovery_bytes += bytes;
  cmeta_mutex_unlock(&q->mutex);
  if (discovery->status != FS_OK)
    return 1;
  FsEntry entry = {0};
  /* FsEntry also represents owned index paths, but visit() only reads this
   * borrowed path and copies it before an asynchronous queue handoff. */
  entry.path = (char *)path;
  discovery->status = fsx_content_visit(q, &entry);
  return discovery->status != FS_OK;
}

FsStatus fsx_content_discover_native(FsContentQuery *q) {
  FsStatus status = fsx_content_open_root(q, &q->root_fd);
  if (status != FS_OK)
    return status;
  struct stat root;
  if (fstat(q->root_fd, &root))
    return fsx_content_os_status(errno);
  q->root_device = (uint64_t)root.st_dev;
  q->root_inode = (uint64_t)root.st_ino;
  FsNativeDiscovery discovery = {q, FS_OK};
  const FsxBfsHost host = {&discovery, allocate, deallocate, stop, visit};
  int error = fsx_bfs_walk(q->root, 32, &host);
  status = fsx_content_stop(q);
  if (status == FS_OK)
    status = discovery.status;
  if (status == FS_OK && error)
    status = fsx_content_os_status(error);
  if (status == FS_OK)
    status = fsx_content_check_root(q);
  return status;
}

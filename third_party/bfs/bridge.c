#include "bridge.h"
#include "allocation_redirect.h"
#include "src/bftw.h"

#include <errno.h>
#include <stdint.h>
#include <string.h>

/* bftw is deliberately single-threaded here. Save/restore the scope to support
 * independent engines on different threads and nested synchronous host calls.
 * Each allocation stores its deallocator, so freeing never depends on TLS. */
static thread_local const FsxBfsHost *allocation_host;

typedef struct FsxBfsAllocation {
  void *base;
  void *context;
  void (*deallocate)(void *, void *);
  size_t bytes;
} FsxBfsAllocation;

int fsx_bfs_memalign(void **out, size_t alignment, size_t bytes) {
  if (!out || alignment < sizeof(void *) ||
      (alignment & (alignment - 1)))
    return EINVAL;
  const FsxBfsHost *host = allocation_host;
  if (!host || !host->allocate || !host->deallocate)
    return ENOMEM;
  if (bytes > SIZE_MAX - sizeof(FsxBfsAllocation) - (alignment - 1))
    return ENOMEM;
  size_t total = sizeof(FsxBfsAllocation) + alignment - 1 + bytes;
  void *base = host->allocate(host->context, total);
  if (!base)
    return ENOMEM;
  uintptr_t start = (uintptr_t)base + sizeof(FsxBfsAllocation);
  uintptr_t aligned = (start + alignment - 1) & ~(uintptr_t)(alignment - 1);
  FsxBfsAllocation *allocation = (FsxBfsAllocation *)aligned - 1;
  *allocation = (FsxBfsAllocation){base, host->context, host->deallocate, bytes};
  *out = (void *)aligned;
  return 0;
}

void *fsx_bfs_malloc(size_t bytes) {
  void *result = NULL;
  int error = fsx_bfs_memalign(&result, alignof(max_align_t), bytes ? bytes : 1);
  if (error)
    errno = error;
  return result;
}

void *fsx_bfs_calloc(size_t count, size_t bytes) {
  if (count && bytes > SIZE_MAX / count) {
    errno = ENOMEM;
    return NULL;
  }
  size_t total = count * bytes;
  void *result = fsx_bfs_malloc(total);
  if (result)
    memset(result, 0, total);
  return result;
}

void fsx_bfs_free(void *memory) {
  if (!memory)
    return;
  FsxBfsAllocation *allocation = (FsxBfsAllocation *)memory - 1;
  allocation->deallocate(allocation->context, allocation->base);
}

void *fsx_bfs_realloc(void *memory, size_t bytes) {
  if (!bytes) {
    fsx_bfs_free(memory);
    return NULL;
  }
  void *result = fsx_bfs_malloc(bytes);
  if (!result)
    return NULL;
  if (memory) {
    const FsxBfsAllocation *allocation = (FsxBfsAllocation *)memory - 1;
    size_t copy = allocation->bytes < bytes ? allocation->bytes : bytes;
    memcpy(result, memory, copy);
    fsx_bfs_free(memory);
  }
  return result;
}

char *fsx_bfs_strndup(const char *text, size_t bytes) {
  size_t length = strnlen(text, bytes);
  char *result = fsx_bfs_malloc(length + 1);
  if (result) {
    memcpy(result, text, length);
    result[length] = 0;
  }
  return result;
}

char *fsx_bfs_strdup(const char *text) {
  return fsx_bfs_strndup(text, strlen(text));
}

typedef struct FsxBfsWalk {
  const FsxBfsHost *host;
  int error;
} FsxBfsWalk;

static enum bftw_action visit(const struct BFTW *entry, void *opaque) {
  FsxBfsWalk *walk = opaque;
  const FsxBfsHost *host = walk->host;
  if (host->stop(host->context))
    return BFTW_STOP;
  if (entry->type == BFS_ERROR) {
    walk->error = entry->error ? entry->error : EIO;
    return BFTW_STOP;
  }
  if (entry->type == BFS_REG && host->visit(host->context, entry->path))
    return BFTW_STOP;
  return BFTW_CONTINUE;
}

int fsx_bfs_walk(const char *root, int descriptor_limit, const FsxBfsHost *host) {
  if (!root || !host || !host->allocate || !host->deallocate || !host->stop ||
      !host->visit || descriptor_limit < 2)
    return EINVAL;
  FsxBfsWalk walk = {host, 0};
  const char *paths[] = {root};
  struct bftw_args args = {
      .paths = paths,
      .npaths = 1,
      .callback = visit,
      .ptr = &walk,
      .nopenfd = descriptor_limit,
      .nthreads = 0,
      .flags = BFTW_RECOVER | BFTW_PRUNE_MOUNTS | BFTW_DETECT_CYCLES,
      .strategy = BFTW_BFS,
      .mtab = NULL,
  };
  const FsxBfsHost *previous = allocation_host;
  allocation_host = host;
  int result = bftw(&args);
  int error = result ? errno : walk.error;
  allocation_host = previous;
  return error;
}

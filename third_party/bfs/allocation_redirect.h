#ifndef FSX_BFS_ALLOCATION_REDIRECT_H
#define FSX_BFS_ALLOCATION_REDIRECT_H

/* Include libc declarations before the vendor-only redirects. This header is
 * force-included only for the bfs target; no macros reach FSX or its callers. */
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

void *fsx_bfs_malloc(size_t bytes);
void *fsx_bfs_calloc(size_t count, size_t bytes);
void *fsx_bfs_realloc(void *memory, size_t bytes);
void fsx_bfs_free(void *memory);
int fsx_bfs_memalign(void **memory, size_t alignment, size_t bytes);
char *fsx_bfs_strdup(const char *text);
char *fsx_bfs_strndup(const char *text, size_t bytes);

#ifndef FSX_BFS_ALLOC_IMPLEMENTATION
#define malloc fsx_bfs_malloc
#define calloc fsx_bfs_calloc
#define realloc fsx_bfs_realloc
#define free fsx_bfs_free
#define posix_memalign fsx_bfs_memalign
#define strdup fsx_bfs_strdup
#define strndup fsx_bfs_strndup
#endif

#endif

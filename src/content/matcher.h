#ifndef FSX_CONTENT_MATCHER_H
#define FSX_CONTENT_MATCHER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct FsMatcher FsMatcher;

/* Private C boundary. Status values are FsStatus; no CMeta/C++ types cross it.
 * The context and callbacks must outlive the compiled matcher and all scans.
 * Host allocations cover this adapter, scratch space, and PCRE2. RE/flex's
 * internal C++ allocations are separate, bounded by the accepted pattern/input
 * sizes rather than charged to the host allocator. */
typedef struct FsMatcherHost {
  void *context;
  void *(*alloc)(void *context, size_t bytes);
  void (*free)(void *context, void *pointer);
  int (*stop)(void *context);
} FsMatcherHost;

/* Text is the complete line(s) containing a match, matching ugrep's %J (without
 * -o); byte_offset identifies the match itself. The text view is borrowed from
 * the input and is valid only during the callback. Return FS_OK to continue,
 * or an FsStatus failure to stop. */
typedef int (*FsMatcherEmit)(void *context, uint64_t line, uint64_t byte_offset,
                             const char *text, size_t text_bytes);

/* mode is FS_CONTENT_LITERAL or FS_CONTENT_PCRE2. Compilation is performed
 * once per job. The resulting object can be scanned concurrently; each scan
 * owns its mutable match state. Diagnostics are optional, NUL terminated. */
int fsx_matcher_compile(const FsMatcherHost *host, const char *pattern,
                        unsigned mode, FsMatcher **out, char *diagnostic,
                        size_t diagnostic_capacity);

/* Input has already passed FSX's UTF-8/NUL checks and BOM stripping. It is
 * borrowed and never modified. Matching is bounded to 32 MiB per file. */
int fsx_matcher_scan(const FsMatcher *matcher, const char *input, size_t bytes,
                     FsMatcherEmit emit, void *context, char *diagnostic,
                     size_t diagnostic_capacity);

void fsx_matcher_destroy(FsMatcher *matcher);

#ifdef __cplusplus
}
#endif

#endif

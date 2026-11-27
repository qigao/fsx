#ifndef FSX_CONTENT_MATCHER_H
#define FSX_CONTENT_MATCHER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct FsMatcher FsMatcher;

/* Private C boundary. Status values are FsStatus; no CMeta types cross it.
 * The context and callbacks must outlive the compiled matcher and all scans.
 * Host allocations cover the adapter and every PCRE2 context, compiled pattern,
 * and per-scan match state. */
typedef struct FsMatcherHost {
  void *context;
  void *(*alloc)(void *context, size_t bytes);
  void (*free)(void *context, void *pointer);
  int (*stop)(void *context);
} FsMatcherHost;

/* Text is the complete line(s) containing a match; byte_offset identifies the
 * match itself. The text view is borrowed from
 * the input and is valid only during the callback. Return FS_OK to continue,
 * or an FsStatus failure to stop. */
typedef int (*FsMatcherEmit)(void *context, uint64_t line, uint64_t byte_offset,
                             const char *text, size_t text_bytes);

/* mode is FS_CONTENT_LITERAL or FS_CONTENT_PCRE2; both use PCRE2. Literal mode
 * treats every pattern byte literally; an empty literal selects each real line
 * once. Regex mode takes an unmodified PCRE2 expression with UTF, UCP, multiline
 * anchors, and LF newlines by default. Pattern newline overrides govern empty
 * match progression; CRLF advances together when it is a newline for the
 * compiled expression. Record line numbers and text remain LF-delimited. An
 * EOF match belongs to the last line only when it is not LF-terminated. An
 * empty file produces no records. PCRE2 \K may move reported starts backward;
 * global search still advances from the engine's actual starting position.
 * Compilation is performed once per job. The resulting object can be scanned
 * concurrently; each scan owns its mutable match state. Diagnostics are
 * optional, NUL terminated. */
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

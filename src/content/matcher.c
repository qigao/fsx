#include "matcher.h"

#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>

#include <stdio.h>
#include <string.h>

/* Keep the private boundary independent of the public CMeta declarations. */
enum Status { Ok = 0, Invalid = -1, NoMemory = -2, Limit = -3, Corrupt = -10 };
enum Mode { Literal = 0, Pcre2 = 1 };
enum {
  Window = 65536,
  MaxPattern = 4096,
  MaxInput = 32 * 1024 * 1024
};

struct FsMatcher {
  FsMatcherHost host;
  unsigned mode;
  size_t literal_bytes;
  int crlf_newline;
  pcre2_general_context *general;
  pcre2_code *code;
};

static void diagnostic(char *out, size_t capacity, const char *message) {
  if (out && capacity)
    snprintf(out, capacity, "%s", message);
}

static void *pcre_alloc(PCRE2_SIZE bytes, void *context) {
  const FsMatcherHost *host = context;
  return host->alloc(host->context, (size_t)bytes);
}

static void pcre_free(void *pointer, void *context) {
  const FsMatcherHost *host = context;
  host->free(host->context, pointer);
}

static int stop(const FsMatcher *matcher) {
  return matcher->host.stop(matcher->host.context);
}

static size_t minimum(size_t a, size_t b) { return a < b ? a : b; }

/* Locate complete line spans in the original input, independently of literal
 * search windows. Counting normally advances monotonically; a PCRE2 \K in a
 * lookbehind subroutine can move a later reported match start backward. */
typedef struct Output {
  const FsMatcher *matcher;
  const char *input;
  size_t bytes;
  FsMatcherEmit emit;
  void *context;
  size_t counted;
  size_t bol;
  size_t cached_eol;
  uint64_t line;
} Output;

static int end_of_line(const Output *out, size_t from, size_t *end) {
  while (from < out->bytes) {
    int status = stop(out->matcher);
    if (status != Ok)
      return status;
    size_t count = minimum(Window, out->bytes - from);
    const char *newline = memchr(out->input + from, '\n', count);
    if (newline) {
      *end = (size_t)(newline - out->input);
      return Ok;
    }
    from += count;
  }
  *end = out->bytes;
  return Ok;
}

static int output_match(Output *out, size_t begin, size_t end) {
  int status = stop(out->matcher);
  if (status != Ok)
    return status;
  if (begin > end || end > out->bytes)
    return Corrupt;
  if (begin < out->counted) {
    out->counted = 0;
    out->bol = 0;
    out->cached_eol = 0;
    out->line = 1;
  }
  while (out->counted < begin) {
    status = stop(out->matcher);
    if (status != Ok)
      return status;
    size_t count = minimum(Window, begin - out->counted);
    const char *newline = memchr(out->input + out->counted, '\n', count);
    if (newline) {
      out->counted = (size_t)(newline - out->input) + 1;
      out->bol = out->counted;
      ++out->line;
    } else {
      out->counted += count;
    }
  }
  if (out->cached_eol < end || out->cached_eol < begin ||
      (out->cached_eol == 0 && out->bytes != 0)) {
    status = end_of_line(out, end, &out->cached_eol);
    if (status != Ok)
      return status;
  }
  return out->emit(out->context, out->line, (uint64_t)begin,
                   out->input + out->bol, out->cached_eol - out->bol);
}

static int scan_empty_literal(Output *out) {
  size_t begin = 0;
  uint64_t line = 1;
  /* An empty literal selects each actual line once. A trailing LF does not
   * introduce a synthetic EOF line, and an empty file has no lines. */
  while (begin < out->bytes) {
    size_t end;
    int status = end_of_line(out, begin, &end);
    if (status != Ok)
      return status;
    status = out->emit(out->context, line, (uint64_t)begin, out->input + begin,
                       end - begin);
    if (status != Ok)
      return status;
    begin = end < out->bytes ? end + 1 : end;
    ++line;
  }
  return stop(out->matcher);
}

typedef struct PcreState {
  const FsMatcher *matcher;
  int status;
  uint32_t calls;
} PcreState;

static int callout(pcre2_callout_block *block, void *context) {
  (void)block;
  PcreState *state = context;
  /* Regex patterns have automatic callouts and disable start optimizations.
   * Check the first and every 256th callout, in addition to each match call. */
  if ((state->calls++ & 255u) == 0) {
    state->status = stop(state->matcher);
    if (state->status != Ok)
      return PCRE2_ERROR_CALLOUT;
  }
  return 0;
}

typedef struct PcreScan {
  pcre2_match_data *data;
  pcre2_match_context *context;
  PcreState state;
} PcreScan;

static void scan_destroy(PcreScan *scan) {
  pcre2_match_data_free(scan->data);
  pcre2_match_context_free(scan->context);
}

static int scan_create(const FsMatcher *matcher, PcreScan *scan) {
  memset(scan, 0, sizeof(*scan));
  scan->data = pcre2_match_data_create_from_pattern(matcher->code,
                                                   matcher->general);
  scan->context = pcre2_match_context_create(matcher->general);
  if (!scan->data || !scan->context) {
    scan_destroy(scan);
    return NoMemory;
  }
  scan->state.matcher = matcher;
  pcre2_set_callout(scan->context, callout, &scan->state);
  pcre2_set_match_limit(scan->context, 1000000);
  pcre2_set_depth_limit(scan->context, 256);
  pcre2_set_heap_limit(scan->context, 32 * 1024);
  return Ok;
}

static int match_error(int result, char *error, size_t error_capacity) {
  PCRE2_UCHAR message[160] = {0};
  pcre2_get_error_message(result, message, sizeof(message));
  diagnostic(error, error_capacity, (const char *)message);
  if (result == PCRE2_ERROR_NOMEMORY)
    return NoMemory;
  if (result == PCRE2_ERROR_MATCHLIMIT || result == PCRE2_ERROR_DEPTHLIMIT ||
      result == PCRE2_ERROR_HEAPLIMIT)
    return Limit;
  return Invalid;
}

static int scan_literal(Output *out, PcreScan *scan, char *error,
                         size_t error_capacity) {
  const FsMatcher *matcher = out->matcher;
  const size_t overlap = matcher->literal_bytes - 1;
  size_t base = 0;
  while (base < out->bytes) {
    size_t boundary = base + minimum(Window, out->bytes - base);
    size_t end = boundary + minimum(overlap, out->bytes - boundary);
    size_t start = 0;
    size_t next = boundary;
    for (;;) {
      int status = stop(matcher);
      if (status != Ok)
        return status;
      /* Byte-mode PCRE2_LITERAL needs no UTF-8 boundary adjustment or copy.
       * Validated UTF-8 patterns cannot begin inside a validated input scalar.
       * Every call sees at most one window plus a bounded pattern lookahead. */
      int result = pcre2_match(matcher->code,
                              (PCRE2_SPTR)(out->input + base), end - base,
                              start, 0, scan->data, scan->context);
      status = stop(matcher);
      if (status != Ok)
        return status;
      if (result == PCRE2_ERROR_NOMATCH)
        break;
      if (result < 0)
        return match_error(result, error, error_capacity);
      PCRE2_SIZE *offsets = pcre2_get_ovector_pointer(scan->data);
      size_t begin_match = base + (size_t)offsets[0];
      size_t end_match = base + (size_t)offsets[1];
      /* The following window owns matches beginning in this lookahead. */
      if (begin_match >= boundary)
        break;
      if (end_match <= begin_match)
        return Corrupt;
      status = output_match(out, begin_match, end_match);
      if (status != Ok)
        return status;
      if (next < end_match)
        next = end_match;
      start = (size_t)offsets[1];
    }
    /* A boundary-spanning match must not be searched again from its middle. */
    base = next;
  }
  return stop(matcher);
}

static int scan_regex(Output *out, PcreScan *scan, char *error,
                       size_t error_capacity) {
  const FsMatcher *matcher = out->matcher;
  uint32_t options = 0;
  size_t start = 0;
  while (start <= out->bytes) {
    int status = stop(matcher);
    if (status != Ok)
      return status;
    int result = pcre2_match(matcher->code, (PCRE2_SPTR)out->input, out->bytes,
                            start, options, scan->data, scan->context);
    if (scan->state.status != Ok)
      return scan->state.status;
    status = stop(matcher);
    if (status != Ok)
      return status;
    if (result == PCRE2_ERROR_NOMATCH) {
      if ((options & PCRE2_ANCHORED) == 0 || start == out->bytes)
        return Ok;
      /* After an empty match, first try a nonempty alternative at that same
       * position. Otherwise advance one whole newline or UTF-8 scalar. The
       * pattern may override the default LF convention with (*CRLF), etc. */
      if (matcher->crlf_newline && start + 1 < out->bytes &&
          out->input[start] == '\r' && out->input[start + 1] == '\n') {
        start += 2;
      } else {
        ++start;
        while (start < out->bytes &&
               ((unsigned char)out->input[start] & 0xc0) == 0x80)
          ++start;
      }
      options = 0;
      continue;
    }
    if (result < 0)
      return match_error(result, error, error_capacity);
    PCRE2_SIZE *offsets = pcre2_get_ovector_pointer(scan->data);
    size_t begin = (size_t)offsets[0];
    size_t end = (size_t)offsets[1];
    /* An EOF position belongs to the final line only if it is not terminated
     * by LF. Suppress the synthetic trailing line, while retaining an EOF $
     * match on an unterminated line. Other empty matches use PCRE2's normal
     * global progression, without expression-specific shortcuts. */
    if (begin == end && end == out->bytes &&
        (!out->bytes || out->input[out->bytes - 1] == '\n'))
      return Ok;
    status = output_match(out, begin, end);
    if (status != Ok)
      return status;
    start = end;
    options = 0;
    if (begin == end) {
      options = PCRE2_NOTEMPTY_ATSTART | PCRE2_ANCHORED;
    } else {
      /* \K inside a lookbehind subroutine can report a nonempty span that
       * ends at or before the actual search start. Follow PCRE2's global
       * matching rule so the next call cannot repeat the same match forever. */
      size_t startchar = (size_t)pcre2_get_startchar(scan->data);
      if (start <= startchar) {
        if (startchar >= out->bytes)
          return Ok;
        start = startchar + 1;
        while (start < out->bytes &&
               ((unsigned char)out->input[start] & 0xc0) == 0x80)
          ++start;
      }
    }
  }
  return Ok;
}

static int compile_pattern(FsMatcher *matcher, const char *pattern,
                            char *error, size_t error_capacity) {
  size_t length = strlen(pattern);
  matcher->literal_bytes = length;
  if (matcher->mode == Literal && !length)
    return Ok;
  matcher->general =
      pcre2_general_context_create(pcre_alloc, pcre_free, &matcher->host);
  if (!matcher->general)
    return NoMemory;
  pcre2_compile_context *context =
      pcre2_compile_context_create(matcher->general);
  if (!context)
    return NoMemory;
  pcre2_set_newline(context, PCRE2_NEWLINE_LF);
  pcre2_set_parens_nest_limit(context, 256);
  /* Literal windows are byte-oriented and bounded. Regex uses the original
   * PCRE2 expression with Unicode properties and multiline anchors. Enabling
   * MATCH_INVALID_UTF safely handles continuation after a byte-wise \C; FSX
   * still validates the entire subject as UTF-8 before this boundary. */
  uint32_t options = matcher->mode == Literal
                         ? PCRE2_LITERAL
                         : PCRE2_UTF | PCRE2_UCP | PCRE2_MULTILINE |
                               PCRE2_MATCH_INVALID_UTF | PCRE2_AUTO_CALLOUT |
                               PCRE2_NO_START_OPTIMIZE;
  int code = 0;
  PCRE2_SIZE offset = 0;
  matcher->code = pcre2_compile((PCRE2_SPTR)pattern, length, options, &code,
                               &offset, context);
  pcre2_compile_context_free(context);
  if (!matcher->code) {
    PCRE2_UCHAR message[160] = {0};
    pcre2_get_error_message(code, message, sizeof(message));
    if (error && error_capacity)
      snprintf(error, error_capacity, "PCRE2 at byte %zu: %s", (size_t)offset,
               (const char *)message);
    return code == PCRE2_ERROR_HEAP_FAILED || code == PCRE2_ERROR_NOMEMORY
               ? NoMemory
               : Invalid;
  }
  uint32_t newline = 0;
  int info_status =
      pcre2_pattern_info(matcher->code, PCRE2_INFO_NEWLINE, &newline);
  if (info_status != 0)
    return match_error(info_status, error, error_capacity);
  matcher->crlf_newline = newline == PCRE2_NEWLINE_CRLF ||
                          newline == PCRE2_NEWLINE_ANYCRLF ||
                          newline == PCRE2_NEWLINE_ANY;
  return stop(matcher);
}

int fsx_matcher_compile(const FsMatcherHost *host, const char *pattern,
                        unsigned mode, FsMatcher **out, char *error,
                        size_t error_capacity) {
  diagnostic(error, error_capacity, "");
  if (!out)
    return Invalid;
  *out = NULL;
  if (!host || !host->alloc || !host->free || !host->stop || !pattern ||
      mode > Pcre2 || strlen(pattern) > MaxPattern || strchr(pattern, '\n'))
    return Invalid;
  int status = host->stop(host->context);
  if (status != Ok)
    return status;
  FsMatcher *matcher = host->alloc(host->context, sizeof(*matcher));
  if (!matcher)
    return NoMemory;
  memset(matcher, 0, sizeof(*matcher));
  matcher->host = *host;
  matcher->mode = mode;
  status = compile_pattern(matcher, pattern, error, error_capacity);
  if (status == Ok)
    *out = matcher;
  else
    fsx_matcher_destroy(matcher);
  return status;
}

int fsx_matcher_scan(const FsMatcher *matcher, const char *input, size_t bytes,
                     FsMatcherEmit emit, void *context, char *error,
                     size_t error_capacity) {
  diagnostic(error, error_capacity, "");
  if (!matcher || (!input && bytes) || !emit)
    return Invalid;
  if (bytes > MaxInput)
    return Limit;
  int status = stop(matcher);
  if (status != Ok || !bytes)
    return status;
  Output output = {matcher, input, bytes, emit, context, 0, 0, 0, 1};
  if (matcher->mode == Literal && !matcher->literal_bytes)
    return scan_empty_literal(&output);
  PcreScan scan;
  status = scan_create(matcher, &scan);
  if (status != Ok)
    return status;
  if (matcher->mode == Literal)
    status = scan_literal(&output, &scan, error, error_capacity);
  else
    status = scan_regex(&output, &scan, error, error_capacity);
  scan_destroy(&scan);
  return status;
}

void fsx_matcher_destroy(FsMatcher *matcher) {
  if (matcher) {
    FsMatcherHost host = matcher->host;
    pcre2_code_free(matcher->code);
    pcre2_general_context_free(matcher->general);
    host.free(host.context, matcher);
  }
}

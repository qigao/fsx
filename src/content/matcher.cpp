#include "matcher.h"

#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>
#include <reflex/matcher.h>
#include <reflex/pcre2matcher.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <exception>
#include <new>
#include <string>

namespace {
// Keep the private boundary independent of the public CMeta declarations.
enum Status {
  Ok = 0,
  Invalid = -1,
  NoMemory = -2,
  Limit = -3,
  Io = -4,
  Corrupt = -10
};
enum Mode { Literal = 0, Pcre2 = 1 };
enum Lines { Matches, AllLines, EmptyLines, NonemptyLines };
const size_t kWindow = 65536;
const size_t kMaxPattern = 4096;
const size_t kMaxInput = 32 * 1024 * 1024;

void diagnostic(char *out, size_t capacity, const char *message) {
  if (out && capacity)
    std::snprintf(out, capacity, "%s", message);
}

void *pcre_alloc(PCRE2_SIZE bytes, void *context) {
  const FsMatcherHost *host = static_cast<const FsMatcherHost *>(context);
  return host->alloc(host->context, static_cast<size_t>(bytes));
}

void pcre_free(void *pointer, void *context) {
  const FsMatcherHost *host = static_cast<const FsMatcherHost *>(context);
  host->free(host->context, pointer);
}

struct HostBuffer {
  const FsMatcherHost &host;
  char *data;
  HostBuffer(const FsMatcherHost &h, size_t bytes)
      : host(h), data(static_cast<char *>(h.alloc(h.context, bytes))) {}
  ~HostBuffer() { host.free(host.context, data); }
  HostBuffer(const HostBuffer &) = delete;
  HostBuffer &operator=(const HostBuffer &) = delete;
};
} // namespace

struct FsMatcher {
  FsMatcherHost host;
  unsigned mode;
  Lines lines;
  size_t literal_bytes;
  bool empty_matches;
  reflex::Pattern *literal;
  pcre2_general_context *general;
  pcre2_code *code;

  FsMatcher(const FsMatcherHost &h, unsigned m)
      : host(h), mode(m), lines(Matches), literal_bytes(0),
        empty_matches(false), literal(NULL), general(NULL), code(NULL) {}

  ~FsMatcher() {
    delete literal;
    pcre2_code_free(code);
    pcre2_general_context_free(general);
  }
};

namespace {
int stop(const FsMatcher *matcher) {
  return matcher->host.stop(matcher->host.context);
}

// Locate complete line spans in the original input, independently of the
// literal search windows. Counting advances monotonically across matches.
struct Output {
  const FsMatcher *matcher;
  const char *input;
  size_t bytes;
  FsMatcherEmit emit;
  void *context;
  size_t counted;
  size_t bol;
  size_t cached_eol;
  uint64_t line;

  Output(const FsMatcher *m, const char *p, size_t n, FsMatcherEmit e, void *c)
      : matcher(m), input(p), bytes(n), emit(e), context(c), counted(0),
        bol(0), cached_eol(0), line(1) {}

  int end_of_line(size_t from, size_t *out) const {
    while (from < bytes) {
      int status = stop(matcher);
      if (status != Ok)
        return status;
      size_t count = std::min(kWindow, bytes - from);
      const char *newline =
          static_cast<const char *>(std::memchr(input + from, '\n', count));
      if (newline) {
        *out = static_cast<size_t>(newline - input);
        return Ok;
      }
      from += count;
    }
    *out = bytes;
    return Ok;
  }

  int match(size_t begin, size_t end) {
    int status = stop(matcher);
    if (status != Ok)
      return status;
    if (begin < counted || begin > end || end > bytes)
      return Corrupt;
    while (counted < begin) {
      status = stop(matcher);
      if (status != Ok)
        return status;
      size_t count = std::min(kWindow, begin - counted);
      const char *newline = static_cast<const char *>(
          std::memchr(input + counted, '\n', count));
      if (newline) {
        counted = static_cast<size_t>(newline - input) + 1;
        bol = counted;
        ++line;
      } else {
        counted += count;
      }
    }
    if (cached_eol < end || cached_eol < begin ||
        (cached_eol == 0 && bytes != 0)) {
      status = end_of_line(end, &cached_eol);
      if (status != Ok)
        return status;
    }
    return emit(context, line, static_cast<uint64_t>(begin), input + bol,
                cached_eol - bol);
  }
};

int scan_lines(Output &out, Lines selection) {
  size_t begin = 0;
  uint64_t line = 1;
  // There is no synthetic empty line after a final LF and no line in an empty
  // file. This is the behavior of ugrep's special-case LineMatcher.
  while (begin < out.bytes) {
    size_t end;
    int status = out.end_of_line(begin, &end);
    if (status != Ok)
      return status;
    if (selection == AllLines ||
        (selection == EmptyLines && begin == end) ||
        (selection == NonemptyLines && begin != end)) {
      status = out.emit(out.context, line, static_cast<uint64_t>(begin),
                        out.input + begin, end - begin);
      if (status != Ok)
        return status;
    }
    begin = end < out.bytes ? end + 1 : end;
    ++line;
  }
  return stop(out.matcher);
}

int scan_literal(Output &out) {
  const FsMatcher *compiled = out.matcher;
  const size_t overlap = compiled->literal_bytes - 1;
  HostBuffer scratch(compiled->host,
                     std::min(out.bytes, kWindow + overlap) + 1);
  if (!scratch.data)
    return NoMemory;
  reflex::Matcher matcher(*compiled->literal);
  size_t base = 0;
  while (base < out.bytes) {
    int status = stop(compiled);
    if (status != Ok)
      return status;
    size_t boundary = base + std::min(kWindow, out.bytes - base);
    size_t end = boundary + std::min(overlap, out.bytes - boundary);
    size_t count = end - base;
    std::memcpy(scratch.data, out.input + base, count);
    scratch.data[count] = '\0';
    matcher.buffer(scratch.data, count + 1);
    size_t next = boundary;
    while (matcher.find()) {
      size_t begin_match = base + matcher.first();
      size_t end_match = base + matcher.last();
      // A following window owns matches that start in this lookahead region.
      if (begin_match >= boundary)
        break;
      status = out.match(begin_match, end_match);
      if (status != Ok)
        return status;
      // Preserve ordinary non-overlapping matching when a match straddles the
      // boundary: do not search inside that match again in the next window.
      next = std::max(next, end_match);
    }
    base = next;
  }
  return stop(compiled);
}

struct PcreState {
  const FsMatcher *matcher;
  int status;
  uint32_t calls;
};

int callout(pcre2_callout_block *, void *context) {
  PcreState *state = static_cast<PcreState *>(context);
  // The compiled pattern has automatic callouts and disables start
  // optimizations. Check at the first and every 256th callout, as well as before
  // and after each match operation.
  if ((state->calls++ & 255u) == 0) {
    state->status = stop(state->matcher);
    if (state->status != Ok)
      return PCRE2_ERROR_CALLOUT;
  }
  return 0;
}

struct PcreScan {
  pcre2_match_data *data;
  pcre2_match_context *context;
  explicit PcreScan(const FsMatcher *matcher)
      : data(pcre2_match_data_create_from_pattern(matcher->code,
                                                  matcher->general)),
        context(pcre2_match_context_create(matcher->general)) {}
  ~PcreScan() {
    pcre2_match_data_free(data);
    pcre2_match_context_free(context);
  }
  PcreScan(const PcreScan &) = delete;
  PcreScan &operator=(const PcreScan &) = delete;
};

int scan_pcre(Output &out, char *error, size_t error_capacity) {
  const FsMatcher *matcher = out.matcher;
  PcreScan scan(matcher);
  if (!scan.data || !scan.context)
    return NoMemory;
  PcreState state = {matcher, Ok, 0};
  pcre2_set_callout(scan.context, callout, &state);
  pcre2_set_match_limit(scan.context, 1000000);
  pcre2_set_depth_limit(scan.context, 256);
  pcre2_set_heap_limit(scan.context, 32 * 1024);
  uint32_t base_options = 0;
  if (!matcher->empty_matches)
    base_options |= PCRE2_NOTEMPTY;
  uint32_t options = base_options;
  size_t start = 0;
  while (start <= out.bytes) {
    int status = stop(matcher);
    if (status != Ok)
      return status;
    int result = pcre2_match(matcher->code,
                            reinterpret_cast<PCRE2_SPTR>(out.input), out.bytes,
                            start, options, scan.data, scan.context);
    if (state.status != Ok)
      return state.status;
    status = stop(matcher);
    if (status != Ok)
      return status;
    if (result == PCRE2_ERROR_NOMATCH) {
      if ((options & PCRE2_ANCHORED) == 0 || start == out.bytes)
        return Ok;
      // After an empty match, first try a nonempty alternative at the same
      // position. If none exists, advance one complete UTF-8 scalar.
      ++start;
      while (start < out.bytes &&
             (static_cast<unsigned char>(out.input[start]) & 0xc0) == 0x80)
        ++start;
      options = base_options;
      continue;
    }
    if (result < 0) {
      PCRE2_UCHAR message[160] = {0};
      pcre2_get_error_message(result, message, sizeof(message));
      diagnostic(error, error_capacity,
                 reinterpret_cast<const char *>(message));
      if (result == PCRE2_ERROR_NOMEMORY)
        return NoMemory;
      if (result == PCRE2_ERROR_MATCHLIMIT ||
          result == PCRE2_ERROR_DEPTHLIMIT ||
          result == PCRE2_ERROR_HEAPLIMIT)
        return Limit;
      return Invalid;
    }
    PCRE2_SIZE *offsets = pcre2_get_ovector_pointer(scan.data);
    size_t begin = static_cast<size_t>(offsets[0]);
    size_t end = static_cast<size_t>(offsets[1]);
    // RE/flex's PCRE2Matcher suppresses empty matches at end of input.
    if (begin == end && end == out.bytes)
      return Ok;
    status = out.match(begin, end);
    if (status != Ok)
      return status;
    start = end;
    options = base_options;
    if (begin == end)
      options |= PCRE2_NOTEMPTY_ATSTART | PCRE2_ANCHORED;
  }
  return Ok;
}

int compile_pattern(FsMatcher *matcher, const char *pattern, char *error,
                    size_t error_capacity) {
  size_t length = std::strlen(pattern);
  if (!length) {
    matcher->lines = AllLines;
    return Ok;
  }
  if (matcher->mode == Literal) {
    // Hex escaping keeps every UTF-8 byte literal, including metacharacters,
    // backslashes, and the \Q/\E quote delimiters themselves.
    static const char hex[] = "0123456789abcdef";
    std::string quoted;
    quoted.reserve(length * 4);
    for (size_t i = 0; i < length; ++i) {
      unsigned char c = static_cast<unsigned char>(pattern[i]);
      quoted.append("\\x");
      quoted.push_back(hex[c >> 4]);
      quoted.push_back(hex[c & 15]);
    }
    matcher->literal_bytes = length;
    matcher->literal = new reflex::Pattern(quoted, "r");
    return stop(matcher);
  }
  if (!std::strcmp(pattern, "^") || !std::strcmp(pattern, "$") ||
      !std::strcmp(pattern, ".*"))
    matcher->lines = AllLines;
  else if (!std::strcmp(pattern, "^$"))
    matcher->lines = EmptyLines;
  else if (!std::strcmp(pattern, ".+"))
    matcher->lines = NonemptyLines;
  if (matcher->lines != Matches)
    return Ok;
  matcher->empty_matches = pattern[0] == '^' || pattern[length - 1] == '$';

  // Match ugrep -P's conversion: multiline anchors; Unicode classes; \s and
  // negated classes do not cross LF unless the expression opts into it.
  std::string expression("(?m)");
  expression.append(pattern);
  expression = reflex::PCRE2UTFMatcher::convert(
      expression, reflex::convert_flag::unicode |
                      reflex::convert_flag::notnewline);
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
  int code = 0;
  PCRE2_SIZE offset = 0;
  matcher->code = pcre2_compile(
      reinterpret_cast<PCRE2_SPTR>(expression.data()), expression.size(),
      // Like ugrep's PCRE2Matcher, permit safe continuation after a byte-wise
      // \C match ends inside a UTF-8 scalar. Subject validity is still checked
      // by FSX before this function; MATCH_INVALID_UTF avoids undefined offset
      // behavior that NO_UTF_CHECK alone would permit on the following match.
      PCRE2_UTF | PCRE2_UCP | PCRE2_MATCH_INVALID_UTF | PCRE2_AUTO_CALLOUT |
          PCRE2_NO_START_OPTIMIZE,
      &code, &offset, context);
  pcre2_compile_context_free(context);
  if (!matcher->code) {
    PCRE2_UCHAR message[160] = {0};
    pcre2_get_error_message(code, message, sizeof(message));
    if (error && error_capacity)
      std::snprintf(error, error_capacity, "PCRE2 at byte %zu: %s",
                    static_cast<size_t>(offset),
                    reinterpret_cast<const char *>(message));
    return code == PCRE2_ERROR_HEAP_FAILED || code == PCRE2_ERROR_NOMEMORY
               ? NoMemory
               : Invalid;
  }
  return stop(matcher);
}
} // namespace

extern "C" int fsx_matcher_compile(const FsMatcherHost *host,
                                    const char *pattern, unsigned mode,
                                    FsMatcher **out, char *error,
                                    size_t error_capacity) {
  diagnostic(error, error_capacity, "");
  if (!out)
    return Invalid;
  *out = NULL;
  if (!host || !host->alloc || !host->free || !host->stop || !pattern ||
      mode > Pcre2 || std::strlen(pattern) > kMaxPattern ||
      std::strchr(pattern, '\n'))
    return Invalid;
  int status = host->stop(host->context);
  if (status != Ok)
    return status;
  void *storage = host->alloc(host->context, sizeof(FsMatcher));
  if (!storage)
    return NoMemory;
  FsMatcher *matcher = new (storage) FsMatcher(*host, mode);
  try {
    status = compile_pattern(matcher, pattern, error, error_capacity);
  } catch (const std::bad_alloc &) {
    diagnostic(error, error_capacity, "native matcher allocation failed");
    status = NoMemory;
  } catch (const reflex::regex_error &exception) {
    diagnostic(error, error_capacity, exception.what());
    status = Invalid;
  } catch (const std::exception &exception) {
    diagnostic(error, error_capacity, exception.what());
    status = Io;
  } catch (...) {
    diagnostic(error, error_capacity, "native matcher compilation failed");
    status = Io;
  }
  if (status == Ok)
    *out = matcher;
  else
    fsx_matcher_destroy(matcher);
  return status;
}

extern "C" int fsx_matcher_scan(const FsMatcher *matcher, const char *input,
                                 size_t bytes, FsMatcherEmit emit, void *context,
                                 char *error, size_t error_capacity) {
  diagnostic(error, error_capacity, "");
  if (!matcher || (!input && bytes) || !emit)
    return Invalid;
  if (bytes > kMaxInput)
    return Limit;
  int status = stop(matcher);
  if (status != Ok || !bytes)
    return status;
  try {
    Output output(matcher, input, bytes, emit, context);
    if (matcher->lines != Matches)
      return scan_lines(output, matcher->lines);
    if (matcher->mode == Literal)
      return scan_literal(output);
    return scan_pcre(output, error, error_capacity);
  } catch (const std::bad_alloc &) {
    diagnostic(error, error_capacity, "native matcher allocation failed");
    return NoMemory;
  } catch (const std::exception &exception) {
    diagnostic(error, error_capacity, exception.what());
    return Io;
  } catch (...) {
    diagnostic(error, error_capacity, "native matcher scan failed");
    return Io;
  }
}

extern "C" void fsx_matcher_destroy(FsMatcher *matcher) {
  if (matcher) {
    FsMatcherHost host = matcher->host;
    matcher->~FsMatcher();
    host.free(host.context, matcher);
  }
}

#define PCRE2_CODE_UNIT_WIDTH 8
#include "internal.h"
#include "lexer.h"
#include <pcre2.h>
#include <stdio.h>
#include <stdlib.h>

enum {
  N_ALL,
  N_TEXT,
  N_GLOB,
  N_REGEX,
  N_EXT,
  N_SIZE,
  N_DATE,
  N_FILE,
  N_FOLDER,
  N_AND,
  N_OR,
  N_NOT
};
enum { F_CASE = 1, F_PATH = 2, F_BYTES = 4 };
typedef struct Node {
  int kind, left, right, height, flags, comparison;
  char *text;
  uint64_t low, high;
  pcre2_code *code;
  pcre2_match_data *match;
  size_t jit_bytes;
  bool jit;
} Node;
typedef struct Plan {
  FsQuery *q;
  FsToken token;
  const char *cursor;
  Node nodes[1024];
  int count, root, depth;
  FsStatus status;
  bool has_regex, all_jit;
  size_t *offset_map;
  const char *decoded;
  size_t decoded_size;
  pcre2_general_context *general;
  pcre2_match_context *match_context;
  pcre2_jit_stack *jit_stack;
  bool stack_charged;
} Plan;
static void error(Plan *p, FsStatus status, size_t at, const char *message) {
  if (p->status != FS_OK)
    return;
  p->status = status;
  cmeta_mutex_lock(&p->q->mutex);
  p->q->info.error_offset = at;
  snprintf(p->q->info.diagnostic, sizeof(p->q->info.diagnostic), "%s", message);
  cmeta_mutex_unlock(&p->q->mutex);
}
static size_t position(Plan *p) {
  return (size_t)(p->token.begin - p->q->text);
}
static void next(Plan *p) {
  fsx_lex(&p->cursor, &p->token);
  if (p->token.kind == FS_T_INVALID)
    error(p, FS_INVALID, position(p), "unterminated quote or invalid token");
}
static int node(Plan *p, int kind, int left, int right) {
  if (p->status != FS_OK)
    return -1;
  int height = 1;
  if (left >= 0 && p->nodes[left].height + 1 > height)
    height = p->nodes[left].height + 1;
  if (right >= 0 && p->nodes[right].height + 1 > height)
    height = p->nodes[right].height + 1;
  if (p->count >= 1024 || height > 32) {
    error(p, FS_LIMIT, position(p), "query AST limit exceeded");
    return -1;
  }
  int index = p->count++;
  p->nodes[index] = (Node){0};
  p->nodes[index].kind = kind;
  p->nodes[index].left = left;
  p->nodes[index].right = right;
  p->nodes[index].height = height;
  return index;
}
static void *regex_alloc(PCRE2_SIZE n, void *ctx) { return fsx_alloc(ctx, n); }
static void regex_free(void *p, void *ctx) {
  (void)ctx;
  fsx_free(p);
}
static int callout(pcre2_callout_block *b, void *ctx) {
  (void)b;
  return fsx_query_stop(ctx) == FS_OK ? 0 : PCRE2_ERROR_CALLOUT;
}
static bool size_value(const char *text, uint64_t *out) {
  uint64_t n = 0;
  const char *at = text;
  while (*at >= '0' && *at <= '9') {
    unsigned digit = (unsigned)(*at++ - '0');
    if (n > (UINT64_MAX - digit) / 10)
      return false;
    n = n * 10 + digit;
  }
  if (at == text)
    return false;
  uint64_t scale = 1;
  if (!strcmp(at, "B") || !*at)
    scale = 1;
  else if (!strcmp(at, "KB"))
    scale = 1000;
  else if (!strcmp(at, "MB"))
    scale = 1000000;
  else if (!strcmp(at, "GB"))
    scale = 1000000000;
  else if (!strcmp(at, "KiB"))
    scale = 1024;
  else if (!strcmp(at, "MiB"))
    scale = 1048576;
  else if (!strcmp(at, "GiB"))
    scale = 1073741824;
  else
    return false;
  if (n > UINT64_MAX / scale)
    return false;
  *out = n * scale;
  return true;
}
/* Gregorian civil-day conversion. The arithmetic is bounded to 1601..9999. */
static bool date_value(const char *s, int tz, uint64_t *low, uint64_t *high) {
  if (strlen(s) != 10 || s[4] != '-' || s[7] != '-')
    return false;
  for (int i = 0; i < 10; i++)
    if (i != 4 && i != 7 && (s[i] < '0' || s[i] > '9'))
      return false;
  int y =
      (s[0] - '0') * 1000 + (s[1] - '0') * 100 + (s[2] - '0') * 10 + s[3] - '0';
  int m = (s[5] - '0') * 10 + s[6] - '0', d = (s[8] - '0') * 10 + s[9] - '0';
  static const int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (y < 1601 || y > 9999 || m < 1 || m > 12 || d < 1 ||
      d > days[m - 1] + (m == 2 && (!(y % 4) && (y % 100 || !(y % 400)))))
    return false;
  y -= m <= 2;
  int era = y / 400, yoe = y - era * 400;
  int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  int64_t unix_days =
      (int64_t)era * 146097 + yoe * 365 + yoe / 4 - yoe / 100 + doy - 719468;
  int64_t sec = unix_days * 86400 + 11644473600LL - (int64_t)tz * 60;
  if (sec < 0)
    return false;
  *low = (uint64_t)sec * 10000000ULL;
  *high = *low + 864000000000ULL;
  return true;
}
static bool prefix(const char *s, const char *word) {
  return strncmp(s, word, strlen(word)) == 0;
}
static int atom(Plan *p, const char *text, int flags, size_t offset) {
  while (prefix(text, "case:") || prefix(text, "path:")) {
    if (prefix(text, "case:")) {
      flags |= F_CASE;
      text += 5;
    } else {
      flags |= F_PATH;
      text += 5;
    }
  }
  int kind = N_TEXT;
  const char *value = text;
  if (prefix(text, "file:") || prefix(text, "folder:")) {
    bool directory = prefix(text, "folder:");
    value = text + (directory ? 7 : 5);
    int a = node(p, directory ? N_FOLDER : N_FILE, -1, -1);
    if (!*value)
      return a;
    int b = atom(p, value, flags, offset + (size_t)(value - text));
    return node(p, N_AND, a, b);
  }
  if (prefix(text, "regex:")) {
    kind = N_REGEX;
    value = text + 6;
  } else if (prefix(text, "regexb:")) {
    kind = N_REGEX;
    flags |= F_BYTES;
    value = text + 7;
  } else if (prefix(text, "ext:")) {
    kind = N_EXT;
    value = text + 4;
  } else if (prefix(text, "size:")) {
    kind = N_SIZE;
    value = text + 5;
  } else if (prefix(text, "dm:")) {
    kind = N_DATE;
    value = text + 3;
  } else if (strchr(text, ':') && !(strlen(text) > 2 && text[1] == ':' &&
                                    (text[2] == '/' || text[2] == '\\'))) {
    error(p, FS_UNSUPPORTED, offset, "unknown or unsupported query function");
    return -1;
  } else if (strlen(text) > 2 && text[1] == ':' &&
             (text[2] == '/' || text[2] == '\\'))
    flags |= F_PATH;
  int id = node(p, kind, -1, -1);
  if (id < 0)
    return -1;
  Node *n = &p->nodes[id];
  n->flags = flags;
  if (!*value) {
    error(p, FS_INVALID, offset, "empty query operand");
    return -1;
  }
  if (kind == N_SIZE) {
    n->comparison = 0;
    if (*value == '<' || *value == '>') {
      n->comparison = *value++ == '<' ? -1 : 1;
      if (*value == '=') {
        n->comparison *= 2;
        value++;
      }
    } else if (*value == '=')
      value++;
    if (!size_value(value, &n->low))
      error(p, FS_INVALID, offset, "invalid or overflowing size");
  } else if (kind == N_DATE) {
    if (!date_value(value, p->q->options.timezone_offset_minutes, &n->low,
                    &n->high))
      error(p, FS_INVALID, offset, "invalid date or timezone range");
  } else {
    if (kind == N_REGEX && strlen(value) > 4096) {
      error(p, FS_LIMIT, offset, "regex pattern exceeds 4096 bytes");
      return -1;
    }
    n->text = fsx_copy(p->q->engine, value, strlen(value));
    if (!n->text) {
      error(p, FS_NOMEM, offset, "query allocation budget exceeded");
      return -1;
    }
    if (kind != N_REGEX) {
      if (flags & F_PATH)
        for (char *c = n->text; *c; c++)
          if (*c == '\\')
            *c = '/';
      if (!(flags & F_CASE)) {
        char *fold = NULL;
        FsStatus st = fsx_fold(p->q->engine, n->text, &fold);
        if (st != FS_OK) {
          error(p, st, offset, "invalid query text or allocation failure");
          return -1;
        }
        fsx_free(n->text);
        n->text = fold;
      }
      if (kind == N_TEXT && (strchr(n->text, '*') || strchr(n->text, '?')))
        n->kind = N_GLOB;
      if (kind == N_EXT &&
          (n->text[0] == ';' || n->text[strlen(n->text) - 1] == ';' ||
           strstr(n->text, ";;")))
        error(p, FS_INVALID, offset, "empty extension in set");
    } else {
      int code = 0;
      PCRE2_SIZE at = 0;
      pcre2_compile_context *cc = pcre2_compile_context_create(p->general);
      if (!cc) {
        error(p, FS_NOMEM, offset, "regex compiler allocation failed");
        return -1;
      }
      pcre2_set_parens_nest_limit(cc, 32);
      uint32_t options = PCRE2_AUTO_CALLOUT;
      options |= flags & F_BYTES ? (PCRE2_NEVER_UTF | PCRE2_NEVER_UCP)
                                 : (PCRE2_UTF | PCRE2_UCP);
      if (!(flags & F_CASE))
        options |= PCRE2_CASELESS;
      n->code = pcre2_compile((PCRE2_SPTR)n->text, strlen(n->text), options,
                              &code, &at, cc);
      pcre2_compile_context_free(cc);
      if (!n->code) {
        char message[160];
        pcre2_get_error_message(code, (PCRE2_UCHAR *)message, sizeof(message));
        size_t decoded_at = (size_t)(value - p->decoded) + (size_t)at;
        size_t source_at =
            decoded_at <= p->decoded_size ? p->offset_map[decoded_at] : offset;
        error(p, code == PCRE2_ERROR_HEAP_FAILED ? FS_NOMEM : FS_INVALID,
              source_at, message);
        return -1;
      }
      uint32_t captures = 0;
      pcre2_pattern_info(n->code, PCRE2_INFO_CAPTURECOUNT, &captures);
      if (captures > 256) {
        error(p, FS_LIMIT, offset, "regex capture limit exceeded");
        return -1;
      }
      n->match = pcre2_match_data_create_from_pattern(n->code, p->general);
      if (!n->match) {
        error(p, FS_NOMEM, offset, "regex match allocation failed");
        return -1;
      }
      p->has_regex = true;
      if (p->q->engine->config.flags & FS_ENGINE_REGEX_JIT) {
        int rc = pcre2_jit_compile(n->code, PCRE2_JIT_COMPLETE);
        if (rc == 0) {
          pcre2_pattern_info(n->code, PCRE2_INFO_JITSIZE, &n->jit_bytes);
          if (!fsx_reserve(p->q->engine, n->jit_bytes)) {
            n->jit_bytes = 0;
            error(p, FS_NOMEM, offset, "JIT memory budget exceeded");
            return -1;
          }
          n->jit = n->jit_bytes != 0;
        } else if (rc != PCRE2_ERROR_JIT_BADOPTION) {
          error(p, FS_NOMEM, offset, "JIT compilation failed");
          return -1;
        }
      }
      if (!n->jit)
        p->all_jit = false;
    }
  }
  return id;
}
static char *decode_token(Plan *p) {
  char *word = fsx_alloc(p->q->engine, p->token.length + 1);
  if (!word)
    return NULL;
  p->offset_map =
      fsx_alloc(p->q->engine, (p->token.length + 1) * sizeof(size_t));
  if (!p->offset_map) {
    fsx_free(word);
    return NULL;
  }
  p->offset_map[0] = position(p);
  bool quote = false;
  size_t k = 0;
  for (size_t i = 0; i < p->token.length; i++) {
    char c = p->token.begin[i];
    size_t source_at = position(p) + i;
    if (c == '"') {
      quote = !quote;
      continue;
    }
    if (quote && c == '\\' && i + 1 < p->token.length &&
        (p->token.begin[i + 1] == '"' || p->token.begin[i + 1] == '\\'))
      c = p->token.begin[++i];
    p->offset_map[k] = source_at;
    word[k++] = c;
    p->offset_map[k] = position(p) + i + 1;
  }
  word[k] = 0;
  p->decoded = word;
  p->decoded_size = k;
  return word;
}
static int expression(Plan *, int);
static int unary(Plan *p, int flags) {
  if (p->status != FS_OK)
    return -1;
  if (++p->depth > 32) {
    error(p, FS_LIMIT, position(p), "query nesting limit exceeded");
    --p->depth;
    return -1;
  }
  int id = -1;
  if (p->token.kind == FS_T_NOT) {
    next(p);
    id = node(p, N_NOT, unary(p, flags), -1);
  } else if (p->token.kind == FS_T_OPEN) {
    next(p);
    id = expression(p, flags);
    if (p->token.kind != FS_T_CLOSE)
      error(p, FS_INVALID, position(p), "missing closing >");
    else
      next(p);
  } else if (p->token.kind == FS_T_WORD) {
    size_t at = position(p);
    char *word = decode_token(p);
    if (!word)
      error(p, FS_NOMEM, at, "query allocation failed");
    else {
      const char *rest = word;
      int inherited = flags;
      while (prefix(rest, "case:") || prefix(rest, "path:")) {
        if (prefix(rest, "case:")) {
          inherited |= F_CASE;
          rest += 5;
        } else {
          inherited |= F_PATH;
          rest += 5;
        }
      }
      bool modifier = rest != word && !*rest;
      next(p);
      if (modifier) {
        fsx_free(p->offset_map);
        p->offset_map = NULL;
        id = unary(p, inherited);
      } else
        id = atom(p, word, flags, at);
      fsx_free(p->offset_map);
      p->offset_map = NULL;
      fsx_free(word);
    }
  } else
    error(p, FS_INVALID, position(p), "expected query operand");
  --p->depth;
  return id;
}
static int conjunction(Plan *p, int flags) {
  int left = unary(p, flags);
  while (p->status == FS_OK &&
         (p->token.kind == FS_T_WORD || p->token.kind == FS_T_NOT ||
          p->token.kind == FS_T_OPEN)) {
    int right = unary(p, flags);
    left = node(p, N_AND, left, right);
  }
  return left;
}
static int expression(Plan *p, int flags) {
  int left = conjunction(p, flags);
  while (p->status == FS_OK && p->token.kind == FS_T_OR) {
    next(p);
    int right = conjunction(p, flags);
    left = node(p, N_OR, left, right);
  }
  return left;
}
static void destroy(Plan *p) {
  if (!p)
    return;
  for (int i = 0; i < p->count; i++) {
    Node *n = &p->nodes[i];
    pcre2_match_data_free(n->match);
    pcre2_code_free(n->code);
    if (n->jit_bytes)
      fsx_unreserve(p->q->engine, n->jit_bytes);
    fsx_free(n->text);
  }
  fsx_free(p->offset_map);
  pcre2_jit_stack_free(p->jit_stack);
  if (p->stack_charged)
    fsx_unreserve(p->q->engine, 256 * 1024);
  pcre2_match_context_free(p->match_context);
  pcre2_general_context_free(p->general);
  fsx_free(p);
}
static size_t scalar_end(const char *s, size_t at, size_t n) {
  salts_unicode_scalar cp;
  return salts_unicode_utf8_next(vstr_from_buf(s, n), &at, &cp) ==
                 SALTS_UNICODE_OK
             ? at
             : n;
}
static FsStatus glob(Plan *plan, const Node *n, const char *s, bool *out) {
  const char *pattern = n->text;
  size_t pi = 0, si = 0, star = SIZE_MAX, retry = 0;
  size_t slen = strlen(s), plen = strlen(pattern);
  uint32_t steps = 0;
  while (si < slen) {
    if (++steps > 1000000)
      return FS_LIMIT;
    if ((steps & 255u) == 0) {
      FsStatus stop = fsx_query_stop(plan->q);
      if (stop != FS_OK)
        return stop;
    }
    if (pi < plen && pattern[pi] == '*') {
      star = ++pi;
      retry = si;
      continue;
    }
    if (pi < plen && pattern[pi] == '?' &&
        (!(n->flags & F_PATH) || s[si] != '/')) {
      pi++;
      si = scalar_end(s, si, slen);
      continue;
    }
    size_t end = scalar_end(s, si, slen), bytes = end - si;
    const char *unit = s + si;
    char folded[16];
    size_t folded_bytes = 0;
    if (!(n->flags & F_CASE)) {
      if (salts_unicode_case_fold(vstr_from_buf(unit, bytes), folded,
                                  sizeof(folded),
                                  &folded_bytes) != SALTS_UNICODE_OK)
        return FS_INVALID;
      unit = folded;
      bytes = folded_bytes;
    }
    bool literal = bytes <= plen - pi && memcmp(pattern + pi, unit, bytes) == 0;
    for (size_t j = 0; literal && j < bytes; j++)
      if (pattern[pi + j] == '*' || pattern[pi + j] == '?')
        literal = false;
    if (literal) {
      pi += bytes;
      si = end;
      continue;
    }
    if (star != SIZE_MAX && retry < slen &&
        (!(n->flags & F_PATH) || s[retry] != '/')) {
      retry = scalar_end(s, retry, slen);
      si = retry;
      pi = star;
      continue;
    }
    *out = false;
    return FS_OK;
  }
  while (pi < plen && pattern[pi] == '*')
    pi++;
  *out = pi == plen;
  return FS_OK;
}
static FsStatus evaluate(Plan *p, int id, const FsEntry *entry, bool *out) {
  Node *n = &p->nodes[id];
  bool left = false, right = false;
  FsStatus status;
  if (n->kind == N_AND || n->kind == N_OR || n->kind == N_NOT) {
    status = evaluate(p, n->left, entry, &left);
    if (status != FS_OK)
      return status;
    if (n->kind == N_NOT) {
      *out = !left;
      return FS_OK;
    }
    if (n->kind == N_AND && !left) {
      *out = false;
      return FS_OK;
    }
    if (n->kind == N_OR && left) {
      *out = true;
      return FS_OK;
    }
    status = evaluate(p, n->right, entry, &right);
    *out = right;
    return status;
  }
  *out = false;
  switch (n->kind) {
  case N_ALL:
    *out = true;
    return FS_OK;
  case N_FILE:
    *out = !(entry->flags & FS_ENTRY_DIRECTORY);
    return FS_OK;
  case N_FOLDER:
    *out = (entry->flags & FS_ENTRY_DIRECTORY) != 0;
    return FS_OK;
  case N_SIZE:
    if (!(entry->flags & FS_ENTRY_SIZE_VALID))
      return FS_OK;
    switch (n->comparison) {
    case -2:
      *out = entry->size <= n->low;
      break;
    case -1:
      *out = entry->size < n->low;
      break;
    case 1:
      *out = entry->size > n->low;
      break;
    case 2:
      *out = entry->size >= n->low;
      break;
    default:
      *out = entry->size == n->low;
      break;
    }
    return FS_OK;
  case N_DATE:
    *out = (entry->flags & FS_ENTRY_MTIME_VALID) && entry->mtime >= n->low &&
           entry->mtime < n->high;
    return FS_OK;
  default:
    break;
  }
  const char *raw = n->flags & F_PATH ? entry->path : entry->name;
  const char *subject =
      n->flags & F_CASE
          ? raw
          : (n->flags & F_PATH ? entry->path_fold : entry->name_fold);
  if (n->kind == N_TEXT) {
    *out = strstr(subject, n->text) != NULL;
    return FS_OK;
  }
  if (n->kind == N_GLOB)
    return glob(p, n, raw, out);
  if (n->kind == N_EXT) {
    const char *name = n->flags & F_CASE ? entry->name : entry->name_fold;
    const char *dot = strrchr(name, '.');
    const char *ext = dot && dot != name ? dot + 1 : "";
    size_t length = strlen(ext);
    for (const char *a = n->text; *a;) {
      const char *b = strchr(a, ';');
      size_t nbytes = b ? (size_t)(b - a) : strlen(a);
      if (nbytes == length && !memcmp(ext, a, length)) {
        *out = true;
        break;
      }
      if (!b)
        break;
      a = b + 1;
    }
    return FS_OK;
  }
  if (n->kind == N_REGEX) {
    size_t length = strlen(raw);
    if (length > 65536)
      return FS_LIMIT;
    int rc =
        pcre2_match(n->code, (PCRE2_SPTR)raw, length, 0,
                    p->all_jit ? 0 : PCRE2_NO_JIT, n->match, p->match_context);
    if (rc >= 0) {
      *out = true;
      return FS_OK;
    }
    if (rc == PCRE2_ERROR_NOMATCH)
      return FS_OK;
    FsStatus stop = fsx_query_stop(p->q);
    if (stop != FS_OK)
      return stop;
    if (rc == PCRE2_ERROR_MATCHLIMIT || rc == PCRE2_ERROR_DEPTHLIMIT ||
        rc == PCRE2_ERROR_HEAPLIMIT || rc == PCRE2_ERROR_JIT_STACKLIMIT)
      return FS_LIMIT;
    if (rc == PCRE2_ERROR_NOMEMORY)
      return FS_NOMEM;
    return FS_IO;
  }
  return FS_INVALID;
}
/* All comparators use a stable entry-id tie break, including descending sorts.
 */
static int compare(FsQuery *q, uint32_t a, uint32_t b) {
  const FsEntry *x = fsx_snapshot_entry(q->snapshot, a),
                *y = fsx_snapshot_entry(q->snapshot, b);
  int c = 0;
  uint32_t valid = q->options.sort_key == FS_SORT_SIZE ? FS_ENTRY_SIZE_VALID
                                                       : FS_ENTRY_MTIME_VALID;
  if (q->options.sort_key == FS_SORT_NAME)
    c = strcmp(x->name_fold, y->name_fold);
  else if (q->options.sort_key == FS_SORT_PATH)
    c = strcmp(x->path_fold, y->path_fold);
  else {
    bool xv = (x->flags & valid) != 0, yv = (y->flags & valid) != 0;
    if (xv != yv)
      return xv ? -1 : 1;
    uint64_t av = q->options.sort_key == FS_SORT_SIZE ? x->size : x->mtime;
    uint64_t bv = q->options.sort_key == FS_SORT_SIZE ? y->size : y->mtime;
    if (xv)
      c = (av > bv) - (av < bv);
  }
  if (c)
    return q->options.flags & FS_QUERY_DESCENDING ? (c > 0 ? -1 : 1)
                                                  : (c > 0 ? 1 : -1);
  return (a > b) - (a < b);
}
static void sift(FsQuery *q, uint32_t *ids, uint32_t count, uint32_t index) {
  for (;;) {
    uint32_t left = index * 2 + 1;
    if (left >= count)
      break;
    uint32_t worst = left;
    if (left + 1 < count && compare(q, ids[left + 1], ids[left]) > 0)
      worst = left + 1;
    if (compare(q, ids[index], ids[worst]) >= 0)
      break;
    uint32_t swap = ids[index];
    ids[index] = ids[worst];
    ids[worst] = swap;
    index = worst;
  }
}
/* Selected nodes describe a superset of all matches. In particular, an OR
 * cannot discard an unindexed branch, and NOT cannot use positive postings. */
static int candidate_nodes(Plan *p, int selected[1024]) {
  for (int i = 0; i < p->count; i++) {
    Node *n = &p->nodes[i];
    selected[i] = -1;
    if (n->kind == N_TEXT && !(n->flags & F_PATH)) {
      unsigned scalars = 0;
      for (const unsigned char *s = (const unsigned char *)n->text; *s; s++)
        scalars += (*s & 0xc0) != 0x80;
      if (scalars >= 3)
        selected[i] = i;
    } else if (n->kind == N_AND || n->kind == N_OR) {
      int left = selected[n->left], right = selected[n->right];
      if (left >= 0 && right >= 0)
        selected[i] = i;
      else if (n->kind == N_AND)
        selected[i] = left >= 0 ? left : right;
    }
  }
  return selected[p->root];
}
/* The same bounded writer measures first, then emits. Phrase quotes are
 * doubled; user text never becomes either SQL syntax or FTS operators. */
typedef struct CandidateText {
  char *bytes;
  size_t length;
  bool overflow;
} CandidateText;
static void candidate_put(CandidateText *out, char c) {
  if (out->length == 512 * 1024) {
    out->overflow = true;
    return;
  }
  if (out->bytes)
    out->bytes[out->length] = c;
  out->length++;
}
static void candidate_string(CandidateText *out, const char *s) {
  for (; *s; s++)
    candidate_put(out, *s);
}
static void candidate_emit(Plan *p, const int selected[1024], int index,
                           CandidateText *out) {
  Node *n = &p->nodes[index];
  if (n->kind == N_TEXT) {
    candidate_string(out, n->flags & F_CASE ? "name : \"" : "name_fold : \"");
    for (const char *s = n->text; *s; s++) {
      candidate_put(out, *s);
      if (*s == '"')
        candidate_put(out, '"');
    }
    candidate_put(out, '"');
  } else {
    candidate_put(out, '(');
    candidate_emit(p, selected, selected[n->left], out);
    candidate_string(out, n->kind == N_AND ? " AND " : " OR ");
    candidate_emit(p, selected, selected[n->right], out);
    candidate_put(out, ')');
  }
}
static FsStatus candidate_expression(Plan *p, char **expression) {
  *expression = NULL;
  if (p->q->engine->config.flags & FS_ENGINE_SCAN_ONLY)
    return FS_OK;
  int selected[1024];
  int root = candidate_nodes(p, selected);
  if (root < 0)
    return FS_OK;
  CandidateText out = {0};
  candidate_emit(p, selected, root, &out);
  if (out.overflow)
    return FS_LIMIT;
  char *bytes = fsx_alloc(p->q->engine, out.length + 1);
  if (!bytes)
    return FS_NOMEM;
  out.bytes = bytes;
  out.length = 0;
  candidate_emit(p, selected, root, &out);
  bytes[out.length] = 0;
  *expression = bytes;
  return FS_OK;
}
FsStatus fsx_query_execute(FsQuery *q, FsResult **out) {
  *out = NULL;
  FsEngine *e = q->engine;
  Plan *p = fsx_alloc(e, sizeof(*p));
  if (!p)
    return FS_NOMEM;
  memset(p, 0, sizeof(*p));
  p->q = q;
  p->cursor = q->text;
  p->all_jit = true;
  p->general = pcre2_general_context_create(regex_alloc, regex_free, e);
  if (!p->general) {
    destroy(p);
    return FS_NOMEM;
  }
  p->match_context = pcre2_match_context_create(p->general);
  if (!p->match_context) {
    destroy(p);
    return FS_NOMEM;
  }
  pcre2_set_match_limit(p->match_context, 100000);
  pcre2_set_depth_limit(p->match_context, 256);
  pcre2_set_heap_limit(p->match_context, 1024);
  pcre2_set_callout(p->match_context, callout, q);
  next(p);
  p->root =
      p->token.kind == FS_T_END
          ? node(p, N_ALL, -1, -1)
          : expression(p,
                       q->options.flags & FS_QUERY_CASE_SENSITIVE ? F_CASE : 0);
  if (p->token.kind != FS_T_END)
    error(p, FS_INVALID, position(p), "unexpected query token");
  if (p->status == FS_OK && p->has_regex && q->snapshot->unsupported &&
      !(q->options.flags & FS_QUERY_ALLOW_PARTIAL_TEXT))
    error(p, FS_UNSUPPORTED, 0,
          "source contains names without a supported text representation");
  if (p->status != FS_OK) {
    FsStatus st = p->status;
    destroy(p);
    return st;
  }
  if (p->has_regex && p->all_jit) {
    if (!fsx_reserve(e, 256 * 1024)) {
      destroy(p);
      return FS_NOMEM;
    }
    p->stack_charged = true;
    p->jit_stack = pcre2_jit_stack_create(32768, 256 * 1024, p->general);
    if (!p->jit_stack) {
      destroy(p);
      return FS_NOMEM;
    }
    pcre2_jit_stack_assign(p->match_context, NULL, p->jit_stack);
  }
  uint32_t cap = q->options.offset + q->options.limit, used = 0;
  uint32_t *ids = cap ? fsx_alloc(e, (size_t)cap * sizeof(*ids)) : NULL;
  if (cap && !ids) {
    destroy(p);
    return FS_NOMEM;
  }
  uint64_t total = 0, examined = 0;
  char *expression = NULL;
  FsStatus status = candidate_expression(p, &expression);
  FsCandidates candidates = {0};
  if (status == FS_OK)
    status = fsx_candidates_open(q, expression, &candidates);
  while (status == FS_OK) {
    uint32_t i;
    status = fsx_candidates_next(&candidates, &i);
    if (status != FS_OK)
      break;
    examined++;
    bool match = false;
    status = evaluate(p, p->root, fsx_snapshot_entry(q->snapshot, i), &match);
    if (status != FS_OK)
      break;
    if (!match)
      continue;
    total++;
    if (used < cap) {
      uint32_t at = used++;
      ids[at] = i;
      while (at && compare(q, ids[at], ids[(at - 1) / 2]) > 0) {
        uint32_t parent = (at - 1) / 2, swap = ids[at];
        ids[at] = ids[parent];
        ids[parent] = swap;
        at = parent;
      }
    } else if (cap && compare(q, i, ids[0]) < 0) {
      ids[0] = i;
      sift(q, ids, used, 0);
    }
  }
  fsx_candidates_close(&candidates);
  fsx_free(expression);
  if (status == FS_END)
    status = FS_OK;
  if (status != FS_OK) {
    fsx_free(ids);
    destroy(p);
    return status;
  }
  for (uint32_t n = used; n > 1; n--) {
    uint32_t swap = ids[0];
    ids[0] = ids[n - 1];
    ids[n - 1] = swap;
    sift(q, ids, n - 1, 0);
  }
  FsResult *result = fsx_alloc(e, sizeof(*result));
  if (!result) {
    fsx_free(ids);
    destroy(p);
    return FS_NOMEM;
  }
  memset(result, 0, sizeof(*result));
  atomic_init(&result->refs, 1);
  result->engine = e;
  result->snapshot = q->snapshot;
  atomic_fetch_add(&q->snapshot->refs, 1);
  uint32_t count = used > q->options.offset ? used - q->options.offset : 0;
  if (count && q->options.offset)
    memmove(ids, ids + q->options.offset, (size_t)count * sizeof(*ids));
  result->ids = ids;
  result->info = (FsResultInfo){q->snapshot->epoch,
                                total,
                                q->snapshot->unsupported,
                                q->options.offset,
                                count,
                                true,
                                q->snapshot->incomplete == 0,
                                q->snapshot->unsupported == 0,
                                p->has_regex && p->all_jit};
  result->info.needs_rescan = q->needs_rescan;
  result->info.candidate_strategy = candidates.strategy;
  result->info.candidates_examined = examined;
  snprintf(result->info.core_unicode_version,
           sizeof(result->info.core_unicode_version), "%s",
           salts_unicode_version());
  pcre2_config(PCRE2_CONFIG_VERSION, result->info.regex_version);
  pcre2_config(PCRE2_CONFIG_UNICODE_VERSION,
               result->info.regex_unicode_version);
  destroy(p);
  *out = result;
  return FS_OK;
}

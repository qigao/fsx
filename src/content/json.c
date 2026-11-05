#include "content.h"

static bool token(char **p, const char *end, const char *text) {
  size_t n = strlen(text);
  if ((size_t)(end - *p) < n || memcmp(*p, text, n))
    return false;
  *p += n;
  return true;
}
static bool number(char **p, const char *end, uint64_t *out) {
  char *start = *p;
  uint64_t n = 0;
  while (*p != end && **p >= '0' && **p <= '9') {
    unsigned d = (unsigned)(*(*p)++ - '0');
    if (n > (UINT64_MAX - d) / 10)
      return false;
    n = n * 10 + d;
  }
  *out = n;
  return *p != start;
}
static bool hex4(char **p, const char *end, uint32_t *v) {
  *v = 0;
  for (unsigned i = 0; i < 4; i++) {
    if (*p == end)
      return false;
    unsigned char c = (unsigned char)*(*p)++;
    unsigned d = c >= '0' && c <= '9'   ? c - '0'
                 : c >= 'a' && c <= 'f' ? c - 'a' + 10
                 : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                        : 16;
    if (d == 16)
      return false;
    *v = *v * 16 + d;
  }
  return true;
}
static bool string(char **p, char *end, FsContentRecordView *r) {
  if (!token(p, end, "\""))
    return false;
  char *out = *p, *start = out;
  while (*p != end && **p != '"') {
    unsigned char c = (unsigned char)*(*p)++;
    if (c < 32)
      return false;
    if (c == '\\') {
      if (*p == end)
        return false;
      c = (unsigned char)*(*p)++;
      const char *esc = "\"\\/bfnrt", *replacement = "\"\\/\b\f\n\r\t";
      const char *at = c ? strchr(esc, c) : NULL;
      if (at)
        c = (unsigned char)replacement[at - esc];
      else if (c == 'u') {
        uint32_t v, low;
        if (!hex4(p, end, &v))
          return false;
        if (v >= 0xd800 && v <= 0xdbff) {
          if (!token(p, end, "\\u") || !hex4(p, end, &low) || low < 0xdc00 ||
              low > 0xdfff)
            return false;
          v = 0x10000 + (v - 0xd800) * 1024 + low - 0xdc00;
        } else if (v >= 0xdc00 && v <= 0xdfff)
          return false;
        if (!v)
          return false;
        if (v < 0x80)
          *out++ = (char)v;
        else if (v < 0x800) {
          *out++ = (char)(0xc0 | (v >> 6));
          *out++ = (char)(0x80 | (v & 63));
        } else {
          if (v >= 0x10000)
            *out++ = (char)(0xf0 | (v >> 18));
          *out++ = (char)((v < 0x10000 ? 0xe0 : 0x80) |
                          ((v >> 12) & (v < 0x10000 ? 15 : 63)));
          *out++ = (char)(0x80 | ((v >> 6) & 63));
          *out++ = (char)(0x80 | (v & 63));
        }
        continue;
      } else
        return false;
    }
    *out++ = (char)c;
  }
  if (!token(p, end, "\""))
    return false;
  r->text = start;
  r->text_bytes = (size_t)(out - start);
  if (!fsx_utf8(start, r->text_bytes))
    return false;
  *out = 0;
  return true;
}
FsStatus fsx_content_decode(FsContentBatch *b, uint32_t limit) {
  char *p = b->wire, *end = p + b->used;
  while (p != end) {
    char *frame = memchr(p, '\n', (size_t)(end - p));
    if (!frame)
      return FS_CORRUPT;
    if (frame - p > 65536)
      return FS_LIMIT;
    if (b->info.record_count == limit)
      return FS_LIMIT;
    FsContentRecordView *r = &b->records[b->info.record_count];
    if (!token(&p, frame, "{\"line\":") || !number(&p, frame, &r->line) ||
        !r->line || !token(&p, frame, ",\"offset\":") ||
        !number(&p, frame, &r->byte_offset) ||
        !token(&p, frame, ",\"text\":") || !string(&p, frame, r) ||
        !token(&p, frame, "}") || p != frame)
      return FS_CORRUPT;
    b->info.record_count++;
    p = frame + 1;
  }
  return FS_OK;
}

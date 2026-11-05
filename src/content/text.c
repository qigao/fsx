#include "content.h"
bool fsx_content_text(FsTextState *s, const unsigned char *p, size_t n) {
  size_t i = 0;
  while (i < n) {
    if (!s->remaining && p[i] < 0x80) {
      /* Skip only complete, non-NUL ASCII words. memcpy permits unaligned
       * input without aliasing or over-read; byte-order does not affect the
       * high-bit/zero-byte predicates. Scalar transitions handle the boundary.
       */
      while (n - i >= sizeof(uint64_t)) {
        uint64_t word;
        memcpy(&word, p + i, sizeof(word));
        const uint64_t high = UINT64_C(0x8080808080808080);
        if ((word & high) ||
            ((word - UINT64_C(0x0101010101010101)) & ~word & high))
          break;
        i += sizeof(word);
      }
      if (i == n)
        break;
    }
    unsigned c = p[i++];
    if (!c)
      return false;
    if (s->remaining) {
      if ((c & 0xc0) != 0x80)
        return false;
      s->scalar = s->scalar * 64 + (c & 63);
      if (!--s->remaining && (s->scalar < s->minimum || s->scalar > 0x10ffff ||
                              (s->scalar >= 0xd800 && s->scalar <= 0xdfff)))
        return false;
    } else if (c >= 0x80) {
      if (c >= 0xc2 && c <= 0xdf) {
        s->remaining = 1;
        s->scalar = c & 31;
        s->minimum = 0x80;
      } else if (c >= 0xe0 && c <= 0xef) {
        s->remaining = 2;
        s->scalar = c & 15;
        s->minimum = 0x800;
      } else if (c >= 0xf0 && c <= 0xf4) {
        s->remaining = 3;
        s->scalar = c & 7;
        s->minimum = 0x10000;
      } else
        return false;
    }
  }
  return true;
}

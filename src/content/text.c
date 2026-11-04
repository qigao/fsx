#include "content.h"
bool fsx_content_text(FsTextState *s, const unsigned char *p, size_t n) {
  for (size_t i = 0; i < n; i++) {
    unsigned c = p[i];
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

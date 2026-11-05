#include "../src/content/content.h"
#include <stdlib.h>
#include <tinytest.h>

/* Core's independent scalar decoder defines full-buffer UTF-8 validity;
 * content additionally rejects NUL and carries partial scalars across calls. */
static bool streamed(const unsigned char *p, size_t n, size_t split) {
  FsTextState state = {0};
  return fsx_content_text(&state, p, split) &&
         fsx_content_text(&state, p + split, n - split) && !state.remaining;
}
static void compare(const unsigned char *p, size_t n, size_t split) {
  bool expected = fsx_utf8((const char *)p, n);
  check_equal(streamed(p, n, split), expected);
}
suite("Streaming UTF8 text validation") {
  it("validates bounded unaligned ASCII ranges without reading past the "
     "allocation") {
    for (size_t offset = 0; offset < 8; offset++)
      for (size_t n = 0; n <= 128; n++) {
        unsigned char *allocation = malloc(offset + n + 1);
        check_not_null(allocation);
        unsigned char *p = allocation + offset;
        for (size_t i = 0; i < n; i++)
          p[i] = (unsigned char)(1 + i % 127);
        /* The last byte is a canary, outside the supplied range. */
        p[n] = 0xff;
        for (size_t split = 0; split <= n; split++)
          compare(p, n, split);
        free(allocation);
      }
  }
  it("rejects NUL at every byte position and chunk split around word "
     "boundaries") {
    unsigned char bytes[80];
    for (size_t offset = 0; offset < 8; offset++)
      for (size_t n = 1; n <= 64; n++)
        for (size_t zero = 0; zero < n; zero++) {
          memset(bytes, 'a', sizeof(bytes));
          unsigned char *p = bytes + offset;
          p[zero] = 0;
          for (size_t split = 0; split <= n; split++)
            check_false(streamed(p, n, split));
        }
  }
  it("agrees with Core for every two-byte combination inside an ASCII run") {
    unsigned char bytes[24];
    memset(bytes, 'a', sizeof(bytes));
    for (unsigned pair = 0; pair <= 65535; pair++) {
      bytes[7] = (unsigned char)(pair >> 8);
      bytes[8] = (unsigned char)pair;
      compare(bytes, sizeof(bytes), 7);
      compare(bytes, sizeof(bytes), 8);
      compare(bytes, sizeof(bytes), 9);
      compare(bytes, sizeof(bytes), sizeof(bytes));
    }
  }
  it("preserves scalar limits and rejects malformed three and four byte "
     "encodings") {
    const unsigned char samples[][4] = {
        {0xe0, 0xa0, 0x80, 0x61}, {0xed, 0x9f, 0xbf, 0x61},
        {0xee, 0x80, 0x80, 0x61}, {0xef, 0xbf, 0xbf, 0x61},
        {0xf0, 0x90, 0x80, 0x80}, {0xf4, 0x8f, 0xbf, 0xbf},
        {0xe0, 0x9f, 0xbf, 0x61}, {0xed, 0xa0, 0x80, 0x61},
        {0xf0, 0x8f, 0xbf, 0xbf}, {0xf4, 0x90, 0x80, 0x80},
        {0xf5, 0x80, 0x80, 0x80}, {0xe2, 0x28, 0xa1, 0x61},
        {0xf0, 0x90, 0x28, 0x80}, {0xf0, 0x90, 0x80, 0},
    };
    for (size_t offset = 0; offset < 16; offset++)
      for (size_t sample = 0; sample < sizeof(samples) / sizeof(*samples);
           sample++) {
        unsigned char bytes[48];
        memset(bytes, 'a', sizeof(bytes));
        memcpy(bytes + offset, samples[sample], 4);
        for (size_t split = 0; split <= sizeof(bytes); split++)
          compare(bytes, sizeof(bytes), split);
      }
  }
  it("agrees with Core on dense multibyte runs and deterministic mutations") {
    unsigned char *bytes = malloc(65536);
    check_not_null(bytes);
    const unsigned char pattern[] = {0xe7, 0x8c, 0xab, 0xf0,
                                     0x9f, 0x98, 0x80, ' '};
    for (size_t i = 0; i < 65536; i += 8)
      memcpy(bytes + i, pattern, 8);
    for (size_t offset = 0; offset < 8; offset++)
      for (size_t split = 0; split < 64; split++)
        compare(bytes + offset, 65536 - offset, split);
    for (size_t at = 0; at < 65536; at += 257) {
      unsigned char saved = bytes[at];
      bytes[at] = 0;
      compare(bytes, 65536, at);
      compare(bytes, 65536, at + 1);
      bytes[at] = saved;
    }
    free(bytes);
  }
  it("carries every UTF8 prefix across the actual 64 KiB input boundary") {
    unsigned char *bytes = malloc(65540);
    check_not_null(bytes);
    const unsigned char text[] = {0xe7, 0x8c, 0xab, 0xf0, 0x9f, 0x98, 0x80};
    for (size_t cut = 1; cut <= 3; cut++) {
      memset(bytes, 'a', 65540);
      memcpy(bytes + 65536 - cut, text + 3, 4);
      compare(bytes, 65540, 65536);
      FsTextState state = {0};
      check_true(fsx_content_text(&state, bytes, 65536));
      check_equal(state.remaining, (unsigned)(4 - cut));
      check_true(fsx_content_text(&state, bytes + 65536, 4));
      check_equal(state.remaining, 0u);
    }
    for (size_t cut = 1; cut <= 2; cut++) {
      memset(bytes, 'a', 65540);
      memcpy(bytes + 65536 - cut, text, 3);
      compare(bytes, 65540, 65536);
    }
    memset(bytes, 'a', 65540);
    bytes[65536] = 0;
    check_false(streamed(bytes, 65540, 65536));
    free(bytes);
  }
}

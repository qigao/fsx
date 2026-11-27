#include "content.h"

static void *matcher_alloc(void *context, size_t bytes) {
  FsContentQuery *q = context;
  return fsx_alloc(q->engine, bytes);
}
static void matcher_free(void *context, void *pointer) {
  (void)context;
  fsx_free(pointer);
}
static int matcher_stop(void *context) {
  return (int)fsx_content_stop(context);
}
FsStatus fsx_content_native_compile(FsContentQuery *q) {
  FsMatcherHost host = {q, matcher_alloc, matcher_free, matcher_stop};
  char diagnostic[192] = {0};
  FsStatus status = (FsStatus)fsx_matcher_compile(
      &host, q->pattern, q->request.mode, &q->matcher, diagnostic,
      sizeof(diagnostic));
  if (diagnostic[0]) {
    cmeta_mutex_lock(&q->mutex);
    memcpy(q->info.diagnostic, diagnostic, sizeof(diagnostic));
    cmeta_mutex_unlock(&q->mutex);
  }
  return status;
}

static int emit(void *context, uint64_t line, uint64_t offset,
                const char *text, size_t bytes) {
  FsContentBatch *b = context;
  FsContentQuery *q = b->query;
  FsStatus status = fsx_content_stop(q);
  if (status != FS_OK)
    return (int)status;
  /* Native output owns UTF-8 text directly; no JSON serialization/decoding.
   * The text plus its NUL is charged once per record, including repeated
   * matches on the same line. Global record admission occurs at publication. */
  if (bytes > 65536 || b->info.record_count == q->request.max_records ||
      bytes >= q->request.max_output_bytes - b->used)
    return FS_LIMIT;
  char *owned = b->wire + b->used;
  memcpy(owned, text, bytes);
  owned[bytes] = 0;
  b->used += bytes + 1;
  FsContentRecordView *record = &b->records[b->info.record_count++];
  *record = (FsContentRecordView){line, offset, owned, bytes};
  return FS_OK;
}

static FsStatus read_block(FsContentQuery *q, FsContentBatch *b,
                           FsContentRead read, void *context,
                           unsigned char *buffer, size_t capacity,
                           size_t *bytes) {
  FsStatus status = fsx_content_stop(q);
  if (status != FS_OK)
    return status;
  uint64_t start = fsx_content_tick(b->profiled);
  *bytes = 0;
  status = read(context, buffer, capacity, bytes);
  fsx_content_tock(b->profiled, start, &b->timing.read_ns);
  if (b->profiled) {
    b->timing.read_calls++;
    b->timing.read_bytes += *bytes;
  }
  return status;
}

FsStatus fsx_content_native_file(FsContentQuery *q, FsContentBatch *b,
                                 uint64_t size_hint, FsContentRead read,
                                 void *context) {
  size_t limit = (size_t)q->request.max_file_bytes;
  if (size_hint > limit)
    return FS_LIMIT;
  size_t capacity = size_hint ? (size_t)size_hint : (limit < 4096 ? limit : 4096);
  unsigned char *input = fsx_alloc(q->engine, capacity + 1);
  if (!input)
    return FS_NOMEM;
  size_t used = 0;
  FsStatus status = FS_OK;
  for (;;) {
    size_t available = capacity - used;
    if (available > 65536)
      available = 65536;
    /* Read one extra byte at a capacity boundary to distinguish EOF from a
     * growing file without allocating the full configured file limit. */
    size_t bytes;
    status = read_block(q, b, read, context, input + used,
                         available ? available : 1, &bytes);
    if (status != FS_OK || !bytes)
      break;
    used += bytes;
    if (used > limit) {
      status = FS_LIMIT;
      break;
    }
    if (used > capacity) {
      size_t next = capacity > limit / 2 ? limit : capacity * 2;
      unsigned char *grown = fsx_alloc(q->engine, next + 1);
      if (!grown) {
        status = FS_NOMEM;
        break;
      }
      memcpy(grown, input, used);
      fsx_free(input);
      input = grown;
      capacity = next;
    }
  }
  FsTextState utf8 = {0};
  uint64_t start = fsx_content_tick(b->profiled);
  for (size_t offset = 0; status == FS_OK && offset < used;) {
    status = fsx_content_stop(q);
    if (status != FS_OK)
      break;
    size_t bytes = used - offset;
    if (bytes > 65536)
      bytes = 65536;
    if (!fsx_content_text(&utf8, input + offset, bytes))
      status = FS_UNSUPPORTED;
    offset += bytes;
  }
  if (status == FS_OK && utf8.remaining)
    status = FS_UNSUPPORTED;
  fsx_content_tock(b->profiled, start, &b->timing.text_ns);
  if (status == FS_OK) {
    size_t bom = used >= 3 && !memcmp(input, "\xef\xbb\xbf", 3) ? 3 : 0;
    input[used] = 0;
    status = (FsStatus)fsx_matcher_scan(q->matcher, (char *)input + bom,
                                        used - bom, emit, b, b->diagnostic,
                                        sizeof(b->diagnostic));
  }
  fsx_free(input);
  return status;
}

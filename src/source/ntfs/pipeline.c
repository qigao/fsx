#include "ntfs.h"

typedef struct Batch {
  unsigned char *data;
  size_t used;
} Batch;
typedef struct Pipeline {
  FsNtfs *source;
  FsNtfsRawStats *stats;
  cmeta_mutex_t mutex;
  cmeta_cond_t changed;
  Batch batches[16];
  unsigned char *storage;
  uint32_t head, tail, count, capacity, peak;
  uint64_t emitted;
  int64_t cursor, target;
  FsStatus status;
  bool inventory, rebuild, done;
} Pipeline;
static bool stopping(Pipeline *p) {
  return fsx_writer_stopping(p->source->engine);
}
/* The producer owns an unpublished tail. Published batches remain pinned in
 * the ring until their SQL callback has finished; borrowed native bytes are
 * copied before emit returns. The queue contains no SQLite connection. */
static FsStatus emit(void *context, const FsNtfsRecord *record) {
  Pipeline *p = context;
  size_t bytes = sizeof(*record) + record->name_bytes;
  if (!record->name || !record->name_bytes ||
      bytes > p->source->config.buffer_bytes)
    return FS_LIMIT;
  cmeta_mutex_lock(&p->mutex);
  for (;;) {
    if (p->status != FS_OK || stopping(p)) {
      FsStatus status = p->status != FS_OK ? p->status : FS_CANCELLED;
      cmeta_mutex_unlock(&p->mutex);
      return status;
    }
    if (p->count == p->capacity) {
      cmeta_cond_timedwait(&p->changed, &p->mutex, 10000000);
      continue;
    }
    Batch *batch = &p->batches[p->tail];
    if (bytes > p->source->config.buffer_bytes - batch->used) {
      p->count++;
      p->emitted++;
      if (p->count > p->peak)
        p->peak = p->count;
      p->tail = (p->tail + 1) % p->capacity;
      cmeta_cond_broadcast(&p->changed);
      continue;
    }
    memcpy(batch->data + batch->used, record, sizeof(*record));
    memcpy(batch->data + batch->used + sizeof(*record), record->name,
           record->name_bytes);
    batch->used += bytes;
    cmeta_mutex_unlock(&p->mutex);
    return FS_OK;
  }
}
typedef struct Inventory {
  Pipeline *pipeline;
  FsNtfsRecordFn output;
  void *context;
} Inventory;
static FsStatus enum_record(void *context, const FsNtfsRecord *record) {
  Inventory *i = context;
  if (stopping(i->pipeline))
    return FS_CANCELLED;
  if (++i->pipeline->stats->records >
      i->pipeline->source->config.max_mft_records)
    return FS_LIMIT;
  return record->attributes & FSX_NTFS_DIR ? i->output(i->context, record)
                                           : FS_OK;
}
static FsStatus collect(Pipeline *p, FsNtfsRecordFn output, void *context) {
  FsNtfs *n = p->source;
  if (p->inventory && n->config.mft_reader == FS_NTFS_MFT_RAW)
    return n->ops->inventory
               ? n->ops->inventory(n->provider, output, context, p->stats)
               : FS_UNSUPPORTED;
  if (p->inventory) {
    uint64_t from = 0;
    Inventory inventory = {p, output, context};
    for (;;) {
      if (stopping(p))
        return FS_CANCELLED;
      size_t bytes = 0;
      FsStatus status = n->ops->mft(n->provider, from, n->buffer,
                                    n->config.buffer_bytes, &bytes);
      if (status == FS_END)
        return FS_OK;
      if (status != FS_OK)
        return status;
      uint64_t next;
      uint32_t count;
      status = fsx_ntfs_records(n->buffer, bytes, false, from, 0, UINT32_MAX,
                                enum_record, &inventory, &next, &count);
      if (status != FS_OK)
        return status;
      if (next <= from)
        return FS_CORRUPT;
      from = next;
    }
  }
  while (p->cursor < p->target) {
    if (stopping(p))
      return FS_CANCELLED;
    size_t bytes = 0;
    FsStatus status =
        n->ops->read(n->provider, p->cursor, n->identity.journal_id, n->buffer,
                     n->config.buffer_bytes, &bytes);
    if (status != FS_OK)
      return status;
    uint64_t next;
    uint32_t count;
    status =
        fsx_ntfs_records(n->buffer, bytes, true, (uint64_t)p->cursor, p->target,
                         p->rebuild ? UINT32_MAX : n->config.max_batch_records,
                         output, context, &next, &count);
    if (status != FS_OK)
      return status;
    if (next <= (uint64_t)p->cursor)
      return FS_STALE;
    p->cursor = (int64_t)next;
    if (!p->rebuild)
      break;
  }
  return FS_OK;
}
static void produce(void *context) {
  Pipeline *p = context;
  FsStatus status = collect(p, emit, p);
  cmeta_mutex_lock(&p->mutex);
  if (p->status == FS_OK)
    p->status = status;
  if (status == FS_OK && p->batches[p->tail].used && p->count < p->capacity) {
    p->count++;
    p->emitted++;
    if (p->count > p->peak)
      p->peak = p->count;
  }
  p->done = true;
  cmeta_cond_broadcast(&p->changed);
  cmeta_mutex_unlock(&p->mutex);
}
FsStatus fsx_ntfs_collect(FsNtfs *n, bool inventory, int64_t *cursor,
                          int64_t target, bool rebuild, FsNtfsRecordFn visit,
                          void *context, FsNtfsRawStats *stats,
                          uint64_t *batches, uint32_t *peak) {
  Pipeline p = {0};
  p.source = n;
  p.stats = stats;
  p.cursor = cursor ? *cursor : 0;
  p.target = target;
  p.inventory = inventory;
  p.rebuild = rebuild;
  p.capacity = n->config.pipeline_batches;
  if (!p.capacity) {
    FsStatus status = collect(&p, visit, context);
    if (status == FS_OK && cursor)
      *cursor = p.cursor;
    return status;
  }
  cmeta_mutex_init(&p.mutex);
  cmeta_cond_init(&p.changed);
  p.storage = fsx_alloc(n->engine, (size_t)p.capacity * n->config.buffer_bytes);
  FsStatus status = FS_NOMEM;
  if (!p.mutex || !p.changed || !p.storage)
    goto cleanup;
  for (uint32_t i = 0; i < p.capacity; i++)
    p.batches[i].data = p.storage + (size_t)i * n->config.buffer_bytes;
  cmeta_thread_t thread = NULL;
  if (cmeta_thread_create(&thread, produce, &p) != 0)
    goto cleanup;
  for (;;) {
    cmeta_mutex_lock(&p.mutex);
    while (!p.count && !p.done && p.status == FS_OK && !stopping(&p))
      cmeta_cond_timedwait(&p.changed, &p.mutex, 10000000);
    status = p.status;
    if (status == FS_OK && stopping(&p))
      status = FS_CANCELLED;
    if (status != FS_OK || !p.count) {
      if (status != FS_OK && p.status == FS_OK)
        p.status = status;
      cmeta_cond_broadcast(&p.changed);
      cmeta_mutex_unlock(&p.mutex);
      break;
    }
    Batch *batch = &p.batches[p.head];
    cmeta_mutex_unlock(&p.mutex);
    for (size_t at = 0; at < batch->used;) {
      FsNtfsRecord record;
      memcpy(&record, batch->data + at, sizeof(record));
      record.name = batch->data + at + sizeof(record);
      status = stopping(&p) ? FS_CANCELLED : visit(context, &record);
      if (status != FS_OK)
        break;
      at += sizeof(record) + record.name_bytes;
    }
    cmeta_mutex_lock(&p.mutex);
    batch->used = 0;
    p.head = (p.head + 1) % p.capacity;
    p.count--;
    if (status != FS_OK) {
      if (p.status == FS_OK)
        p.status = status;
      else
        status = p.status;
      /* Wake native parsing/overlapped waits, retaining the original SQL or
       * callback failure rather than the producer's cancellation status. */
      atomic_store(&n->engine->writer_cancelled, true);
    }
    cmeta_cond_broadcast(&p.changed);
    cmeta_mutex_unlock(&p.mutex);
    if (status != FS_OK)
      break;
  }
  cmeta_thread_join(&thread);
  if (status == FS_OK && cursor)
    *cursor = p.cursor;
  *batches += p.emitted;
  if (p.peak > *peak)
    *peak = p.peak;
cleanup:
  fsx_free(p.storage);
  if (p.changed)
    cmeta_cond_destroy(&p.changed);
  if (p.mutex)
    cmeta_mutex_destroy(&p.mutex);
  return status;
}

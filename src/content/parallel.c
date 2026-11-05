#include "content.h"

static void abort_job(FsContentQuery *q, FsStatus status) {
  int expected = FS_OK;
  atomic_compare_exchange_strong(&q->failure, &expected, (int)status);
  /* Also interrupt a metadata-scope evaluator through its existing parent
   * cancellation flag. The failure status remains the terminal cause. */
  atomic_store(&q->cancelled, true);
  cmeta_mutex_lock(&q->mutex);
  cmeta_cond_broadcast(&q->changed);
  cmeta_mutex_unlock(&q->mutex);
}
FsStatus fsx_content_enqueue(FsContentQuery *q, const FsEntry *entry) {
  FsStatus status = fsx_content_count_candidate(q, entry);
  if (status != FS_OK)
    return status == FS_UNSUPPORTED ? FS_OK : status;
  size_t bytes = q->root ? strlen(entry->path) + 1 : 0;
  cmeta_mutex_lock(&q->mutex);
  while ((q->queue_count == FSX_CONTENT_QUEUE ||
          bytes > FSX_CONTENT_PATH_BYTES - q->queue_bytes) &&
         fsx_content_stop(q) == FS_OK)
    cmeta_cond_timedwait(&q->changed, &q->mutex, 25000000);
  status = fsx_content_stop(q);
  if (status == FS_OK) {
    char *owned = q->root ? fsx_copy(q->engine, entry->path, bytes - 1) : NULL;
    if (q->root && !owned)
      status = FS_NOMEM;
    else {
      uint32_t tail = (q->queue_head + q->queue_count) % FSX_CONTENT_QUEUE;
      q->queue[tail] = (FsContentCandidate){*entry, owned};
      if (owned)
        q->queue[tail].entry.path = owned;
      q->queue_count++;
      q->queue_bytes += (uint32_t)bytes;
      q->info.queued_candidates = q->queue_count;
      q->info.queued_path_bytes = q->queue_bytes;
      cmeta_cond_broadcast(&q->changed);
    }
  }
  cmeta_mutex_unlock(&q->mutex);
  return status;
}
static void consume(void *context) {
  FsContentQuery *q = context;
  for (;;) {
    cmeta_mutex_lock(&q->mutex);
    while (!q->queue_count && !q->producer_done && fsx_content_stop(q) == FS_OK)
      cmeta_cond_timedwait(&q->changed, &q->mutex, 25000000);
    if (!q->queue_count || fsx_content_stop(q) != FS_OK) {
      cmeta_mutex_unlock(&q->mutex);
      break;
    }
    FsContentCandidate candidate = q->queue[q->queue_head];
    q->queue_head = (q->queue_head + 1) % FSX_CONTENT_QUEUE;
    q->queue_count--;
    if (candidate.owned_path)
      q->queue_bytes -= (uint32_t)(strlen(candidate.owned_path) + 1);
    q->info.queued_candidates = q->queue_count;
    q->info.queued_path_bytes = q->queue_bytes;
    cmeta_cond_broadcast(&q->changed);
    cmeta_mutex_unlock(&q->mutex);
    FsStatus status = fsx_content_process(q, &candidate.entry, false);
    fsx_free(candidate.owned_path);
    if (status != FS_OK) {
      abort_job(q, status);
      break;
    }
  }
}
FsStatus fsx_content_parallel(FsContentQuery *q,
                              FsStatus (*produce)(FsContentQuery *)) {
  cmeta_thread_t workers[2] = {0};
  unsigned started = 0;
  FsStatus status = fsx_content_stop(q);
  while (status == FS_OK && started < 2) {
    if (cmeta_thread_create(&workers[started], consume, q))
      status = FS_NOMEM;
    else
      started++;
  }
  if (status == FS_OK)
    status = produce(q);
  if (status != FS_OK)
    abort_job(q, status);
  cmeta_mutex_lock(&q->mutex);
  q->producer_done = true;
  cmeta_cond_broadcast(&q->changed);
  cmeta_mutex_unlock(&q->mutex);
  for (unsigned i = 0; i < started; i++) {
    cmeta_thread_join(&workers[i]);
    cmeta_thread_destroy(&workers[i]);
  }
  status = fsx_content_stop(q);
  while (q->queue_count) {
    fsx_free(q->queue[q->queue_head].owned_path);
    q->queue_head = (q->queue_head + 1) % FSX_CONTENT_QUEUE;
    q->queue_count--;
  }
  cmeta_mutex_lock(&q->mutex);
  q->queue_bytes = 0;
  q->info.queued_candidates = q->info.queued_path_bytes = 0;
  cmeta_mutex_unlock(&q->mutex);
  return status;
}

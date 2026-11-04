#include "content.h"

FsContentRequest fsx_content_request(const FsResult *candidates,
                                     const char *tool, const char *pattern) {
  FsContentRequest r = {sizeof(r), FSX_API_VERSION, candidates,
                        tool,      pattern,         FS_CONTENT_LITERAL,
                        10000,     8 * 1024 * 1024, 32 * 1024 * 1024,
                        UINT64_MAX};
  return r;
}
FsStatus fsx_content_stop(FsContentQuery *q) {
  if (atomic_load(&q->cancelled) || atomic_load(&q->engine->stopping))
    return FS_CANCELLED;
  return q->request.deadline_ns != UINT64_MAX &&
                 cmeta_hrtime() >= q->request.deadline_ns
             ? FS_TIMEOUT
             : FS_OK;
}
static void unref(FsContentQuery *q) {
  if (atomic_fetch_sub(&q->refs, 1) != 1)
    return;
  fsx_result_unref(q->candidates);
  fsx_free(q->tool);
  fsx_free(q->pattern);
  cmeta_cond_destroy(&q->changed);
  cmeta_mutex_destroy(&q->mutex);
  fsx_free(q);
}
static void free_batch(FsContentBatch *b) {
  FsContentQuery *q = b->query;
  fsx_free(b->wire);
  fsx_free(b->records);
  fsx_free(b);
  cmeta_mutex_lock(&q->mutex);
  q->outstanding = false;
  cmeta_cond_broadcast(&q->changed);
  cmeta_mutex_unlock(&q->mutex);
  unref(q);
}
static FsContentBatch *new_batch(FsContentQuery *q, const FsEntry *entry) {
  FsEngine *e = q->engine;
  FsContentBatch *b = fsx_alloc(e, sizeof(*b));
  if (!b)
    return NULL;
  memset(b, 0, sizeof(*b));
  b->query = q;
  b->wire = fsx_alloc(e, q->request.max_output_bytes + 1);
  b->records =
      fsx_alloc(e, (size_t)q->request.max_records * sizeof(*b->records));
  if (!b->wire || !b->records) {
    fsx_free(b->wire);
    fsx_free(b->records);
    fsx_free(b);
    return NULL;
  }
  atomic_fetch_add(&q->refs, 1);
  b->info.key = entry->key;
  b->info.entry_key = entry->entry_key;
  b->info.path = entry->path;
  b->info.metadata_epoch = q->info.metadata_epoch;
  return b;
}
static void run(void *context) {
  FsContentQuery *q = context;
  FsStatus status = fsx_content_stop(q);
  for (uint32_t i = 0; status == FS_OK && i < q->candidates->info.window_count;
       i++) {
    cmeta_mutex_lock(&q->mutex);
    while (q->outstanding && (status = fsx_content_stop(q)) == FS_OK)
      cmeta_cond_timedwait(&q->changed, &q->mutex, 25000000);
    status = fsx_content_stop(q);
    cmeta_mutex_unlock(&q->mutex);
    if (status != FS_OK)
      break;
    const FsEntry *entry =
        fsx_snapshot_entry(q->candidates->snapshot, q->candidates->ids[i]);
    FsContentBatch *b = new_batch(q, entry);
    if (!b) {
      status = FS_NOMEM;
      break;
    }
    status = fsx_content_file(q, entry, b);
    cmeta_mutex_lock(&q->mutex);
    if (status == FS_STALE)
      q->info.changed_files++;
    else if (status == FS_DENIED || status == FS_UNSUPPORTED)
      q->info.skipped_files++;
    else if (status == FS_OK) {
      status = fsx_content_decode(b, q->request.max_records -
                                         (uint32_t)q->info.match_records);
    }
    FsStatus stopped = fsx_content_stop(q);
    if (stopped != FS_OK && (status == FS_OK || status == FS_STALE ||
                             status == FS_DENIED || status == FS_UNSUPPORTED))
      status = stopped;
    if (status == FS_OK) {
      q->info.files_scanned++;
      q->info.match_records += b->info.record_count;
    }
    bool publish = status == FS_OK && b->info.record_count != 0;
    if (publish) {
      q->outstanding = true;
      q->pending = b;
      cmeta_cond_broadcast(&q->changed);
    }
    cmeta_mutex_unlock(&q->mutex);
    if (!publish)
      free_batch(b);
    if (status == FS_STALE || status == FS_DENIED || status == FS_UNSUPPORTED)
      status = FS_OK;
    if (status != FS_OK)
      break;
  }
  cmeta_mutex_lock(&q->mutex);
  q->info.status = status;
  cmeta_mutex_unlock(&q->mutex);
}
static void finalize(void *context) {
  FsContentQuery *q = context;
  FsEngine *e = q->engine;
  cmeta_mutex_lock(&e->mutex);
  e->content = NULL;
  cmeta_mutex_unlock(&e->mutex);
  cmeta_mutex_lock(&q->mutex);
  q->info.finished = true;
  q->info.complete = q->info.status == FS_OK && !q->info.changed_files &&
                     !q->info.skipped_files &&
                     q->candidates->info.coverage_complete &&
                     !q->candidates->info.needs_rescan;
  cmeta_cond_broadcast(&q->changed);
  cmeta_mutex_unlock(&q->mutex);
  unref(q);
  cmeta_mutex_lock(&e->mutex);
  e->active_jobs--;
  cmeta_cond_broadcast(&e->changed);
  cmeta_mutex_unlock(&e->mutex);
}
FsStatus fsx_content_submit(FsEngine *e, const FsContentRequest *r,
                            FsContentQuery **out) {
  if (!out)
    return FS_INVALID;
  *out = NULL;
#ifndef __linux__
  (void)e;
  (void)r;
  return FS_UNSUPPORTED;
#else
  if (!e || !r || r->struct_size != sizeof(*r) ||
      r->api_version != FSX_API_VERSION || !r->candidates ||
      r->candidates->engine != e || !r->ugrep_executable ||
      r->ugrep_executable[0] != '/' || strlen(r->ugrep_executable) > 65535 ||
      !r->pattern || strlen(r->pattern) > 4096 || strchr(r->pattern, '\n') ||
      !fsx_utf8(r->pattern, strlen(r->pattern)) || r->mode > FS_CONTENT_PCRE2 ||
      !r->max_records || r->max_records > 10000 || r->max_output_bytes < 1024 ||
      r->max_output_bytes > 8 * 1024 * 1024 || !r->max_file_bytes ||
      r->max_file_bytes > 32 * 1024 * 1024)
    return FS_INVALID;
  FsContentQuery *q = fsx_alloc(e, sizeof(*q));
  if (!q)
    return FS_NOMEM;
  memset(q, 0, sizeof(*q));
  atomic_init(&q->refs, 2);
  atomic_init(&q->cancelled, false);
  q->engine = e;
  q->request = *r;
  q->candidates = (FsResult *)r->candidates;
  atomic_fetch_add(&q->candidates->refs, 1);
  q->tool = fsx_copy(e, r->ugrep_executable, strlen(r->ugrep_executable));
  q->pattern = fsx_copy(e, r->pattern, strlen(r->pattern));
  q->request.ugrep_executable = q->tool;
  q->request.pattern = q->pattern;
  cmeta_mutex_init(&q->mutex);
  cmeta_cond_init(&q->changed);
  if (!q->tool || !q->pattern || !q->mutex || !q->changed) {
    fsx_result_unref(q->candidates);
    fsx_free(q->tool);
    fsx_free(q->pattern);
    if (q->mutex)
      cmeta_mutex_destroy(&q->mutex);
    if (q->changed)
      cmeta_cond_destroy(&q->changed);
    fsx_free(q);
    return FS_NOMEM;
  }
  q->info.metadata_epoch = q->candidates->info.snapshot_epoch;
  q->info.candidate_count = q->candidates->info.window_count;
  q->info.status = FS_WAIT;
  cmeta_mutex_lock(&e->mutex);
  FsStatus status = FS_BUSY;
  if (!atomic_load(&e->stopping) && !e->content &&
      e->handles < e->config.max_handles) {
    if (!e->content_pool) {
      cmeta_threadpool_config_t config = {1, 1};
      e->content_pool = cmeta_threadpool_create_with_config(&config);
    }
    if (!e->content_pool)
      status = FS_NOMEM;
    else {
      e->content = q;
      e->handles++;
      e->active_jobs++;
      cmeta_threadpool_task_t task = {run, NULL, finalize, q};
      if (cmeta_threadpool_try_submit_task(e->content_pool, &task) == 0)
        status = FS_OK;
      else {
        e->content = NULL;
        e->handles--;
        e->active_jobs--;
      }
    }
  }
  cmeta_mutex_unlock(&e->mutex);
  if (status == FS_OK)
    *out = q;
  else {
    atomic_store(&q->refs, 1);
    unref(q);
  }
  return status;
#endif
}
FsStatus fsx_content_poll(FsContentQuery *q, FsContentInfo *out) {
  if (!q || !out)
    return FS_INVALID;
  cmeta_mutex_lock(&q->mutex);
  *out = q->info;
  FsStatus status = out->finished ? out->status : FS_WAIT;
  cmeta_mutex_unlock(&q->mutex);
  return status;
}
FsStatus fsx_content_wait(FsContentQuery *q, uint64_t deadline) {
  if (!q)
    return FS_INVALID;
  cmeta_mutex_lock(&q->mutex);
  while (!q->pending && !q->info.finished) {
    if (!deadline) {
      cmeta_mutex_unlock(&q->mutex);
      return FS_WAIT;
    }
    uint64_t now = cmeta_hrtime();
    if (deadline != UINT64_MAX && now >= deadline) {
      cmeta_mutex_unlock(&q->mutex);
      return FS_TIMEOUT;
    }
    if (deadline == UINT64_MAX)
      cmeta_cond_wait(&q->changed, &q->mutex);
    else
      cmeta_cond_timedwait(&q->changed, &q->mutex, deadline - now);
  }
  FsStatus status = q->pending ? FS_OK : q->info.status;
  cmeta_mutex_unlock(&q->mutex);
  return status;
}
FsStatus fsx_content_batch_acquire(FsContentQuery *q, FsContentBatch **out) {
  if (!q || !out)
    return FS_INVALID;
  *out = NULL;
  FsEngine *e = q->engine;
  cmeta_mutex_lock(&e->mutex);
  cmeta_mutex_lock(&q->mutex);
  FsStatus status = q->info.finished ? FS_END : FS_WAIT;
  if (q->pending) {
    if (e->handles >= e->config.max_handles)
      status = FS_BUSY;
    else {
      *out = q->pending;
      q->pending = NULL;
      (*out)->acquired = true;
      e->handles++;
      status = FS_OK;
    }
  }
  cmeta_mutex_unlock(&q->mutex);
  cmeta_mutex_unlock(&e->mutex);
  return status;
}
FsStatus fsx_content_batch_info(const FsContentBatch *b,
                                FsContentFileView *out) {
  if (!b || !out)
    return FS_INVALID;
  *out = b->info;
  return FS_OK;
}
FsStatus fsx_content_batch_record(const FsContentBatch *b, uint32_t id,
                                  FsContentRecordView *out) {
  if (!b || !out || id >= b->info.record_count)
    return FS_INVALID;
  *out = b->records[id];
  return FS_OK;
}
void fsx_content_batch_release(FsContentBatch *b) {
  if (!b)
    return;
  FsEngine *e = b->query->engine;
  free_batch(b);
  cmeta_mutex_lock(&e->mutex);
  e->handles--;
  cmeta_cond_broadcast(&e->changed);
  cmeta_mutex_unlock(&e->mutex);
}
void fsx_content_cancel(FsContentQuery *q) {
  if (!q)
    return;
  atomic_store(&q->cancelled, true);
  cmeta_mutex_lock(&q->mutex);
  cmeta_cond_broadcast(&q->changed);
  cmeta_mutex_unlock(&q->mutex);
}
void fsx_content_cancel_all(FsEngine *e) {
  if (e->content)
    fsx_content_cancel(e->content);
}
void fsx_content_release(FsContentQuery *q) {
  if (!q)
    return;
  FsEngine *e = q->engine;
  fsx_content_cancel(q);
  cmeta_mutex_lock(&q->mutex);
  FsContentBatch *pending = q->pending;
  q->pending = NULL;
  cmeta_mutex_unlock(&q->mutex);
  if (pending)
    free_batch(pending);
  unref(q);
  cmeta_mutex_lock(&e->mutex);
  e->handles--;
  cmeta_cond_broadcast(&e->changed);
  cmeta_mutex_unlock(&e->mutex);
}

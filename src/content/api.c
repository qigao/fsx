#include "content.h"
#ifdef FSX_POSIX_CONTENT
#include "child_posix.h"
#include <unistd.h>
#endif

bool fsx_content_tool_path(const char *path) {
#ifdef _WIN32
  return path && ((strlen(path) >= 3 &&
                   ((path[0] >= 'A' && path[0] <= 'Z') ||
                    (path[0] >= 'a' && path[0] <= 'z')) &&
                   path[1] == ':' && (path[2] == '/' || path[2] == '\\')) ||
                  (strlen(path) >= 3 && (path[0] == '/' || path[0] == '\\') &&
                   (path[1] == '/' || path[1] == '\\')));
#else
  return path && path[0] == '/';
#endif
}
static bool fsx_content_root_path(const char *path) {
#ifdef FSX_POSIX_CONTENT
  return fsx_content_path(path);
#else
  (void)path;
  return false;
#endif
}
FsContentRequest fsx_content_request(const FsResult *candidates,
                                     const char *tool, const char *pattern) {
  FsContentRequest r = {sizeof(r),
                        FSX_API_VERSION,
                        candidates,
                        tool,
                        pattern,
                        FS_CONTENT_LITERAL,
                        10000,
                        8 * 1024 * 1024,
                        32 * 1024 * 1024,
                        UINT64_MAX,
                        NULL,
                        0,
                        0,
                        NULL,
                        NULL,
                        1000000,
                        64 * 1024 * 1024,
                        1};
  return r;
}
FsContentRequest fsx_content_scope_request(const char *scope, const char *tool,
                                           const char *pattern) {
  FsContentRequest r = fsx_content_request(NULL, tool, pattern);
  r.scope = scope;
  return r;
}
FsContentRequest fsx_content_live_request(const char *root, const char *bfs,
                                          const char *tool,
                                          const char *pattern) {
  FsContentRequest r = fsx_content_request(NULL, tool, pattern);
  r.root = root;
  r.bfs_executable = bfs;
  return r;
}
FsStatus fsx_content_stop(FsContentQuery *q) {
  FsStatus failure = (FsStatus)atomic_load(&q->failure);
  if (failure != FS_OK)
    return failure;
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
  fsx_snapshot_release(q->snapshot);
  fsx_free(q->scope_query.text);
  fsx_free(q->tool);
  fsx_free(q->pattern);
  fsx_free(q->root);
  fsx_free(q->bfs);
  fsx_free(q->queue);
  cmeta_cond_destroy(&q->changed);
  cmeta_mutex_destroy(&q->mutex);
  fsx_free(q);
}
static void free_batch(FsContentBatch *b) {
  FsContentQuery *q = b->query;
  fsx_free(b->wire);
  fsx_free(b->records);
  fsx_free(b->owned_path);
  fsx_free(b);
  cmeta_mutex_lock(&q->mutex);
  q->in_flight--;
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
  b->profiled = q->metrics != NULL;
  b->wire = fsx_alloc(e, q->request.max_output_bytes + 1);
  b->records =
      fsx_alloc(e, (size_t)q->request.max_records * sizeof(*b->records));
  if (q->root)
    b->owned_path = fsx_copy(e, entry->path, strlen(entry->path));
  if (!b->wire || !b->records || (q->root && !b->owned_path)) {
    fsx_free(b->wire);
    fsx_free(b->records);
    fsx_free(b->owned_path);
    fsx_free(b);
    return NULL;
  }
  atomic_fetch_add(&q->refs, 1);
  b->info.key = entry->key;
  b->info.entry_key = entry->entry_key;
  b->info.path = b->owned_path ? b->owned_path : entry->path;
  b->info.metadata_epoch = q->info.metadata_epoch;
  return b;
}
FsStatus fsx_content_count_candidate(FsContentQuery *q, const FsEntry *entry) {
  FsStatus status = FS_OK;
  if (q->request.scope || q->root) {
    cmeta_mutex_lock(&q->mutex);
    if (q->root && q->info.candidate_count == q->request.max_candidates)
      status = FS_LIMIT;
    else {
      q->info.candidate_count++;
      if (q->root && !fsx_utf8(entry->path, strlen(entry->path))) {
        q->info.skipped_files++;
        status = FS_UNSUPPORTED;
      }
    }
    cmeta_mutex_unlock(&q->mutex);
  }
  return status;
}
static void release_slot(FsContentQuery *q) {
  cmeta_mutex_lock(&q->mutex);
  q->in_flight--;
  cmeta_cond_broadcast(&q->changed);
  cmeta_mutex_unlock(&q->mutex);
}
FsStatus fsx_content_process(FsContentQuery *q, const FsEntry *entry,
                             bool count) {
  cmeta_mutex_lock(&q->mutex);
  while (q->in_flight == q->request.parallel_files &&
         fsx_content_stop(q) == FS_OK)
    cmeta_cond_timedwait(&q->changed, &q->mutex, 25000000);
  FsStatus status = fsx_content_stop(q);
  if (status == FS_OK)
    q->in_flight++;
  cmeta_mutex_unlock(&q->mutex);
  if (status != FS_OK)
    return status;
  if (count) {
    status = fsx_content_count_candidate(q, entry);
    if (status != FS_OK) {
      release_slot(q);
      return status == FS_UNSUPPORTED ? FS_OK : status;
    }
  }
  FsContentBatch *b = new_batch(q, entry);
  if (!b) {
    release_slot(q);
    return FS_NOMEM;
  }
  status = fsx_content_file(q, entry, b);
  cmeta_mutex_lock(&q->mutex);
  if (b->diagnostic[0] && !q->info.diagnostic[0])
    memcpy(q->info.diagnostic, b->diagnostic, sizeof(b->diagnostic));
  /* Only publication/decode is serialized. A second worker may own a fully
   * checked file while the consumer retains the other bounded batch slot. */
  while (status == FS_OK && b->used && q->pending &&
         fsx_content_stop(q) == FS_OK)
    cmeta_cond_timedwait(&q->changed, &q->mutex, 25000000);
  FsStatus stopped = fsx_content_stop(q);
  if (stopped != FS_OK && (status == FS_OK || status == FS_STALE ||
                           status == FS_DENIED || status == FS_UNSUPPORTED))
    status = stopped;
  if (status == FS_STALE)
    q->info.changed_files++;
  else if (status == FS_DENIED || status == FS_UNSUPPORTED)
    q->info.skipped_files++;
  else if (status == FS_OK) {
    uint64_t decode_start = fsx_content_tick(b->profiled);
    status = fsx_content_decode(b, q->request.max_records -
                                       (uint32_t)q->info.match_records);
    fsx_content_tock(b->profiled, decode_start, &b->timing.decode_ns);
  }
  if (q->metrics) {
#define ADD(name) q->metrics->name += b->timing.name;
    FSX_CONTENT_METRICS(ADD)
#undef ADD
  }
  if (status == FS_OK) {
    q->info.files_scanned++;
    q->info.match_records += b->info.record_count;
  }
  bool publish = status == FS_OK && b->info.record_count != 0;
  if (publish) {
    q->pending = b;
    cmeta_cond_broadcast(&q->changed);
  }
  cmeta_mutex_unlock(&q->mutex);
  if (!publish)
    free_batch(b);
  if (status == FS_STALE || status == FS_DENIED || status == FS_UNSUPPORTED)
    status = FS_OK;
  return status;
}
FsStatus fsx_content_visit(void *context, const FsEntry *entry) {
  FsContentQuery *q = context;
  return q->request.parallel_files == 2 ? fsx_content_enqueue(q, entry)
                                        : fsx_content_process(q, entry, true);
}
static FsStatus produce(FsContentQuery *q) {
  FsStatus status = fsx_content_stop(q);
  if (status != FS_OK)
    return status;
  if (q->root) {
#ifdef FSX_POSIX_CONTENT
    status = fsx_content_discover(q);
#endif
  } else if (q->request.scope)
    status = fsx_query_visit(&q->scope_query, fsx_content_visit, q);
  else
    for (uint32_t i = 0;
         status == FS_OK && i < q->candidates->info.window_count; i++)
      status = fsx_content_visit(
          q, fsx_snapshot_entry(q->snapshot, q->candidates->ids[i]));
  return status;
}
static void run(void *context) {
  FsContentQuery *q = context;
  FsStatus status = q->request.parallel_files == 2
                        ? fsx_content_parallel(q, produce)
                        : produce(q);
#ifdef FSX_POSIX_CONTENT
  if (q->root_fd >= 0) {
    if (status == FS_OK)
      status = fsx_content_check_root(q);
    close(q->root_fd);
    q->root_fd = -1;
  }
#endif
  cmeta_mutex_lock(&q->mutex);
  if ((q->request.scope || q->root) && status == FS_OK)
    q->info.candidate_count_known = true;
  if (q->scope_query.info.diagnostic[0])
    memcpy(q->info.diagnostic, q->scope_query.info.diagnostic,
           sizeof(q->info.diagnostic));
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
                     !q->info.skipped_files && q->coverage_complete &&
                     !q->needs_rescan;
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
#if !defined(FSX_POSIX_CONTENT) && !defined(_WIN32)
  (void)e;
  (void)r;
  return FS_UNSUPPORTED;
#else
#ifdef _WIN32
  if (r && r->struct_size == sizeof(*r) && r->root)
    return FS_UNSUPPORTED;
#endif
  if (!e || !r || r->struct_size != sizeof(*r) ||
      r->api_version != FSX_API_VERSION ||
      (!!r->candidates + !!r->scope + !!r->root != 1) ||
      (!r->root && r->bfs_executable) ||
      (r->root && (!r->bfs_executable || r->bfs_executable[0] != '/' ||
                   strlen(r->bfs_executable) > 65535 ||
                   strlen(r->root) > 65535 || !fsx_content_root_path(r->root) ||
                   !fsx_utf8(r->root, strlen(r->root)) || r->scope_flags ||
                   r->timezone_offset_minutes || !r->max_candidates ||
                   r->max_candidates > 1000000 || !r->max_discovery_bytes ||
                   r->max_discovery_bytes > 256 * 1024 * 1024)) ||
      (r->candidates && (r->candidates->engine != e || r->scope_flags ||
                         r->timezone_offset_minutes)) ||
      (r->scope &&
       (strlen(r->scope) > 65536 || !fsx_utf8(r->scope, strlen(r->scope)))) ||
      r->scope_flags &
          ~(FS_QUERY_CASE_SENSITIVE | FS_QUERY_ALLOW_PARTIAL_TEXT) ||
      r->timezone_offset_minutes < -840 || r->timezone_offset_minutes > 840 ||
      !r->ugrep_executable || !fsx_content_tool_path(r->ugrep_executable) ||
      strlen(r->ugrep_executable) > 65535 || !r->pattern ||
      strlen(r->pattern) > 4096 || strchr(r->pattern, '\n') ||
      !fsx_utf8(r->pattern, strlen(r->pattern)) || r->mode > FS_CONTENT_PCRE2 ||
      !r->max_records || r->max_records > 10000 || r->max_output_bytes < 1024 ||
      r->max_output_bytes > 8 * 1024 * 1024 || !r->max_file_bytes ||
      r->max_file_bytes > 32 * 1024 * 1024 || !r->parallel_files ||
      r->parallel_files > 2)
    return FS_INVALID;
  FsContentQuery *q = fsx_alloc(e, sizeof(*q));
  if (!q)
    return FS_NOMEM;
  memset(q, 0, sizeof(*q));
  atomic_init(&q->refs, 2);
  atomic_init(&q->cancelled, false);
  atomic_init(&q->failure, FS_OK);
  q->engine = e;
  q->root_fd = -1;
  q->request = *r;
  q->candidates = (FsResult *)r->candidates;
  if (q->candidates)
    atomic_fetch_add(&q->candidates->refs, 1);
  if (r->scope)
    q->scope_query.text = fsx_copy(e, r->scope, strlen(r->scope));
  q->tool = fsx_copy(e, r->ugrep_executable, strlen(r->ugrep_executable));
  q->pattern = fsx_copy(e, r->pattern, strlen(r->pattern));
  q->request.ugrep_executable = q->tool;
  q->request.pattern = q->pattern;
  q->request.scope = q->scope_query.text;
  if (r->root) {
    q->root = fsx_copy(e, r->root, strlen(r->root));
    q->bfs = fsx_copy(e, r->bfs_executable, strlen(r->bfs_executable));
  }
  q->request.root = q->root;
  q->request.bfs_executable = q->bfs;
  cmeta_mutex_init(&q->mutex);
  cmeta_cond_init(&q->changed);
  if (r->parallel_files == 2)
    q->queue = fsx_alloc(e, FSX_CONTENT_QUEUE * sizeof(*q->queue));
  if (!q->tool || !q->pattern || !q->mutex || !q->changed ||
      (r->parallel_files == 2 && !q->queue) ||
      (r->scope && !q->scope_query.text) ||
      (r->root && (!q->root || !q->bfs))) {
    fsx_result_unref(q->candidates);
    fsx_free(q->scope_query.text);
    fsx_free(q->tool);
    fsx_free(q->pattern);
    fsx_free(q->root);
    fsx_free(q->bfs);
    fsx_free(q->queue);
    if (q->mutex)
      cmeta_mutex_destroy(&q->mutex);
    if (q->changed)
      cmeta_cond_destroy(&q->changed);
    fsx_free(q);
    return FS_NOMEM;
  }
  q->scope_query.engine = e;
  q->scope_query.mutex = q->mutex;
  q->scope_query.parent_cancelled = &q->cancelled;
  atomic_init(&q->scope_query.cancelled, false);
  q->scope_query.options = fsx_query_options();
  q->scope_query.options.flags = r->scope_flags;
  q->scope_query.options.timezone_offset_minutes = r->timezone_offset_minutes;
  q->scope_query.options.deadline_ns = r->deadline_ns;
  q->scope_query.text_bytes = r->scope ? strlen(r->scope) : 0;
  if (q->candidates) {
    q->info.candidate_count = q->candidates->info.window_count;
    q->info.candidate_count_known = true;
  }
  q->info.status = FS_WAIT;
  q->info.candidate_source = r->root    ? FS_CONTENT_BFS_LIVE
                             : r->scope ? FS_CONTENT_SCOPE
                                        : FS_CONTENT_WINDOW;
  cmeta_mutex_lock(&e->mutex);
  FsStatus status = FS_BUSY;
  if (!atomic_load(&e->stopping) && !e->content &&
      e->handles < e->config.max_handles) {
    q->metrics = e->content_metrics;
    if (q->root)
      q->coverage_complete = true;
    else {
      q->snapshot = q->candidates ? q->candidates->snapshot : e->current;
      atomic_fetch_add(&q->snapshot->refs, 1);
      q->scope_query.snapshot = q->snapshot;
      q->info.metadata_epoch = q->snapshot->epoch;
      q->coverage_complete = q->candidates
                                 ? q->candidates->info.coverage_complete
                                 : q->snapshot->incomplete == 0;
      q->needs_rescan =
          q->candidates ? q->candidates->info.needs_rescan : e->needs_rescan;
    }
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
      cmeta_cond_broadcast(&q->changed);
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

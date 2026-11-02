#include "internal.h"
#include <stdio.h>
#include <stdlib.h>

FsEngineConfig fsx_engine_config(const char *path) {
  FsEngineConfig c = {0};
  c.struct_size = sizeof(c);
  c.api_version = FSX_API_VERSION;
  c.database_path = path;
  c.worker_threads = 2;
  c.flags = FS_ENGINE_CREATE_DATABASE;
  c.owned_memory_budget = 256u * 1024u * 1024u;
  c.max_entries = 1000000;
  c.max_handles = 32;
  c.max_query_window = 100000;
  c.max_scan_depth = 256;
  return c;
}
FsQueryOptions fsx_query_options(void) {
  FsQueryOptions o = {0};
  o.struct_size = sizeof(o);
  o.api_version = FSX_API_VERSION;
  o.limit = 100;
  return o;
}
const char *fsx_status_name(FsStatus s) {
  const char *text = FsStatus_to_string(s);
  return text ? text : "unknown";
}
uint64_t fsx_engine_now_ns(const FsEngine *e) {
  (void)e;
  return cmeta_hrtime();
}
static void query_unref(FsQuery *q) {
  if (atomic_fetch_sub_explicit(&q->refs, 1, memory_order_acq_rel) != 1)
    return;
  fsx_result_unref(q->result);
  fsx_snapshot_release(q->snapshot);
  fsx_free(q->text);
  cmeta_cond_destroy(&q->changed);
  cmeta_mutex_destroy(&q->mutex);
  fsx_free(q);
}
void fsx_result_unref(FsResult *r) {
  if (!r || atomic_fetch_sub_explicit(&r->refs, 1, memory_order_acq_rel) != 1)
    return;
  fsx_snapshot_release(r->snapshot);
  fsx_free(r->ids);
  fsx_free(r);
}
FsStatus fsx_query_stop(FsQuery *q) {
  if (atomic_load_explicit(&q->cancelled, memory_order_relaxed) ||
      atomic_load_explicit(&q->engine->stopping, memory_order_relaxed))
    return FS_CANCELLED;
  if (q->options.deadline_ns && q->options.deadline_ns != UINT64_MAX &&
      cmeta_hrtime() >= q->options.deadline_ns)
    return FS_TIMEOUT;
  return FS_OK;
}
static void query_run(void *arg) {
  FsQuery *q = arg;
  cmeta_mutex_lock(&q->mutex);
  q->info.state = FS_QUERY_RUNNING;
  cmeta_mutex_unlock(&q->mutex);
  FsResult *result = NULL;
  FsStatus s = fsx_query_stop(q);
  if (s == FS_OK)
    s = fsx_query_execute(q, &result);
  cmeta_mutex_lock(&q->mutex);
  if (s == FS_OK)
    s = fsx_query_stop(q);
  q->info.status = s;
  if (s == FS_OK) {
    q->result = result;
    result = NULL;
    q->info.state = FS_QUERY_READY;
  } else
    q->info.state = s == FS_CANCELLED ? FS_QUERY_CANCELLED : FS_QUERY_FAILED;
  cmeta_cond_broadcast(&q->changed);
  cmeta_mutex_unlock(&q->mutex);
  fsx_result_unref(result);
}
static void query_cancel_pending(void *arg) {
  FsQuery *q = arg;
  cmeta_mutex_lock(&q->mutex);
  q->info.status = FS_CANCELLED;
  q->info.state = FS_QUERY_CANCELLED;
  cmeta_cond_broadcast(&q->changed);
  cmeta_mutex_unlock(&q->mutex);
}
static void query_finalize(void *arg) {
  FsQuery *q = arg;
  FsEngine *e = q->engine;
  cmeta_mutex_lock(&e->mutex);
  FsQuery **link = &e->jobs;
  while (*link && *link != q)
    link = &(*link)->next;
  if (*link)
    *link = q->next;
  cmeta_mutex_unlock(&e->mutex);
  query_unref(q);
  cmeta_mutex_lock(&e->mutex);
  e->active_jobs--;
  cmeta_cond_broadcast(&e->changed);
  cmeta_mutex_unlock(&e->mutex);
}
FsStatus fsx_engine_open(const FsEngineConfig *c, FsEngine **out) {
  if (!out)
    return FS_INVALID;
  *out = NULL;
  if (c && c->struct_size == sizeof(*c) && c->open_error)
    *c->open_error =
        (FsOpenError){FS_INVALID, 0, "invalid engine configuration"};
  if (!c || c->struct_size != sizeof(*c) || c->api_version != FSX_API_VERSION ||
      !c->database_path || !c->database_path[0] ||
      !fsx_utf8(c->database_path, strlen(c->database_path)) ||
      strlen(c->database_path) > 65535 ||
      c->flags & ~(FS_ENGINE_REGEX_JIT | FS_ENGINE_CREATE_DATABASE |
                   FS_ENGINE_SCAN_ONLY) ||
      !c->worker_threads || c->worker_threads > 4 || !c->max_entries ||
      c->max_entries >= UINT32_MAX || !c->max_handles ||
      c->max_handles > 1024 || !c->max_query_window ||
      c->max_query_window > 100000 || !c->max_scan_depth ||
      c->max_scan_depth > 256 || c->owned_memory_budget < 4096)
    return FS_INVALID;
  FsEngine *e = calloc(1, sizeof(*e));
  if (!e)
    return FS_NOMEM;
  e->config = *c;
  e->config.open_error = NULL;
  atomic_init(&e->stopping, false);
  cmeta_mutex_init(&e->mutex);
  cmeta_mutex_init(&e->budget_mutex);
  cmeta_cond_init(&e->changed);
  FsStatus s = FS_NOMEM;
  if (!e->mutex || !e->budget_mutex || !e->changed)
    goto fail;
  e->database_path = fsx_copy(e, c->database_path, strlen(c->database_path));
  if (!e->database_path)
    goto fail;
  e->config.database_path = e->database_path;
  s = fsx_db_open(e);
  if (s != FS_OK)
    goto fail;
  s = fsx_db_status(sqlite3_exec(e->db, "BEGIN", NULL, NULL, NULL));
  if (s != FS_OK)
    goto fail;
  s = fsx_snapshot_load(e, &e->current);
  if (s == FS_OK)
    s = fsx_health_load(e);
  if (s != FS_OK) {
    sqlite3_exec(e->db, "ROLLBACK", NULL, NULL, NULL);
    goto fail;
  }
  s = fsx_db_status(sqlite3_exec(e->db, "COMMIT", NULL, NULL, NULL));
  if (s != FS_OK)
    goto fail;
  cmeta_threadpool_config_t pc = {(int)c->worker_threads, c->max_handles};
  e->pool = cmeta_threadpool_create_with_config(&pc);
  if (!e->pool) {
    s = FS_NOMEM;
    goto fail;
  }
  if (c->open_error)
    *c->open_error = (FsOpenError){0};
  *out = e;
  return FS_OK;
fail:
  if (c->open_error) {
    c->open_error->status = s;
    c->open_error->native_code = e->db ? sqlite3_extended_errcode(e->db) : 0;
    snprintf(c->open_error->diagnostic, sizeof(c->open_error->diagnostic),
             "%s [os=%d]", e->db ? sqlite3_errmsg(e->db) : fsx_status_name(s),
             e->db ? sqlite3_system_errno(e->db) : 0);
  }
  fsx_snapshot_release(e->current);
  fsx_health_destroy(e);
  if (e->db)
    sqlite3_close(e->db);
  fsx_free(e->database_path);
  fsx_free(e->reader_path);
  if (e->changed)
    cmeta_cond_destroy(&e->changed);
  if (e->mutex)
    cmeta_mutex_destroy(&e->mutex);
  if (e->budget_mutex)
    cmeta_mutex_destroy(&e->budget_mutex);
  free(e);
  return s;
}
FsStatus fsx_engine_progress(FsEngine *e, FsProgress *out) {
  if (!e || !out)
    return FS_INVALID;
  cmeta_mutex_lock(&e->mutex);
  *out = (FsProgress){0};
  out->snapshot_epoch = e->current->epoch;
  out->entries = e->current->count;
  out->snapshot_pages = e->current->page_count;
  out->reused_snapshot_pages = e->current->reused_pages;
  out->active_watches = e->watch_count;
  out->dirty_watches = e->dirty_watches;
  out->source_count = e->current->sources;
  out->incomplete_sources = e->current->incomplete;
  out->scanning = e->writer;
  out->stopping = atomic_load(&e->stopping);
  out->needs_rescan = e->needs_rescan;
  cmeta_mutex_unlock(&e->mutex);
  cmeta_mutex_lock(&e->budget_mutex);
  out->owned_bytes = e->owned;
  cmeta_mutex_unlock(&e->budget_mutex);
  return FS_OK;
}
FsStatus fsx_engine_scan(FsEngine *e, const char *root, FsScanInfo *out) {
  if (!out)
    return FS_INVALID;
  *out = (FsScanInfo){0};
  if (!e || !root || !root[0] || strlen(root) > 65535 ||
      !fsx_utf8(root, strlen(root)))
    return FS_INVALID;
  cmeta_mutex_lock(&e->mutex);
  if (atomic_load(&e->stopping) || e->writer) {
    cmeta_mutex_unlock(&e->mutex);
    return FS_BUSY;
  }
  e->writer = true;
  cmeta_mutex_unlock(&e->mutex);
  char *canonical = NULL;
  FsRootHealth *health = NULL;
  FsStatus s = fsx_native_root(e, root, &canonical);
  if (s == FS_OK)
    s = fsx_health_prepare(e, canonical, &health);
  if (s == FS_OK)
    s = fsx_db_scan(e, canonical, out);
  fsx_free(canonical);
  cmeta_mutex_lock(&e->mutex);
  e->writer = false;
  if (s == FS_OK) {
    fsx_health_validate(e, health);
    fsx_ntfs_mark_invalid(e, out->source_id);
  } else
    fsx_health_invalidate(e, health ? health->root : NULL);
  cmeta_cond_broadcast(&e->changed);
  cmeta_mutex_unlock(&e->mutex);
  return s;
}
FsStatus fsx_query_submit(FsEngine *e, const FsQueryRequest *r, FsQuery **out) {
  if (!out)
    return FS_INVALID;
  *out = NULL;
  if (!e || !r || r->struct_size != sizeof(*r) ||
      r->api_version != FSX_API_VERSION)
    return FS_INVALID;
  if (r->text_bytes > 65536)
    return FS_LIMIT;
  if (r->options.struct_size != sizeof(r->options) ||
      r->options.api_version != FSX_API_VERSION ||
      r->options.flags & ~(FS_QUERY_DESCENDING | FS_QUERY_CASE_SENSITIVE |
                           FS_QUERY_ALLOW_PARTIAL_TEXT) ||
      r->options.sort_key > FS_SORT_MTIME ||
      r->options.timezone_offset_minutes < -840 ||
      r->options.timezone_offset_minutes > 840 || !r->text ||
      !fsx_utf8(r->text, r->text_bytes))
    return FS_INVALID;
  if (r->text_bytes > 65536 || (uint64_t)r->options.offset + r->options.limit >
                                   e->config.max_query_window)
    return FS_LIMIT;
  FsQuery *q = fsx_alloc(e, sizeof(*q));
  if (!q)
    return FS_NOMEM;
  memset(q, 0, sizeof(*q));
  q->engine = e;
  q->options = r->options;
  atomic_init(&q->refs, 2);
  atomic_init(&q->cancelled, false);
  q->info.state = FS_QUERY_QUEUED;
  q->info.status = FS_WAIT;
  cmeta_mutex_init(&q->mutex);
  cmeta_cond_init(&q->changed);
  q->text = fsx_copy(e, r->text, r->text_bytes);
  q->text_bytes = r->text_bytes;
  if (!q->mutex || !q->changed || !q->text) {
    if (q->mutex)
      cmeta_mutex_destroy(&q->mutex);
    if (q->changed)
      cmeta_cond_destroy(&q->changed);
    fsx_free(q->text);
    fsx_free(q);
    return FS_NOMEM;
  }
  cmeta_mutex_lock(&e->mutex);
  if (atomic_load(&e->stopping) || e->handles >= e->config.max_handles) {
    cmeta_mutex_unlock(&e->mutex);
    atomic_store(&q->refs, 1);
    query_unref(q);
    return FS_BUSY;
  }
  q->snapshot = e->current;
  atomic_fetch_add(&q->snapshot->refs, 1);
  q->info.snapshot_epoch = q->snapshot->epoch;
  q->needs_rescan = e->needs_rescan;
  q->next = e->jobs;
  e->jobs = q;
  e->handles++;
  e->active_jobs++;
  cmeta_threadpool_task_t task = {query_run, query_cancel_pending,
                                  query_finalize, q};
  int admitted = cmeta_threadpool_try_submit_task(e->pool, &task);
  if (admitted != 0) {
    e->jobs = q->next;
    e->handles--;
    e->active_jobs--;
    cmeta_mutex_unlock(&e->mutex);
    atomic_store(&q->refs, 1);
    query_unref(q);
    return FS_BUSY;
  }
  cmeta_mutex_unlock(&e->mutex);
  *out = q;
  return FS_OK;
}
FsStatus fsx_query_poll(FsQuery *q, FsQueryInfo *out) {
  if (!q || !out)
    return FS_INVALID;
  cmeta_mutex_lock(&q->mutex);
  *out = q->info;
  FsStatus s = q->info.state < FS_QUERY_READY ? FS_WAIT : q->info.status;
  cmeta_mutex_unlock(&q->mutex);
  return s;
}
FsStatus fsx_query_wait(FsQuery *q, uint64_t deadline) {
  if (!q)
    return FS_INVALID;
  cmeta_mutex_lock(&q->mutex);
  while (q->info.state < FS_QUERY_READY) {
    if (!deadline) {
      cmeta_mutex_unlock(&q->mutex);
      return FS_WAIT;
    }
    if (deadline == UINT64_MAX)
      cmeta_cond_wait(&q->changed, &q->mutex);
    else {
      uint64_t now = cmeta_hrtime();
      if (now >= deadline) {
        cmeta_mutex_unlock(&q->mutex);
        return FS_TIMEOUT;
      }
      cmeta_cond_timedwait(&q->changed, &q->mutex, deadline - now);
    }
  }
  FsStatus s = q->info.status;
  cmeta_mutex_unlock(&q->mutex);
  return s;
}
void fsx_query_cancel(FsQuery *q) {
  if (!q)
    return;
  cmeta_mutex_lock(&q->mutex);
  if (q->info.state < FS_QUERY_READY)
    atomic_store(&q->cancelled, true);
  cmeta_mutex_unlock(&q->mutex);
}
void fsx_query_release(FsQuery *q) {
  if (!q)
    return;
  FsEngine *e = q->engine;
  /* Keep the external slot until destruction no longer uses the engine. */
  query_unref(q);
  cmeta_mutex_lock(&e->mutex);
  e->handles--;
  cmeta_cond_broadcast(&e->changed);
  cmeta_mutex_unlock(&e->mutex);
}
FsStatus fsx_query_result_acquire(FsQuery *q, FsResult **out) {
  if (!out)
    return FS_INVALID;
  *out = NULL;
  if (!q)
    return FS_INVALID;
  cmeta_mutex_lock(&q->mutex);
  FsStatus s = q->info.state < FS_QUERY_READY ? FS_WAIT : q->info.status;
  FsResult *r = s == FS_OK ? q->result : NULL;
  if (r)
    atomic_fetch_add(&r->refs, 1);
  cmeta_mutex_unlock(&q->mutex);
  if (!r)
    return s;
  FsEngine *e = q->engine;
  cmeta_mutex_lock(&e->mutex);
  if (atomic_load(&e->stopping) || e->handles >= e->config.max_handles)
    s = FS_BUSY;
  else {
    e->handles++;
    *out = r;
  }
  cmeta_mutex_unlock(&e->mutex);
  if (s != FS_OK)
    fsx_result_unref(r);
  return s;
}
FsStatus fsx_result_info(const FsResult *r, FsResultInfo *out) {
  if (!r || !out)
    return FS_INVALID;
  *out = r->info;
  return FS_OK;
}
FsStatus fsx_result_page(const FsResult *r, uint32_t offset, uint32_t limit,
                         FsHit *dst, size_t cap, size_t *written) {
  if (!written)
    return FS_INVALID;
  *written = 0;
  if (!r || (limit && !dst) || cap < limit || offset > r->info.window_count)
    return FS_INVALID;
  uint32_t n = r->info.window_count - offset;
  if (n > limit)
    n = limit;
  for (uint32_t i = 0; i < n; i++)
    dst[i] = (FsHit){r->snapshot->epoch, r->ids[offset + i], 0};
  *written = n;
  return FS_OK;
}
FsStatus fsx_result_entry(const FsResult *r, FsHit hit, FsEntryView *out) {
  if (!r || !out || hit.reserved)
    return FS_INVALID;
  if (hit.snapshot_epoch != r->snapshot->epoch ||
      hit.entry_id >= r->snapshot->count)
    return FS_STALE;
  const FsEntry *e = fsx_snapshot_entry(r->snapshot, hit.entry_id);
  *out = (FsEntryView){e->key,   e->entry_key,    e->size,
                       e->mtime, e->flags,        e->name,
                       e->path,  strlen(e->name), strlen(e->path)};
  return FS_OK;
}
void fsx_result_release(FsResult *r) {
  if (!r)
    return;
  FsEngine *e = r->engine;
  fsx_result_unref(r);
  cmeta_mutex_lock(&e->mutex);
  e->handles--;
  cmeta_cond_broadcast(&e->changed);
  cmeta_mutex_unlock(&e->mutex);
}
FsStatus fsx_engine_close(FsEngine **owner, uint64_t deadline) {
  if (!owner || !*owner)
    return FS_INVALID;
  FsEngine *e = *owner;
  cmeta_mutex_lock(&e->mutex);
  atomic_store(&e->stopping, true);
  fsx_watch_cancel_all(e);
  for (FsQuery *q = e->jobs; q; q = q->next)
    atomic_store(&q->cancelled, true);
  cmeta_mutex_unlock(&e->mutex);
  cmeta_threadpool_shutdown_with_policy(
      e->pool, SALTS_THREADPOOL_SHUTDOWN_CANCEL_PENDING);
  cmeta_mutex_lock(&e->mutex);
  while (e->handles || e->active_jobs || e->writer) {
    if (!deadline) {
      cmeta_mutex_unlock(&e->mutex);
      return FS_BUSY;
    }
    if (deadline == UINT64_MAX)
      cmeta_cond_wait(&e->changed, &e->mutex);
    else {
      uint64_t now = cmeta_hrtime();
      if (now >= deadline) {
        cmeta_mutex_unlock(&e->mutex);
        return FS_TIMEOUT;
      }
      cmeta_cond_timedwait(&e->changed, &e->mutex, deadline - now);
    }
  }
  cmeta_mutex_unlock(&e->mutex);
  cmeta_threadpool_wait_status(e->pool);
  cmeta_threadpool_destroy(e->pool);
  fsx_snapshot_release(e->current);
  fsx_health_destroy(e);
  sqlite3_close(e->db);
  fsx_free(e->database_path);
  fsx_free(e->reader_path);
  cmeta_cond_destroy(&e->changed);
  cmeta_mutex_destroy(&e->mutex);
  cmeta_mutex_destroy(&e->budget_mutex);
  free(e);
  *owner = NULL;
  return FS_OK;
}

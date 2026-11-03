#include "source/ntfs/ntfs.h"

struct FsIndexJob {
  atomic_uint refs;
  atomic_bool cancelled;
  FsEngine *engine;
  FsNtfs *source;
  char *root;
  cmeta_mutex_t mutex;
  cmeta_cond_t changed;
  FsIndexInfo info;
};

/* Only the admitted writer and its joined native producer call this helper.
 * The pool retains the job until both have stopped using this pointer. */
bool fsx_writer_stopping(FsEngine *e) {
  FsIndexJob *job = atomic_load(&e->index_job);
  return atomic_load(&e->stopping) || atomic_load(&e->writer_cancelled) ||
         (job && atomic_load(&job->cancelled));
}
static void unref(FsIndexJob *job) {
  if (atomic_fetch_sub(&job->refs, 1) != 1)
    return;
  fsx_free(job->root);
  cmeta_cond_destroy(&job->changed);
  cmeta_mutex_destroy(&job->mutex);
  fsx_free(job);
}
static void run(void *context) {
  FsIndexJob *job = context;
  cmeta_mutex_lock(&job->mutex);
  job->info.state = FS_INDEX_RUNNING;
  cmeta_mutex_unlock(&job->mutex);
  FsIndexInfo info = {0};
  FsStatus status =
      job->source
          ? fsx_ntfs_run_admitted(job->source,
                                  job->info.operation == FS_INDEX_NTFS_REBUILD,
                                  &info.ntfs)
          : fsx_engine_scan_admitted(job->engine, job->root, &info.scan);
  cmeta_mutex_lock(&job->mutex);
  job->info.status = status;
  job->info.scan = info.scan;
  job->info.ntfs = info.ntfs;
  cmeta_mutex_unlock(&job->mutex);
}
static void cancel_pending(void *context) {
  FsIndexJob *job = context;
  cmeta_mutex_lock(&job->mutex);
  job->info.status = FS_CANCELLED;
  cmeta_mutex_unlock(&job->mutex);
}
static void finalize(void *context) {
  FsIndexJob *job = context;
  FsEngine *e = job->engine;
  cmeta_mutex_lock(&e->mutex);
  if (atomic_load(&e->index_job) == job) {
    atomic_store(&e->index_job, NULL);
    e->writer = false; /* Includes tasks cancelled before run. */
  }
  if (job->source)
    job->source->async_pending = false;
  cmeta_cond_broadcast(&e->changed);
  cmeta_mutex_unlock(&e->mutex);
  /* Terminal publication follows native/SQL cleanup and session unpinning. */
  cmeta_mutex_lock(&job->mutex);
  job->info.state = job->info.status == FS_OK          ? FS_INDEX_READY
                    : job->info.status == FS_CANCELLED ? FS_INDEX_CANCELLED
                                                       : FS_INDEX_FAILED;
  cmeta_cond_broadcast(&job->changed);
  cmeta_mutex_unlock(&job->mutex);
  unref(job);
  cmeta_mutex_lock(&e->mutex);
  e->active_jobs--;
  cmeta_cond_broadcast(&e->changed);
  cmeta_mutex_unlock(&e->mutex);
}
static FsStatus submit(FsEngine *e, FsNtfs *source, const char *root,
                       uint32_t operation, FsIndexJob **out) {
  if (!out)
    return FS_INVALID;
  *out = NULL;
  if (!e || (!source && (!root || !root[0] || strlen(root) > 65535 ||
                         !fsx_utf8(root, strlen(root)))))
    return FS_INVALID;
  FsIndexJob *job = fsx_alloc(e, sizeof(*job));
  if (!job)
    return FS_NOMEM;
  memset(job, 0, sizeof(*job));
  atomic_init(&job->refs, 2);
  atomic_init(&job->cancelled, false);
  job->engine = e;
  job->source = source;
  job->info.operation = operation;
  job->info.state = FS_INDEX_QUEUED;
  job->info.status = FS_WAIT;
  cmeta_mutex_init(&job->mutex);
  cmeta_cond_init(&job->changed);
  if (root)
    job->root = fsx_copy(e, root, strlen(root));
  if (!job->mutex || !job->changed || (root && !job->root)) {
    if (job->mutex)
      cmeta_mutex_destroy(&job->mutex);
    if (job->changed)
      cmeta_cond_destroy(&job->changed);
    fsx_free(job->root);
    fsx_free(job);
    return FS_NOMEM;
  }
  cmeta_mutex_lock(&e->mutex);
  FsStatus status = FS_BUSY;
  if (!atomic_load(&e->stopping) && !e->writer &&
      (!source || !source->async_pending) &&
      e->handles < e->config.max_handles) {
    e->writer = true;
    atomic_store(&e->writer_cancelled, false);
    atomic_store(&e->index_job, job);
    if (source)
      source->async_pending = true;
    e->handles++;
    e->active_jobs++;
    cmeta_threadpool_task_t task = {run, cancel_pending, finalize, job};
    if (cmeta_threadpool_try_submit_task(e->index_pool, &task) == 0)
      status = FS_OK;
    else {
      e->writer = false;
      atomic_store(&e->index_job, NULL);
      if (source)
        source->async_pending = false;
      e->handles--;
      e->active_jobs--;
    }
  }
  cmeta_mutex_unlock(&e->mutex);
  if (status == FS_OK)
    *out = job;
  else {
    atomic_store(&job->refs, 1);
    unref(job);
  }
  return status;
}
FsStatus fsx_engine_scan_async(FsEngine *e, const char *root,
                               FsIndexJob **out) {
  return submit(e, NULL, root, FS_INDEX_SCAN, out);
}
FsStatus fsx_ntfs_rebuild_async(FsNtfs *n, FsIndexJob **out) {
  return submit(n ? n->engine : NULL, n, NULL, FS_INDEX_NTFS_REBUILD, out);
}
FsStatus fsx_ntfs_sync_async(FsNtfs *n, FsIndexJob **out) {
  return submit(n ? n->engine : NULL, n, NULL, FS_INDEX_NTFS_SYNC, out);
}
FsStatus fsx_index_poll(FsIndexJob *job, FsIndexInfo *out) {
  if (!job || !out)
    return FS_INVALID;
  cmeta_mutex_lock(&job->mutex);
  *out = job->info;
  FsStatus status = out->state < FS_INDEX_READY ? FS_WAIT : out->status;
  cmeta_mutex_unlock(&job->mutex);
  return status;
}
FsStatus fsx_index_wait(FsIndexJob *job, uint64_t deadline) {
  if (!job)
    return FS_INVALID;
  cmeta_mutex_lock(&job->mutex);
  while (job->info.state < FS_INDEX_READY) {
    if (!deadline) {
      cmeta_mutex_unlock(&job->mutex);
      return FS_WAIT;
    }
    if (deadline == UINT64_MAX)
      cmeta_cond_wait(&job->changed, &job->mutex);
    else {
      uint64_t now = cmeta_hrtime();
      if (now >= deadline) {
        cmeta_mutex_unlock(&job->mutex);
        return FS_TIMEOUT;
      }
      cmeta_cond_timedwait(&job->changed, &job->mutex, deadline - now);
    }
  }
  FsStatus status = job->info.status;
  cmeta_mutex_unlock(&job->mutex);
  return status;
}
void fsx_index_cancel(FsIndexJob *job) {
  if (job)
    atomic_store(&job->cancelled, true);
}
void fsx_index_release(FsIndexJob *job) {
  if (!job)
    return;
  FsEngine *e = job->engine;
  unref(job);
  cmeta_mutex_lock(&e->mutex);
  e->handles--;
  cmeta_cond_broadcast(&e->changed);
  cmeta_mutex_unlock(&e->mutex);
}

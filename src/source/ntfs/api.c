#include "ntfs.h"
FsNtfsConfig fsx_ntfs_config(void) {
  FsNtfsConfig config = {sizeof(config),
                         FSX_API_VERSION,
                         65536,
                         16000000,
                         4096,
                         NULL,
                         FS_NTFS_MFT_ENUM,
                         FS_NTFS_RAW_PREFETCH | FS_NTFS_RAW_BATCH_DIRS};
  return config;
}
bool fsx_ntfs_same_identity(const FsNtfsIdentity *a, const FsNtfsIdentity *b) {
  return a->volume_serial == b->volume_serial && a->root_frn == b->root_frn &&
         a->volume_root_frn == b->volume_root_frn &&
         a->journal_id == b->journal_id &&
         !strcmp(a->volume_guid, b->volume_guid);
}
static bool valid(const FsNtfsConfig *c) {
  return c && c->struct_size == sizeof(*c) &&
         c->api_version == FSX_API_VERSION && c->buffer_bytes >= 4096 &&
         c->buffer_bytes <= 1048576 && c->buffer_bytes % 8 == 0 &&
         c->max_mft_records && c->max_mft_records <= 64000000 &&
         c->max_batch_records && c->max_batch_records <= 65536 &&
         c->mft_reader <= FS_NTFS_MFT_RAW &&
         !(c->raw_flags &
           ~(uint64_t)(FS_NTFS_RAW_PREFETCH | FS_NTFS_RAW_BATCH_DIRS));
}
static void set_dirty(FsNtfs *n, bool value) {
  if (n->dirty != value) {
    if (value)
      n->engine->dirty_ntfs++;
    else
      n->engine->dirty_ntfs--;
    n->dirty = value;
  }
  fsx_health_refresh(n->engine);
}
static void dispose(FsNtfs *n) {
  if (n->provider)
    n->ops->close(n->provider);
  fsx_free(n->buffer);
  fsx_free(n->root);
  fsx_free(n);
}
FsStatus fsx_ntfs_open_provider(FsEngine *e, const char *root,
                                const FsNtfsConfig *config,
                                const FsNtfsOps *ops, void *provider,
                                FsNtfs **out) {
  if (!out || !e || !root || !valid(config)) {
    if (provider)
      ops->close(provider);
    if (out)
      *out = NULL;
    return FS_INVALID;
  }
  *out = NULL;
  FsNtfs *n = fsx_alloc(e, sizeof(*n));
  if (!n) {
    ops->close(provider);
    return FS_NOMEM;
  }
  memset(n, 0, sizeof(*n));
  n->engine = e;
  n->config = *config;
  n->config.open_error = NULL;
  n->ops = ops;
  n->provider = provider;
  FsStatus status = fsx_native_root(e, root, &n->root);
  if (status != FS_OK) {
    dispose(n);
    return status;
  }
  n->buffer = fsx_alloc(e, config->buffer_bytes);
  if (!n->buffer) {
    dispose(n);
    return FS_NOMEM;
  }
  cmeta_mutex_lock(&e->mutex);
  if (atomic_load(&e->stopping) || e->writer ||
      e->handles >= e->config.max_handles)
    status = FS_BUSY;
  for (FsNtfs *other = e->ntfs; status == FS_OK && other; other = other->next)
    if (!strcmp(other->root, n->root))
      status = FS_BUSY;
  if (status == FS_OK)
    e->writer = true;
  cmeta_mutex_unlock(&e->mutex);
  if (status != FS_OK) {
    dispose(n);
    return status;
  }
  status = ops->query(provider, &n->identity);
  if (status == FS_OK &&
      (!n->identity.root_frn || !n->identity.volume_root_frn ||
       !n->identity.volume_guid[0] || n->identity.first_usn < 0 ||
       n->identity.lowest_usn < 0 ||
       n->identity.next_usn < n->identity.first_usn ||
       n->identity.next_usn < n->identity.lowest_usn))
    status = FS_CORRUPT;
  if (status == FS_OK)
    status = fsx_ntfs_schema(e);
  if (status == FS_OK)
    status = fsx_health_prepare(e, n->root, &n->health);
  if (status == FS_OK)
    status = fsx_ntfs_checkpoint(n);
  if (status == FS_OK) {
    n->info.volume_serial = n->identity.volume_serial;
    n->info.root_frn = n->identity.root_frn;
    n->info.journal_id = n->identity.journal_id;
    cmeta_mutex_lock(&e->mutex);
    n->next = e->ntfs;
    e->ntfs = n;
    e->handles++;
    set_dirty(n, true);
    e->writer = false;
    cmeta_cond_broadcast(&e->changed);
    cmeta_mutex_unlock(&e->mutex);
    *out = n;
  } else {
    dispose(n);
    cmeta_mutex_lock(&e->mutex);
    e->writer = false;
    cmeta_cond_broadcast(&e->changed);
    cmeta_mutex_unlock(&e->mutex);
  }
  return status;
}
FsStatus fsx_ntfs_open(FsEngine *e, const char *root,
                       const FsNtfsConfig *config, FsNtfs **out) {
  if (!out)
    return FS_INVALID;
  *out = NULL;
  if (!e || !root || !root[0] || strlen(root) > 65535 ||
      !fsx_utf8(root, strlen(root)) || !valid(config))
    return FS_INVALID;
  if (config->open_error)
    memset(config->open_error, 0, sizeof(*config->open_error));
  const FsNtfsOps *ops = NULL;
  void *provider = NULL;
  FsStatus status = fsx_ntfs_native_open(e, root, config, &ops, &provider);
  if (status == FS_OK)
    status = fsx_ntfs_open_provider(e, root, config, ops, provider, out);
  if (config->open_error && status != FS_OK) {
    config->open_error->status = status;
    if (!config->open_error->diagnostic[0])
      strcpy(config->open_error->diagnostic, "session admission");
  }
  return status;
}
static FsStatus run(FsNtfs *n, bool rebuild, FsNtfsInfo *out) {
  if (!n || !out)
    return FS_INVALID;
  FsEngine *e = n->engine;
  cmeta_mutex_lock(&e->mutex);
  if (atomic_load(&e->stopping) || e->writer) {
    FsStatus status = atomic_load(&e->stopping) ? FS_CANCELLED : FS_BUSY;
    if (status == FS_CANCELLED) {
      n->info.status = status;
      n->info.state = FS_NTFS_STOPPED;
      n->info.caught_up = false;
    }
    cmeta_mutex_unlock(&e->mutex);
    *out = n->info;
    return status;
  }
  e->writer = true;
  set_dirty(n, true);
  cmeta_mutex_unlock(&e->mutex);
  FsStatus status =
      !rebuild && n->info.needs_rebuild ? FS_STALE : fsx_ntfs_store(n, rebuild);
  n->info.status = status;
  if (status == FS_STALE || status == FS_UNSUPPORTED || status == FS_CORRUPT)
    n->info.needs_rebuild = true;
  if (status != FS_OK) {
    n->info.caught_up = false;
    n->info.native_error = n->ops->error(n->provider);
    n->info.state = status == FS_CANCELLED  ? FS_NTFS_STOPPED
                    : n->info.needs_rebuild ? FS_NTFS_REBUILD_REQUIRED
                                            : FS_NTFS_FAILED;
  } else
    n->info.state = n->info.caught_up ? FS_NTFS_READY : FS_NTFS_CATCHING_UP;
  cmeta_mutex_lock(&e->mutex);
  if (status == FS_OK && n->info.caught_up)
    fsx_health_validate(e, n->health);
  else if (status != FS_OK)
    fsx_health_invalidate(e, n->root);
  set_dirty(n, status != FS_OK || !n->info.caught_up);
  e->writer = false;
  cmeta_cond_broadcast(&e->changed);
  cmeta_mutex_unlock(&e->mutex);
  *out = n->info;
  return status;
}
FsStatus fsx_ntfs_rebuild(FsNtfs *n, FsNtfsInfo *out) {
  return run(n, true, out);
}
FsStatus fsx_ntfs_sync(FsNtfs *n, FsNtfsInfo *out) {
  return run(n, false, out);
}
void fsx_ntfs_mark_invalid(FsEngine *e, uint32_t source) {
  for (FsNtfs *n = e->ntfs; n; n = n->next)
    if (n->info.source_id == source)
      set_dirty(n, true);
}
FsStatus fsx_ntfs_close(FsNtfs **owner) {
  if (!owner || !*owner)
    return FS_INVALID;
  FsNtfs *n = *owner;
  FsEngine *e = n->engine;
  cmeta_mutex_lock(&e->mutex);
  FsNtfs **slot = &e->ntfs;
  while (*slot != n)
    slot = &(*slot)->next;
  *slot = n->next;
  set_dirty(n, false);
  fsx_health_invalidate(e, n->root);
  cmeta_mutex_unlock(&e->mutex);
  dispose(n);
  *owner = NULL;
  cmeta_mutex_lock(&e->mutex);
  e->handles--;
  cmeta_cond_broadcast(&e->changed);
  cmeta_mutex_unlock(&e->mutex);
  return FS_OK;
}

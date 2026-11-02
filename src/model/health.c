#include "internal.h"

void fsx_health_refresh(FsEngine *e) {
  bool pending = e->unscoped_failure || e->dirty_watches != 0;
  for (FsRootHealth *root = e->root_health; root; root = root->next)
    pending |= root->pending;
  e->needs_rescan = pending;
}
void fsx_health_invalidate(FsEngine *e, const char *name) {
  FsRootHealth *matched = NULL;
  if (name)
    for (FsRootHealth *root = e->root_health; root; root = root->next)
      if (!strcmp(root->root, name)) {
        matched = root;
        break;
      }
  if (matched)
    matched->pending = true;
  else {
    /* Without a reliable root identity, retain every cached obligation. */
    for (FsRootHealth *root = e->root_health; root; root = root->next)
      root->pending = true;
    if (!e->root_health)
      e->unscoped_failure = true;
  }
  fsx_health_refresh(e);
}
void fsx_health_validate(FsEngine *e, FsRootHealth *root) {
  root->pending = false;
  bool pending = false;
  for (FsRootHealth *other = e->root_health; other; other = other->next)
    pending |= other->pending;
  if (!pending)
    e->unscoped_failure = false;
  fsx_health_refresh(e);
}
FsStatus fsx_health_prepare(FsEngine *e, const char *name, FsRootHealth **out) {
  *out = NULL;
  cmeta_mutex_lock(&e->mutex);
  for (FsRootHealth *root = e->root_health; root; root = root->next)
    if (!strcmp(root->root, name)) {
      *out = root;
      break;
    }
  bool full = e->root_health_count == 256;
  cmeta_mutex_unlock(&e->mutex);
  if (*out)
    return FS_OK;
  if (full)
    return FS_LIMIT;
  /* Only the admitted writer (or startup) creates records. No allocator runs
   * under the publication mutex, and records stay alive until engine teardown.
   */
  FsRootHealth *root = fsx_alloc(e, sizeof(*root));
  if (!root)
    return FS_NOMEM;
  memset(root, 0, sizeof(*root));
  root->root = fsx_copy(e, name, strlen(name));
  if (!root->root) {
    fsx_free(root);
    return FS_NOMEM;
  }
  cmeta_mutex_lock(&e->mutex);
  root->next = e->root_health;
  e->root_health = root;
  e->root_health_count++;
  cmeta_mutex_unlock(&e->mutex);
  *out = root;
  return FS_OK;
}
FsStatus fsx_health_load(FsEngine *e) {
  sqlite3_stmt *rows = NULL;
  int rc =
      sqlite3_prepare_v2(e->db, "SELECT root FROM sources", -1, &rows, NULL);
  FsStatus status = fsx_db_status(rc);
  while (status == FS_OK && (rc = sqlite3_step(rows)) == SQLITE_ROW) {
    if (sqlite3_column_type(rows, 0) != SQLITE_TEXT) {
      status = FS_CORRUPT;
      break;
    }
    const char *name = (const char *)sqlite3_column_text(rows, 0);
    int bytes = sqlite3_column_bytes(rows, 0);
    if (!name || bytes <= 0 || bytes > 65535 ||
        !fsx_utf8(name, (size_t)bytes)) {
      status = FS_CORRUPT;
      break;
    }
    FsRootHealth *root;
    status = fsx_health_prepare(e, name, &root);
  }
  if (status == FS_OK && rc != SQLITE_DONE)
    status = fsx_db_status(rc);
  sqlite3_finalize(rows);
  return status;
}
void fsx_health_destroy(FsEngine *e) {
  while (e->root_health) {
    FsRootHealth *root = e->root_health;
    e->root_health = root->next;
    fsx_free(root->root);
    fsx_free(root);
  }
}

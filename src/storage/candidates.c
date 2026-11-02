#include "internal.h"

static int progress(void *context) { return fsx_query_stop(context) != FS_OK; }
static FsStatus status(FsCandidates *cursor, int rc) {
  FsStatus stop = fsx_query_stop(cursor->query);
  return stop == FS_OK ? fsx_db_status(rc) : stop;
}
void fsx_candidates_close(FsCandidates *cursor) {
  sqlite3_finalize(cursor->rows);
  cursor->rows = NULL;
  if (cursor->connection) {
    sqlite3_progress_handler(cursor->connection, 0, NULL, NULL);
    sqlite3_exec(cursor->connection, "ROLLBACK", NULL, NULL, NULL);
    sqlite3_close(cursor->connection);
    cursor->connection = NULL;
  }
}
FsStatus fsx_candidates_open(FsQuery *query, const char *expression,
                             FsCandidates *cursor) {
  memset(cursor, 0, sizeof(*cursor));
  cursor->query = query;
  FsEngine *engine = query->engine;
  if (engine->config.flags & FS_ENGINE_SCAN_ONLY) {
    cursor->strategy = FS_CANDIDATE_SCAN_PROFILE;
    return FS_OK;
  }
  if (!expression)
    return FS_OK;
  if (!engine->reader_path) {
    cursor->strategy = FS_CANDIDATE_SCAN_MEMORY;
    return FS_OK;
  }
  if (!engine->has_database_identity) {
    cursor->strategy = FS_CANDIDATE_SCAN_LEGACY;
    return FS_OK;
  }
  int rc = sqlite3_open_v2(engine->reader_path, &cursor->connection,
                           SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX |
                               SQLITE_OPEN_PRIVATECACHE,
                           NULL);
  if (rc != SQLITE_OK) {
    FsStatus result = status(cursor, rc);
    fsx_candidates_close(cursor);
    return result;
  }
  sqlite3_extended_result_codes(cursor->connection, 1);
  sqlite3_limit(cursor->connection, SQLITE_LIMIT_LENGTH, 1024 * 1024);
  sqlite3_limit(cursor->connection, SQLITE_LIMIT_SQL_LENGTH, 32768);
  sqlite3_db_config(cursor->connection, SQLITE_DBCONFIG_ENABLE_LOAD_EXTENSION,
                    0, NULL);
  sqlite3_progress_handler(cursor->connection, 256, progress, query);
  rc = sqlite3_exec(cursor->connection,
                    "PRAGMA query_only=ON;PRAGMA read_uncommitted=OFF;PRAGMA "
                    "cache_size=-1024;PRAGMA temp_store=FILE;BEGIN;",
                    NULL, NULL, NULL);
  sqlite3_stmt *admission = NULL;
  if (rc == SQLITE_OK)
    rc = sqlite3_prepare_v2(
        cursor->connection,
        "SELECT i.value,m.revision,m.unicode_version,m.text_version FROM "
        "fsx_identity i CROSS JOIN fsx_meta m WHERE i.id=1 AND m.id=1",
        -1, &admission, NULL);
  if (rc == SQLITE_OK)
    rc = sqlite3_step(admission);
  FsStatus result = FS_OK;
  if (rc != SQLITE_ROW)
    result = rc == SQLITE_DONE ? FS_CORRUPT : status(cursor, rc);
  else if (sqlite3_column_type(admission, 0) != SQLITE_BLOB ||
           sqlite3_column_bytes(admission, 0) != 16)
    result = FS_CORRUPT;
  else if (memcmp(engine->database_identity, sqlite3_column_blob(admission, 0),
                  16))
    result = FS_STALE;
  else if (sqlite3_column_int64(admission, 1) < 0 ||
           sqlite3_column_type(admission, 2) != SQLITE_TEXT ||
           strcmp((const char *)sqlite3_column_text(admission, 2),
                  salts_unicode_version()) ||
           sqlite3_column_int(admission, 3) != 2)
    result = FS_CORRUPT;
  else if ((uint64_t)sqlite3_column_int64(admission, 1) !=
           query->snapshot->epoch)
    cursor->strategy = FS_CANDIDATE_SCAN_REVISION;
  sqlite3_finalize(admission);
  if (result != FS_OK || cursor->strategy == FS_CANDIDATE_SCAN_REVISION) {
    fsx_candidates_close(cursor);
    return result;
  }
  rc = sqlite3_prepare_v2(cursor->connection,
                          "SELECT rowid FROM name_fts WHERE name_fts MATCH ?1",
                          -1, &cursor->rows, NULL);
  /* The expression is owned by the worker until this cursor is closed. */
  if (rc == SQLITE_OK)
    rc = sqlite3_bind_text(cursor->rows, 1, expression, -1, SQLITE_STATIC);
  if (rc != SQLITE_OK) {
    result = status(cursor, rc);
    fsx_candidates_close(cursor);
    return result;
  }
  cursor->strategy = FS_CANDIDATE_FTS5;
  return FS_OK;
}
FsStatus fsx_candidates_next(FsCandidates *cursor, uint32_t *out) {
  FsStatus stop = fsx_query_stop(cursor->query);
  if (stop != FS_OK)
    return stop;
  FsSnapshot *snapshot = cursor->query->snapshot;
  if (!cursor->rows) {
    if (cursor->next_entry == snapshot->count)
      return FS_END;
    *out = cursor->next_entry++;
    return FS_OK;
  }
  int rc = sqlite3_step(cursor->rows);
  if (rc == SQLITE_DONE)
    return FS_END;
  if (rc != SQLITE_ROW)
    return status(cursor, rc);
  sqlite3_int64 key = sqlite3_column_int64(cursor->rows, 0);
  if (key <= 0)
    return FS_CORRUPT;
  uint32_t first = 0, last = snapshot->count;
  while (first < last) {
    uint32_t middle = first + (last - first) / 2;
    if (snapshot->entries[middle].entry_key < (uint64_t)key)
      first = middle + 1;
    else
      last = middle;
  }
  if (first == snapshot->count ||
      snapshot->entries[first].entry_key != (uint64_t)key)
    return FS_CORRUPT;
  *out = first;
  return FS_OK;
}

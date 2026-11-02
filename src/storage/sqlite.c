#include "internal.h"
#include <stdio.h>
#include <stdlib.h>

FsStatus fsx_db_status(int rc) {
  switch (rc & 255) {
  case SQLITE_OK:
  case SQLITE_DONE:
  case SQLITE_ROW:
    return FS_OK;
  case SQLITE_NOMEM:
    return FS_NOMEM;
  case SQLITE_BUSY:
  case SQLITE_LOCKED:
    return FS_BUSY;
  case SQLITE_FULL:
  case SQLITE_TOOBIG:
    return FS_LIMIT;
  case SQLITE_PERM:
  case SQLITE_AUTH:
  case SQLITE_READONLY:
    return FS_DENIED;
  case SQLITE_CORRUPT:
  case SQLITE_NOTADB:
  case SQLITE_CONSTRAINT:
    return FS_CORRUPT;
  default:
    return FS_IO;
  }
}
static FsStatus sql(FsEngine *e, const char *text) {
  return fsx_db_status(sqlite3_exec(e->db, text, NULL, NULL, NULL));
}
static int64_t scalar(FsEngine *e, const char *text, int *rc) {
  sqlite3_stmt *s = NULL;
  *rc = sqlite3_prepare_v2(e->db, text, -1, &s, NULL);
  int64_t value = 0;
  if (*rc == SQLITE_OK) {
    *rc = sqlite3_step(s);
    if (*rc == SQLITE_ROW)
      value = sqlite3_column_int64(s, 0);
  }
  sqlite3_finalize(s);
  return value;
}
static void encode64(unsigned char b[8], uint64_t v) {
  for (unsigned i = 0; i < 8; i++)
    b[i] = (unsigned char)(v >> (i * 8));
}
static uint64_t decode64(const unsigned char *p) {
  uint64_t v = 0;
  for (unsigned i = 0; i < 8; i++)
    v |= (uint64_t)p[i] << (8 * i);
  return v;
}
FsStatus fsx_db_open(FsEngine *e) {
  int rc = sqlite3_open_v2(e->database_path, &e->db,
                           SQLITE_OPEN_READWRITE | SQLITE_OPEN_FULLMUTEX |
                               ((e->config.flags & FS_ENGINE_CREATE_DATABASE)
                                    ? SQLITE_OPEN_CREATE
                                    : 0),
                           NULL);
  if (rc != SQLITE_OK)
    return fsx_db_status(rc);
  sqlite3_extended_result_codes(e->db, 1);
  sqlite3_busy_timeout(e->db, 1000);
  sqlite3_limit(e->db, SQLITE_LIMIT_LENGTH, 1024 * 1024);
  sqlite3_limit(e->db, SQLITE_LIMIT_SQL_LENGTH, 32768);
  sqlite3_db_config(e->db, SQLITE_DBCONFIG_ENABLE_LOAD_EXTENSION, 0, NULL);
  FsStatus st = sql(e, "PRAGMA foreign_keys=ON; PRAGMA cache_size=-4096; "
                       "PRAGMA temp_store=FILE;");
  if (st != FS_OK)
    return st;
  int64_t version = scalar(e, "PRAGMA user_version", &rc);
  if (rc != SQLITE_ROW)
    return fsx_db_status(rc);
  int64_t app = scalar(e, "PRAGMA application_id", &rc);
  if (rc != SQLITE_ROW)
    return fsx_db_status(rc);
  if (!version && !app) {
    int64_t tables = scalar(
        e, "SELECT count(*) FROM sqlite_schema WHERE name NOT LIKE 'sqlite_%'",
        &rc);
    if (rc != SQLITE_ROW)
      return fsx_db_status(rc);
    if (tables || !(e->config.flags & FS_ENGINE_CREATE_DATABASE))
      return FS_CORRUPT;
    st = sql(
        e, "BEGIN IMMEDIATE;"
           "CREATE TABLE fsx_meta(id INTEGER PRIMARY KEY CHECK(id=1),revision "
           "INTEGER NOT NULL,unicode_version TEXT NOT NULL,text_version "
           "INTEGER NOT NULL);"
           "INSERT INTO fsx_meta VALUES(1,0,'17.0.0',2);"
           "CREATE TABLE sources(source_id INTEGER PRIMARY KEY "
           "AUTOINCREMENT,root TEXT NOT NULL UNIQUE,volume BLOB NOT NULL "
           "CHECK(length(volume)=8),complete INTEGER NOT NULL,skipped INTEGER "
           "NOT NULL,unsupported INTEGER NOT NULL);"
           "CREATE TABLE objects(object_key INTEGER PRIMARY KEY,source_id "
           "INTEGER NOT NULL REFERENCES sources(source_id),file_id BLOB NOT "
           "NULL CHECK(length(file_id)=16),size BLOB NOT NULL "
           "CHECK(length(size)=8),mtime BLOB NOT NULL "
           "CHECK(length(mtime)=8),flags INTEGER NOT "
           "NULL,UNIQUE(source_id,file_id));"
           "CREATE TABLE entries(entry_key INTEGER PRIMARY KEY "
           "AUTOINCREMENT,source_id INTEGER NOT NULL REFERENCES "
           "sources(source_id),object_key INTEGER NOT NULL REFERENCES "
           "objects(object_key),name TEXT NOT NULL,path TEXT NOT "
           "NULL,name_fold TEXT NOT NULL,path_fold TEXT NOT NULL,raw_name BLOB "
           "NOT NULL,seen INTEGER NOT NULL,UNIQUE(source_id,path));"
           "CREATE INDEX entries_object ON entries(object_key);"
           "CREATE INDEX entries_seen ON entries(source_id,seen);"
           "CREATE VIRTUAL TABLE name_fts USING "
           "fts5(name,name_fold,content='entries',content_rowid='entry_key',"
           "tokenize='trigram case_sensitive 1',detail=full);"
           "CREATE TRIGGER entries_ai AFTER INSERT ON entries BEGIN INSERT "
           "INTO name_fts(rowid,name,name_fold) "
           "VALUES(new.entry_key,new.name,new.name_fold); END;"
           "CREATE TRIGGER entries_ad AFTER DELETE ON entries BEGIN INSERT "
           "INTO name_fts(name_fts,rowid,name,name_fold) "
           "VALUES('delete',old.entry_key,old.name,old.name_fold); END;"
           "CREATE TRIGGER entries_au AFTER UPDATE ON entries BEGIN INSERT "
           "INTO name_fts(name_fts,rowid,name,name_fold) "
           "VALUES('delete',old.entry_key,old.name,old.name_fold); INSERT INTO "
           "name_fts(rowid,name,name_fold) "
           "VALUES(new.entry_key,new.name,new.name_fold); END;"
           "PRAGMA application_id=1179867185; PRAGMA user_version=1; COMMIT;");
    if (st != FS_OK) {
      sql(e, "ROLLBACK");
      return st == FS_IO ? FS_UNSUPPORTED : st;
    }
  } else if (version != 1 || app != 1179867185)
    return FS_UNSUPPORTED;
  sqlite3_stmt *s = NULL;
  rc = sqlite3_prepare_v2(
      e->db, "SELECT unicode_version,text_version FROM fsx_meta WHERE id=1", -1,
      &s, NULL);
  if (rc == SQLITE_OK)
    rc = sqlite3_step(s);
  bool compatible = rc == SQLITE_ROW &&
                    sqlite3_column_type(s, 0) == SQLITE_TEXT &&
                    strcmp((const char *)sqlite3_column_text(s, 0),
                           salts_unicode_version()) == 0 &&
                    sqlite3_column_int(s, 1) == 2;
  sqlite3_finalize(s);
  if (!compatible)
    return rc == SQLITE_ROW ? FS_UNSUPPORTED : FS_CORRUPT;
  return sql(e, "PRAGMA journal_mode=WAL; PRAGMA synchronous=FULL;");
}
static FsStatus column_copy(FsEngine *e, sqlite3_stmt *s, int col, char **out) {
  *out = NULL;
  if (sqlite3_column_type(s, col) != SQLITE_TEXT)
    return FS_CORRUPT;
  const char *p = (const char *)sqlite3_column_text(s, col);
  int n = sqlite3_column_bytes(s, col);
  int maximum = col >= 5 ? 262144 : 65535;
  if (!p || n < 0 || n > maximum || !fsx_utf8(p, (size_t)n))
    return FS_CORRUPT;
  *out = fsx_copy(e, p, (size_t)n);
  return *out ? FS_OK : FS_NOMEM;
}
FsStatus fsx_snapshot_load(FsEngine *e, FsSnapshot **out) {
  *out = NULL;
  int rc = 0;
  int64_t count = scalar(e, "SELECT count(*) FROM entries", &rc);
  if (rc != SQLITE_ROW)
    return fsx_db_status(rc);
  if (count < 0 || (uint64_t)count > e->config.max_entries ||
      (uint64_t)count > SIZE_MAX / sizeof(FsEntry))
    return FS_LIMIT;
  FsSnapshot *snap = fsx_alloc(e, sizeof(*snap));
  if (!snap)
    return FS_NOMEM;
  memset(snap, 0, sizeof(*snap));
  atomic_init(&snap->refs, 1);
  snap->engine = e;
  snap->epoch =
      (uint64_t)scalar(e, "SELECT revision FROM fsx_meta WHERE id=1", &rc);
  if (rc != SQLITE_ROW || snap->epoch > INT64_MAX) {
    fsx_free(snap);
    return FS_CORRUPT;
  }
  sqlite3_stmt *q = NULL;
  FsStatus status = FS_OK;
  rc = sqlite3_prepare_v2(e->db,
                          "SELECT "
                          "count(*),coalesce(sum(complete=0),0),coalesce(sum("
                          "unsupported),0) FROM sources",
                          -1, &q, NULL);
  if (rc == SQLITE_OK)
    rc = sqlite3_step(q);
  if (rc != SQLITE_ROW || sqlite3_column_int64(q, 0) > 256 ||
      sqlite3_column_int64(q, 2) < 0) {
    status = FS_CORRUPT;
    goto done;
  }
  snap->sources = (uint32_t)sqlite3_column_int(q, 0);
  snap->incomplete = (uint32_t)sqlite3_column_int(q, 1);
  snap->unsupported = (uint64_t)sqlite3_column_int64(q, 2);
  sqlite3_finalize(q);
  q = NULL;
  if (count) {
    snap->entries = fsx_alloc(e, (size_t)count * sizeof(FsEntry));
    if (!snap->entries) {
      status = FS_NOMEM;
      goto done;
    }
    memset(snap->entries, 0, (size_t)count * sizeof(FsEntry));
  }
  rc = sqlite3_prepare_v2(
      e->db,
      "SELECT "
      "e.entry_key,e.source_id,o.file_id,e.name,e.path,e.name_fold,e.path_fold,"
      "o.size,o.mtime,o.flags FROM entries e JOIN objects o ON "
      "e.object_key=o.object_key AND e.source_id=o.source_id ORDER BY "
      "e.entry_key",
      -1, &q, NULL);
  if (rc != SQLITE_OK) {
    status = fsx_db_status(rc);
    goto done;
  }
  while ((rc = sqlite3_step(q)) == SQLITE_ROW) {
    if (snap->count >= (uint64_t)count || sqlite3_column_int64(q, 0) <= 0 ||
        sqlite3_column_int64(q, 1) <= 0 ||
        sqlite3_column_int64(q, 1) > UINT32_MAX ||
        sqlite3_column_bytes(q, 2) != 16 || sqlite3_column_bytes(q, 7) != 8 ||
        sqlite3_column_bytes(q, 8) != 8) {
      status = FS_CORRUPT;
      goto done;
    }
    FsEntry *item = &snap->entries[snap->count++];
    item->entry_key = (uint64_t)sqlite3_column_int64(q, 0);
    item->key.source_id = (uint32_t)sqlite3_column_int(q, 1);
    const unsigned char *id = sqlite3_column_blob(q, 2);
    item->key.file_id_lo = decode64(id);
    item->key.file_id_hi = decode64(id + 8);
    item->size = decode64(sqlite3_column_blob(q, 7));
    item->mtime = decode64(sqlite3_column_blob(q, 8));
    item->flags = (uint32_t)sqlite3_column_int(q, 9);
    if (item->flags & ~15u) {
      status = FS_CORRUPT;
      goto done;
    }
    status = column_copy(e, q, 3, &item->name);
    if (status != FS_OK)
      goto done;
    status = column_copy(e, q, 4, &item->path);
    if (status != FS_OK)
      goto done;
    status = column_copy(e, q, 5, &item->name_fold);
    if (status != FS_OK)
      goto done;
    status = column_copy(e, q, 6, &item->path_fold);
    if (status != FS_OK)
      goto done;
    char *fold = NULL;
    status = fsx_fold(e, item->name, &fold);
    if (status != FS_OK)
      goto done;
    bool equal = strcmp(fold, item->name_fold) == 0;
    fsx_free(fold);
    if (!equal) {
      status = FS_CORRUPT;
      goto done;
    }
    status = fsx_fold(e, item->path, &fold);
    if (status != FS_OK)
      goto done;
    equal = strcmp(fold, item->path_fold) == 0;
    fsx_free(fold);
    if (!equal) {
      status = FS_CORRUPT;
      goto done;
    }
  }
  if (rc != SQLITE_DONE)
    status = fsx_db_status(rc);
  else if (snap->count != (uint64_t)count)
    status = FS_CORRUPT;
done:
  sqlite3_finalize(q);
  if (status != FS_OK)
    fsx_snapshot_release(snap);
  else
    *out = snap;
  return status;
}
typedef struct StoreScan {
  FsEngine *engine;
  sqlite3_stmt *object, *entry;
  uint32_t source;
  uint64_t revision, count;
} StoreScan;
static FsStatus observe(void *ctx, const FsObserved *o) {
  StoreScan *s = ctx;
  FsEngine *e = s->engine;
  if (atomic_load(&e->stopping))
    return FS_CANCELLED;
  if (++s->count > e->config.max_entries)
    return FS_LIMIT;
  unsigned char id[16], size[8], mtime[8];
  encode64(id, o->file_lo);
  encode64(id + 8, o->file_hi);
  encode64(size, o->size);
  encode64(mtime, o->mtime);
  sqlite3_stmt *q = s->object;
  sqlite3_bind_int64(q, 1, s->source);
  sqlite3_bind_blob(q, 2, id, 16, SQLITE_TRANSIENT);
  sqlite3_bind_blob(q, 3, size, 8, SQLITE_TRANSIENT);
  sqlite3_bind_blob(q, 4, mtime, 8, SQLITE_TRANSIENT);
  sqlite3_bind_int(q, 5, (int)o->flags);
  int rc = sqlite3_step(q);
  int64_t object_key = rc == SQLITE_ROW ? sqlite3_column_int64(q, 0) : 0;
  if (rc == SQLITE_ROW)
    rc = sqlite3_step(q);
  sqlite3_reset(q);
  sqlite3_clear_bindings(q);
  if (rc != SQLITE_DONE)
    return fsx_db_status(rc);
  char *name_fold = NULL, *path_fold = NULL;
  FsStatus st = fsx_fold(e, o->name, &name_fold);
  if (st == FS_OK)
    st = fsx_fold(e, o->path, &path_fold);
  if (st != FS_OK) {
    fsx_free(name_fold);
    fsx_free(path_fold);
    return st;
  }
  q = s->entry;
  sqlite3_bind_int64(q, 1, s->source);
  sqlite3_bind_int64(q, 2, object_key);
  sqlite3_bind_text(q, 3, o->name, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(q, 4, o->path, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(q, 5, name_fold, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(q, 6, path_fold, -1, SQLITE_TRANSIENT);
  sqlite3_bind_blob64(q, 7, o->raw_name, (sqlite3_uint64)o->raw_bytes,
                      SQLITE_TRANSIENT);
  sqlite3_bind_int64(q, 8, (sqlite3_int64)s->revision);
  rc = sqlite3_step(q);
  sqlite3_reset(q);
  sqlite3_clear_bindings(q);
  fsx_free(name_fold);
  fsx_free(path_fold);
  return fsx_db_status(rc);
}
FsStatus fsx_db_scan(FsEngine *e, const char *root, FsScanInfo *out) {
  FsStatus st = sql(e, "BEGIN IMMEDIATE");
  if (st != FS_OK)
    return st;
  StoreScan s = {0};
  s.engine = e;
  FsSnapshot *snapshot = NULL;
  uint64_t volume = 0;
  unsigned char volume_bytes[8];
  st = fsx_native_volume(e, root, &volume);
  if (st != FS_OK)
    goto done;
  encode64(volume_bytes, volume);
  int rc = 0;
  int64_t rev = scalar(e, "SELECT revision FROM fsx_meta WHERE id=1", &rc);
  if (rc != SQLITE_ROW || rev < 0) {
    st = FS_CORRUPT;
    goto done;
  }
  if (rev == INT64_MAX) {
    st = FS_LIMIT;
    goto done;
  }
  s.revision = (uint64_t)rev + 1;
  sqlite3_stmt *q = NULL;
  rc = sqlite3_prepare_v2(e->db,
                          "SELECT source_id,volume FROM sources WHERE root=?1",
                          -1, &q, NULL);
  if (rc == SQLITE_OK) {
    sqlite3_bind_text(q, 1, root, -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(q);
  }
  if (rc == SQLITE_ROW) {
    if (sqlite3_column_bytes(q, 1) != 8 ||
        memcmp(sqlite3_column_blob(q, 1), volume_bytes, 8)) {
      st = FS_STALE;
      sqlite3_finalize(q);
      goto done;
    }
    s.source = (uint32_t)sqlite3_column_int(q, 0);
  } else if (rc != SQLITE_DONE) {
    st = fsx_db_status(rc);
    sqlite3_finalize(q);
    goto done;
  }
  sqlite3_finalize(q);
  q = NULL;
  if (!s.source) {
    int64_t count = scalar(e, "SELECT count(*) FROM sources", &rc);
    if (rc != SQLITE_ROW) {
      st = fsx_db_status(rc);
      goto done;
    }
    if (count >= 256) {
      st = FS_LIMIT;
      goto done;
    }
    rc = sqlite3_prepare_v2(
        e->db,
        "INSERT INTO sources(root,volume,complete,skipped,unsupported) "
        "VALUES(?1,?2,0,0,0)",
        -1, &q, NULL);
    if (rc == SQLITE_OK) {
      sqlite3_bind_text(q, 1, root, -1, SQLITE_TRANSIENT);
      sqlite3_bind_blob(q, 2, volume_bytes, 8, SQLITE_TRANSIENT);
      rc = sqlite3_step(q);
    }
    sqlite3_finalize(q);
    q = NULL;
    if (rc != SQLITE_DONE) {
      st = fsx_db_status(rc);
      goto done;
    }
    int64_t id = sqlite3_last_insert_rowid(e->db);
    if (id <= 0 || id > UINT32_MAX) {
      st = FS_LIMIT;
      goto done;
    }
    s.source = (uint32_t)id;
  }
  rc = sqlite3_prepare_v2(
      e->db,
      "INSERT INTO objects(source_id,file_id,size,mtime,flags) "
      "VALUES(?1,?2,?3,?4,?5) ON CONFLICT(source_id,file_id) DO UPDATE SET "
      "size=excluded.size,mtime=excluded.mtime,flags=excluded.flags RETURNING "
      "object_key",
      -1, &s.object, NULL);
  if (rc != SQLITE_OK) {
    st = fsx_db_status(rc);
    goto done;
  }
  rc = sqlite3_prepare_v2(
      e->db,
      "INSERT INTO "
      "entries(source_id,object_key,name,path,name_fold,path_fold,raw_name,"
      "seen) VALUES(?1,?2,?3,?4,?5,?6,?7,?8) ON CONFLICT(source_id,path) DO "
      "UPDATE SET "
      "object_key=excluded.object_key,name=excluded.name,name_fold=excluded."
      "name_fold,path_fold=excluded.path_fold,raw_name=excluded.raw_name,seen="
      "excluded.seen",
      -1, &s.entry, NULL);
  if (rc != SQLITE_OK) {
    st = fsx_db_status(rc);
    goto done;
  }
  FsWalk walk = {e, observe, &s, 0, 0};
  st = fsx_walk(&walk, root);
  if (st != FS_OK)
    goto done;
  char command[256];
  snprintf(command, sizeof(command),
           "DELETE FROM entries WHERE source_id=%u AND seen!=%llu; DELETE FROM "
           "objects WHERE NOT EXISTS(SELECT 1 FROM entries WHERE "
           "entries.object_key=objects.object_key);",
           s.source, (unsigned long long)s.revision);
  st = sql(e, command);
  if (st != FS_OK)
    goto done;
  snprintf(command, sizeof(command),
           "UPDATE sources SET complete=%u,skipped=%llu,unsupported=%llu WHERE "
           "source_id=%u; UPDATE fsx_meta SET revision=%llu WHERE id=1;",
           walk.skipped == 0 ? 1u : 0u, (unsigned long long)walk.skipped,
           (unsigned long long)walk.unsupported, s.source,
           (unsigned long long)s.revision);
  st = sql(e, command);
  if (st != FS_OK)
    goto done;
  st = fsx_snapshot_load(e, &snapshot);
  if (st != FS_OK)
    goto done;
  st = sql(e, "COMMIT");
  if (st != FS_OK)
    goto done;
  cmeta_mutex_lock(&e->mutex);
  FsSnapshot *old = e->current;
  e->current = snapshot;
  snapshot = NULL;
  cmeta_mutex_unlock(&e->mutex);
  fsx_snapshot_release(old);
  *out = (FsScanInfo){s.revision,       s.count,  walk.skipped,
                      walk.unsupported, s.source, walk.skipped == 0};
done:
  sqlite3_finalize(s.object);
  sqlite3_finalize(s.entry);
  if (st != FS_OK)
    sql(e, "ROLLBACK");
  fsx_snapshot_release(snapshot);
  return st;
}

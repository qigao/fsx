#include "internal.h"
#include <stdio.h>
#include <stdlib.h>

FsStatus fsx_db_status(int rc) {
  switch (rc & 255) {
  case SQLITE_OK:
  case SQLITE_DONE:
  case SQLITE_ROW:
    return FS_OK;
  case SQLITE_INTERRUPT:
    return FS_CANCELLED;
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
/* Optional schema-v1 capability. Existing readers ignore this additive table.
 */
static FsStatus database_identity(FsEngine *e) {
  int rc = SQLITE_OK;
  int64_t exists = scalar(
      e, "SELECT count(*) FROM sqlite_schema WHERE name='fsx_identity'", &rc);
  if (rc != SQLITE_ROW)
    return fsx_db_status(rc);
  if (!exists && (e->config.flags & FS_ENGINE_CREATE_DATABASE)) {
    FsStatus status =
        sql(e, "BEGIN IMMEDIATE;"
               "CREATE TABLE IF NOT EXISTS fsx_identity(id INTEGER PRIMARY KEY "
               "CHECK(id=1),value BLOB NOT NULL CHECK(length(value)=16));"
               "INSERT OR IGNORE INTO fsx_identity VALUES(1,randomblob(16));"
               "COMMIT;");
    if (status != FS_OK) {
      sql(e, "ROLLBACK");
      return status;
    }
    exists = 1;
  }
  if (exists) {
    sqlite3_stmt *row = NULL;
    rc = sqlite3_prepare_v2(e->db, "SELECT value FROM fsx_identity WHERE id=1",
                            -1, &row, NULL);
    if (rc == SQLITE_OK)
      rc = sqlite3_step(row);
    FsStatus status = FS_CORRUPT;
    if (rc == SQLITE_ROW && sqlite3_column_type(row, 0) == SQLITE_BLOB &&
        sqlite3_column_bytes(row, 0) == 16) {
      memcpy(e->database_identity, sqlite3_column_blob(row, 0), 16);
      e->has_database_identity = true;
      status = FS_OK;
    } else if (rc != SQLITE_ROW && rc != SQLITE_DONE)
      status = fsx_db_status(rc);
    sqlite3_finalize(row);
    if (status != FS_OK)
      return status;
  }
  const char *filename = sqlite3_db_filename(e->db, "main");
  if (filename && filename[0]) {
    e->reader_path = fsx_copy(e, filename, strlen(filename));
    if (!e->reader_path)
      return FS_NOMEM;
  }
  return FS_OK;
}
static int writer_progress(void *context) {
  FsEngine *e = context;
  if (fsx_writer_stopping(e))
    return 1;
  return e->sql_observer ? e->sql_observer(e->sql_observer_context) : 0;
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
  sqlite3_progress_handler(e->db, 2048, writer_progress, e);
  sqlite3_extended_result_codes(e->db, 1);
  sqlite3_busy_timeout(e->db, 1000);
  sqlite3_limit(e->db, SQLITE_LIMIT_LENGTH, 1024 * 1024);
  sqlite3_limit(e->db, SQLITE_LIMIT_SQL_LENGTH, 32768);
  sqlite3_db_config(e->db, SQLITE_DBCONFIG_ENABLE_LOAD_EXTENSION, 0, NULL);
  char settings[192];
  snprintf(settings, sizeof(settings),
           "PRAGMA foreign_keys=ON; PRAGMA main.cache_size=-%u; "
           "PRAGMA temp_store=FILE;",
           e->config.sqlite_cache_kib);
  FsStatus st = sql(e, settings);
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
  st = sql(e, "PRAGMA journal_mode=WAL; PRAGMA synchronous=FULL;");
  if (st == FS_OK)
    st = database_identity(e);
  if (st == FS_OK) {
    st = sql(e, e->config.work_memory_pages
                    ? "ATTACH DATABASE ':memory:' AS fsx_work"
                    : "ATTACH DATABASE '' AS fsx_work");
    if (st == FS_OK && e->config.work_memory_pages) {
      snprintf(settings, sizeof(settings),
               "PRAGMA fsx_work.page_size=4096;"
               "PRAGMA fsx_work.max_page_count=%u;",
               e->config.work_memory_pages);
      st = sql(e, settings);
    }
  }
  return st;
}
/* SQLite owns these views until the next step/finalize. Copy only into private
 * pages. */
static FsStatus column_view(sqlite3_stmt *s, int col, char **out) {
  *out = NULL;
  if (sqlite3_column_type(s, col) != SQLITE_TEXT)
    return FS_CORRUPT;
  const char *p = (const char *)sqlite3_column_text(s, col);
  int n = sqlite3_column_bytes(s, col);
  int maximum = col >= 5 ? 262144 : 65535;
  if (!p || n < 0 || n > maximum || !fsx_utf8(p, (size_t)n))
    return FS_CORRUPT;
  *out = (char *)p;
  return FS_OK;
}
static bool entry_equal(const FsEntry *a, const FsEntry *b) {
  return a->entry_key == b->entry_key && a->key.source_id == b->key.source_id &&
         a->key.file_id_lo == b->key.file_id_lo &&
         a->key.file_id_hi == b->key.file_id_hi && a->size == b->size &&
         a->mtime == b->mtime && a->flags == b->flags &&
         !strcmp(a->name, b->name) && !strcmp(a->path, b->path) &&
         !strcmp(a->name_fold, b->name_fold) &&
         !strcmp(a->path_fold, b->path_fold);
}
static FsStatus validate_fold(FsEngine *e, const char *text,
                              const char *expected) {
  char *fold = NULL;
  FsStatus status = fsx_fold(e, text, &fold);
  if (status != FS_OK)
    return status;
  bool equal = !strcmp(fold, expected);
  fsx_free(fold);
  return equal ? FS_OK : FS_CORRUPT;
}
FsStatus fsx_snapshot_load(FsEngine *e, FsSnapshot **out) {
  *out = NULL;
  int rc = 0;
  int64_t count = scalar(e, "SELECT count(*) FROM entries", &rc);
  if (rc != SQLITE_ROW)
    return fsx_db_status(rc);
  if (count < 0 || (uint64_t)count > e->config.max_entries)
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
  snap->page_count = count ? (uint32_t)((count - 1) / FSX_PAGE_ENTRIES + 1) : 0;
  if (snap->page_count) {
    if (sizeof(*snap->pages) > SIZE_MAX / snap->page_count) {
      status = FS_LIMIT;
      goto done;
    }
    size_t bytes = (size_t)snap->page_count * sizeof(*snap->pages);
    snap->pages = fsx_alloc(e, bytes);
    if (!snap->pages) {
      status = FS_NOMEM;
      goto done;
    }
    memset(snap->pages, 0, bytes);
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
    FsEntry item = {0};
    item.entry_key = (uint64_t)sqlite3_column_int64(q, 0);
    item.key.source_id = (uint32_t)sqlite3_column_int(q, 1);
    const unsigned char *id = sqlite3_column_blob(q, 2);
    item.key.file_id_lo = decode64(id);
    item.key.file_id_hi = decode64(id + 8);
    item.size = decode64(sqlite3_column_blob(q, 7));
    item.mtime = decode64(sqlite3_column_blob(q, 8));
    item.flags = (uint32_t)sqlite3_column_int(q, 9);
    if (item.flags & ~15u) {
      status = FS_CORRUPT;
      goto done;
    }
    status = column_view(q, 3, &item.name);
    if (status == FS_OK)
      status = column_view(q, 4, &item.path);
    if (status == FS_OK)
      status = column_view(q, 5, &item.name_fold);
    if (status == FS_OK)
      status = column_view(q, 6, &item.path_fold);
    if (status != FS_OK)
      goto done;
    uint32_t page_id = snap->count / FSX_PAGE_ENTRIES;
    uint32_t offset = snap->count % FSX_PAGE_ENTRIES;
    uint32_t remaining = (uint32_t)count - page_id * FSX_PAGE_ENTRIES;
    uint32_t capacity =
        remaining < FSX_PAGE_ENTRIES ? remaining : FSX_PAGE_ENTRIES;
    /* The single writer keeps current alive until publication; close waits for
     * it. */
    FsSnapshot *base = e->current;
    FsPage *old =
        base && page_id < base->page_count ? base->pages[page_id] : NULL;
    if (old && old->count != capacity)
      old = NULL;
    bool same = old && entry_equal(&item, &old->entries[offset]);
    if (!same) {
      status = validate_fold(e, item.name, item.name_fold);
      if (status == FS_OK)
        status = validate_fold(e, item.path, item.path_fold);
      if (status != FS_OK)
        goto done;
    }
    FsPage *page = snap->pages[page_id];
    if (!page && !same) {
      page = fsx_page_create(e, capacity);
      if (!page) {
        status = FS_NOMEM;
        goto done;
      }
      snap->pages[page_id] = page;
      for (uint32_t i = 0; i < offset; i++) {
        status = fsx_page_append(e, page, &old->entries[i]);
        if (status != FS_OK)
          goto done;
      }
    }
    if (page) {
      status = fsx_page_append(e, page, &item);
      if (status != FS_OK)
        goto done;
    } else if (offset + 1 == capacity) {
      status = fsx_page_retain(old);
      if (status != FS_OK)
        goto done;
      snap->pages[page_id] = old;
      snap->reused_pages++;
    }
    snap->count++;
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

/* Metadata-only transaction cursor: the loader's first ten columns, followed by
 * expected alias name/path and an ownership relation boolean. The admitted
 * writer guarantees an unchanged namespace and an exact base revision. */
FsStatus fsx_snapshot_patch(FsEngine *e, uint64_t epoch, sqlite3_stmt *rows,
                            FsSnapshot **out) {
  *out = NULL;
  FsSnapshot *base = e->current;
  if (!base || epoch <= base->epoch || epoch > INT64_MAX)
    return FS_INVALID;
  FsSnapshot *next = fsx_alloc(e, sizeof(*next));
  if (!next)
    return FS_NOMEM;
  memset(next, 0, sizeof(*next));
  atomic_init(&next->refs, 1);
  next->engine = e;
  next->epoch = epoch;
  next->count = base->count;
  next->sources = base->sources;
  next->incomplete = base->incomplete;
  next->unsupported = base->unsupported;
  next->page_count = base->page_count;
  FsStatus status = FS_OK;
  if (next->page_count) {
    if (sizeof(*next->pages) > SIZE_MAX / next->page_count) {
      status = FS_LIMIT;
      goto done;
    }
    next->pages = fsx_alloc(e, next->page_count * sizeof(*next->pages));
    if (!next->pages) {
      status = FS_NOMEM;
      goto done;
    }
    memset(next->pages, 0, next->page_count * sizeof(*next->pages));
    for (uint32_t i = 0; i < next->page_count; i++) {
      status = fsx_page_retain(base->pages[i]);
      if (status != FS_OK)
        goto done;
      next->pages[i] = base->pages[i];
      next->reused_pages++;
    }
  }
  uint64_t previous = 0;
  int rc;
  while ((rc = sqlite3_step(rows)) == SQLITE_ROW) {
    if (fsx_writer_stopping(e)) {
      status = FS_CANCELLED;
      goto done;
    }
    sqlite3_int64 key = sqlite3_column_int64(rows, 0),
                  source = sqlite3_column_int64(rows, 1);
    if (sqlite3_column_type(rows, 0) != SQLITE_INTEGER || key <= 0 ||
        (uint64_t)key <= previous || source <= 0 || source > UINT32_MAX ||
        sqlite3_column_bytes(rows, 2) != 16 ||
        sqlite3_column_bytes(rows, 7) != 8 ||
        sqlite3_column_bytes(rows, 8) != 8 ||
        sqlite3_column_int(rows, 12) != 1) {
      status = FS_CORRUPT;
      goto done;
    }
    previous = (uint64_t)key;
    uint32_t lo = 0, hi = base->count;
    while (lo < hi) {
      uint32_t mid = lo + (hi - lo) / 2;
      if (fsx_snapshot_entry(base, mid)->entry_key < (uint64_t)key)
        lo = mid + 1;
      else
        hi = mid;
    }
    if (lo == base->count) {
      status = FS_CORRUPT;
      goto done;
    }
    const FsEntry *old = fsx_snapshot_entry(base, lo);
    const unsigned char *id = sqlite3_column_blob(rows, 2);
    if (old->entry_key != (uint64_t)key ||
        old->key.source_id != (uint32_t)source ||
        old->key.file_id_lo != decode64(id) ||
        old->key.file_id_hi != decode64(id + 8)) {
      status = FS_CORRUPT;
      goto done;
    }
    const char *expected[] = {old->name,      old->path, old->name_fold,
                              old->path_fold, old->name, old->path};
    const int columns[] = {3, 4, 5, 6, 10, 11};
    for (unsigned i = 0; i < 6; i++) {
      char *text = NULL;
      status = column_view(rows, columns[i], &text);
      if (status != FS_OK)
        goto done;
      if (strcmp(text, expected[i])) {
        status = FS_CORRUPT;
        goto done;
      }
    }
    uint64_t size = decode64(sqlite3_column_blob(rows, 7)),
             mtime = decode64(sqlite3_column_blob(rows, 8));
    sqlite3_int64 raw_flags = sqlite3_column_int64(rows, 9);
    uint32_t flags = (uint32_t)raw_flags;
    if (sqlite3_column_type(rows, 9) != SQLITE_INTEGER || raw_flags < 0 ||
        raw_flags > 15) {
      status = FS_CORRUPT;
      goto done;
    }
    if (size == old->size && mtime == old->mtime && flags == old->flags)
      continue;
    uint32_t page_id = lo / FSX_PAGE_ENTRIES, offset = lo % FSX_PAGE_ENTRIES;
    if (next->pages[page_id] == base->pages[page_id]) {
      FsPage *copy = fsx_page_create(e, base->pages[page_id]->count);
      if (!copy) {
        status = FS_NOMEM;
        goto done;
      }
      for (uint32_t i = 0; i < base->pages[page_id]->count; i++) {
        if (fsx_writer_stopping(e))
          status = FS_CANCELLED;
        else
          status = fsx_page_append(e, copy, &base->pages[page_id]->entries[i]);
        if (status != FS_OK) {
          fsx_page_release(copy);
          goto done;
        }
      }
      fsx_page_release(next->pages[page_id]);
      next->pages[page_id] = copy;
      next->reused_pages--;
    }
    FsEntry *item = &next->pages[page_id]->entries[offset];
    item->size = size;
    item->mtime = mtime;
    item->flags = flags;
  }
  if (rc != SQLITE_DONE)
    status = fsx_db_status(rc);
done:
  if (status != FS_OK)
    fsx_snapshot_release(next);
  else
    *out = next;
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
  if (fsx_writer_stopping(e))
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
  st = fsx_ntfs_invalidate_database(e, s.source);
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
  st = fsx_writer_stopping(e) ? FS_CANCELLED : sql(e, "COMMIT");
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

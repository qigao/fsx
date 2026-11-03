#include "ntfs.h"
#include <stdio.h>
#include <stdlib.h>
static FsStatus sql(FsEngine *e, const char *text) {
  return fsx_db_status(sqlite3_exec(e->db, text, NULL, NULL, NULL));
}
static void bind_id(sqlite3_stmt *s, int index, uint64_t id) {
  unsigned char bytes[8];
  fsx_ntfs_put64(bytes, id);
  sqlite3_bind_blob(s, index, bytes, 8, SQLITE_TRANSIENT);
}
static bool id_column(sqlite3_stmt *s, int column, uint64_t *out) {
  if (sqlite3_column_type(s, column) != SQLITE_BLOB ||
      sqlite3_column_bytes(s, column) != 8)
    return false;
  *out = fsx_ntfs_get64(sqlite3_column_blob(s, column));
  return true;
}
static FsStatus number(FsEngine *e, const char *text, int64_t *out) {
  sqlite3_stmt *s = NULL;
  int rc = sqlite3_prepare_v2(e->db, text, -1, &s, NULL);
  if (rc == SQLITE_OK)
    rc = sqlite3_step(s);
  FsStatus status = rc == SQLITE_ROW    ? FS_OK
                    : rc == SQLITE_DONE ? FS_CORRUPT
                                        : fsx_db_status(rc);
  if (status == FS_OK)
    *out = sqlite3_column_int64(s, 0);
  sqlite3_finalize(s);
  return status;
}
FsStatus fsx_ntfs_schema(FsEngine *e) {
  int64_t exists;
  FsStatus status = number(
      e, "SELECT count(*) FROM sqlite_schema WHERE name='fsx_ntfs_sources'",
      &exists);
  if (status != FS_OK)
    return status;
  if (exists)
    return FS_OK;
  if (!(e->config.flags & FS_ENGINE_CREATE_DATABASE))
    return FS_UNSUPPORTED;
  status = sql(
      e, "BEGIN IMMEDIATE;"
         "CREATE TABLE fsx_ntfs_sources(source_id INTEGER PRIMARY KEY "
         "REFERENCES sources(source_id),"
         "profile INTEGER NOT NULL,volume_guid TEXT NOT NULL,serial BLOB NOT "
         "NULL,root BLOB NOT NULL,"
         "volume_root BLOB NOT NULL,journal BLOB NOT NULL,next_usn INTEGER NOT "
         "NULL,valid INTEGER NOT NULL);"
         "CREATE TABLE fsx_ntfs_dirs(source_id INTEGER NOT NULL REFERENCES "
         "sources(source_id),"
         "frn BLOB NOT NULL,parent BLOB NOT NULL,name TEXT,raw BLOB NOT "
         "NULL,attributes INTEGER NOT NULL,"
         "PRIMARY KEY(source_id,frn));"
         "CREATE INDEX fsx_ntfs_dirs_parent ON fsx_ntfs_dirs(source_id,parent);"
         "CREATE TABLE fsx_ntfs_scopes(source_id INTEGER NOT NULL REFERENCES "
         "sources(source_id),frn BLOB NOT NULL,PRIMARY KEY(source_id,frn));"
         "CREATE TABLE fsx_ntfs_names(source_id INTEGER NOT NULL REFERENCES "
         "sources(source_id),"
         "parent BLOB NOT NULL,raw BLOB NOT NULL,frn BLOB NOT NULL,name TEXT "
         "NOT NULL,"
         "object_key INTEGER NOT NULL REFERENCES objects(object_key),entry_key "
         "INTEGER,"
         "seen INTEGER NOT NULL,PRIMARY KEY(source_id,parent,raw));"
         "CREATE INDEX fsx_ntfs_names_file ON fsx_ntfs_names(source_id,frn);"
         "COMMIT;");
  if (status != FS_OK)
    sql(e, "ROLLBACK");
  return status;
}
FsStatus fsx_ntfs_invalidate_database(FsEngine *e, uint32_t source) {
  int64_t exists;
  FsStatus status = number(
      e, "SELECT count(*) FROM sqlite_schema WHERE name='fsx_ntfs_sources'",
      &exists);
  if (status != FS_OK || !exists)
    return status;
  char command[320];
  snprintf(command, sizeof(command),
           "UPDATE fsx_ntfs_sources SET valid=0 WHERE source_id=%u;"
           "DELETE FROM fsx_ntfs_names WHERE source_id=%u;"
           "DELETE FROM fsx_ntfs_scopes WHERE source_id=%u;",
           source, source, source);
  return sql(e, command);
}

typedef struct Checkpoint {
  uint32_t source;
  bool present, valid;
  FsNtfsIdentity identity;
  int64_t cursor;
} Checkpoint;
static FsStatus checkpoint(FsNtfs *n, Checkpoint *out) {
  memset(out, 0, sizeof(*out));
  sqlite3_stmt *s = NULL;
  int rc =
      sqlite3_prepare_v2(n->engine->db,
                         "SELECT "
                         "s.source_id,n.profile,n.volume_guid,n.serial,n.root,"
                         "n.volume_root,n.journal,"
                         "n.next_usn,n.valid FROM sources s LEFT JOIN "
                         "fsx_ntfs_sources n USING(source_id) WHERE s.root=?1",
                         -1, &s, NULL);
  if (rc == SQLITE_OK) {
    sqlite3_bind_text(s, 1, n->root, -1, SQLITE_STATIC);
    rc = sqlite3_step(s);
  }
  FsStatus status = FS_OK;
  if (rc == SQLITE_ROW) {
    sqlite3_int64 source = sqlite3_column_int64(s, 0);
    if (source <= 0 || source > UINT32_MAX)
      status = FS_CORRUPT;
    else
      out->source = (uint32_t)source;
    if (sqlite3_column_type(s, 1) != SQLITE_NULL && status == FS_OK) {
      const char *guid = (const char *)sqlite3_column_text(s, 2);
      if (sqlite3_column_int(s, 1) != 1)
        status = FS_UNSUPPORTED;
      else if (!guid || strlen(guid) >= sizeof(out->identity.volume_guid) ||
               !id_column(s, 3, &out->identity.volume_serial) ||
               !id_column(s, 4, &out->identity.root_frn) ||
               !id_column(s, 5, &out->identity.volume_root_frn) ||
               !id_column(s, 6, &out->identity.journal_id) ||
               sqlite3_column_type(s, 7) != SQLITE_INTEGER ||
               sqlite3_column_int64(s, 7) < 0)
        status = FS_CORRUPT;
      else {
        strcpy(out->identity.volume_guid, guid);
        out->cursor = sqlite3_column_int64(s, 7);
        out->present = true;
        out->valid = sqlite3_column_int(s, 8) == 1;
      }
    }
  } else if (rc != SQLITE_DONE)
    status = fsx_db_status(rc);
  sqlite3_finalize(s);
  return status;
}
static bool covered(const FsNtfsIdentity *id, int64_t cursor) {
  return cursor >= id->first_usn && cursor >= id->lowest_usn &&
         cursor <= id->next_usn;
}
FsStatus fsx_ntfs_checkpoint(FsNtfs *n) {
  Checkpoint saved;
  FsStatus status = checkpoint(n, &saved);
  if (status != FS_OK)
    return status;
  n->info.source_id = saved.source;
  n->info.snapshot_epoch = n->engine->current->epoch;
  n->info.next_usn = saved.cursor;
  n->info.target_usn = n->identity.next_usn;
  n->info.needs_rebuild =
      !saved.present || !saved.valid ||
      !fsx_ntfs_same_identity(&n->identity, &saved.identity) ||
      !covered(&n->identity, saved.cursor);
  n->info.state =
      n->info.needs_rebuild ? FS_NTFS_REBUILD_REQUIRED : FS_NTFS_CATCHING_UP;
  return FS_OK;
}
typedef struct Tx {
  FsNtfs *n;
  FsEngine *e;
  uint32_t source;
  uint64_t revision, parent, enumerated, mft_records, journal_records,
      directories, dirty_parents;
  bool all, topology_changed, structural;
  sqlite3_stmt *statements[10];
} Tx;
static int prepare(Tx *t, unsigned slot, const char *text, sqlite3_stmt **out) {
  *out = t->statements[slot];
  int rc = SQLITE_OK;
  if (!*out) {
    rc = sqlite3_prepare_v2(t->e->db, text, -1, out, NULL);
    t->statements[slot] = *out;
  }
  sqlite3_reset(*out);
  sqlite3_clear_bindings(*out);
  return rc;
}
static void finish(Tx *t) {
  for (unsigned i = 0; i < 10; i++)
    sqlite3_finalize(t->statements[i]);
}
static FsStatus source(Tx *t, const FsNtfsIdentity *id) {
  sqlite3_stmt *s = NULL;
  int rc = sqlite3_prepare_v2(
      t->e->db, "SELECT source_id,volume FROM sources WHERE root=?1", -1, &s,
      NULL);
  if (rc == SQLITE_OK) {
    sqlite3_bind_text(s, 1, t->n->root, -1, SQLITE_STATIC);
    rc = sqlite3_step(s);
  }
  FsStatus status = FS_OK;
  if (rc == SQLITE_ROW) {
    uint64_t volume;
    sqlite3_int64 sid = sqlite3_column_int64(s, 0);
    if (sid <= 0 || sid > UINT32_MAX || !id_column(s, 1, &volume))
      status = FS_CORRUPT;
    else if (volume != id->common_volume)
      status = FS_STALE;
    else
      t->source = (uint32_t)sid;
  } else if (rc != SQLITE_DONE)
    status = fsx_db_status(rc);
  sqlite3_finalize(s);
  if (status != FS_OK || t->source)
    return status;
  int64_t count;
  status = number(t->e, "SELECT count(*) FROM sources", &count);
  if (status != FS_OK)
    return status;
  if (count >= 256)
    return FS_LIMIT;
  rc = sqlite3_prepare_v2(
      t->e->db,
      "INSERT INTO sources(root,volume,complete,skipped,unsupported) "
      "VALUES(?1,?2,0,0,0)",
      -1, &s, NULL);
  if (rc == SQLITE_OK) {
    sqlite3_bind_text(s, 1, t->n->root, -1, SQLITE_STATIC);
    bind_id(s, 2, id->common_volume);
    rc = sqlite3_step(s);
  }
  sqlite3_finalize(s);
  if (rc != SQLITE_DONE)
    return fsx_db_status(rc);
  sqlite3_int64 sid = sqlite3_last_insert_rowid(t->e->db);
  if (sid <= 0 || sid > UINT32_MAX)
    return FS_LIMIT;
  t->source = (uint32_t)sid;
  return FS_OK;
}
static FsStatus directory(Tx *t, const FsNtfsRecord *r) {
  if (atomic_load(&t->e->stopping))
    return FS_CANCELLED;
  char *name = NULL;
  FsStatus status = fsx_ntfs_text(t->e, r->name, r->name_bytes, &name);
  if (status != FS_OK && status != FS_UNSUPPORTED)
    return status;
  sqlite3_stmt *s;
  int rc = prepare(t, 0,
                   "INSERT INTO fsx_ntfs_dirs VALUES(?1,?2,?3,?4,?5,?6)"
                   " ON CONFLICT(source_id,frn) DO UPDATE SET "
                   "parent=excluded.parent,name=excluded.name,"
                   "raw=excluded.raw,attributes=excluded.attributes WHERE "
                   "fsx_ntfs_dirs.parent IS NOT excluded.parent OR "
                   "fsx_ntfs_dirs.name IS NOT excluded.name OR "
                   "fsx_ntfs_dirs.raw IS NOT excluded.raw OR "
                   "(fsx_ntfs_dirs.attributes&1040)!=excluded.attributes",
                   &s);
  if (rc == SQLITE_OK) {
    sqlite3_bind_int64(s, 1, t->source);
    bind_id(s, 2, r->frn);
    bind_id(s, 3, r->parent);
    if (name)
      sqlite3_bind_text(s, 4, name, -1, SQLITE_TRANSIENT);
    else
      sqlite3_bind_null(s, 4);
    sqlite3_bind_blob(s, 5, r->name, r->name_bytes, SQLITE_TRANSIENT);
    sqlite3_bind_int64(s, 6, r->attributes & (FSX_NTFS_DIR | FSX_NTFS_REPARSE));
    rc = sqlite3_step(s);
  }
  if (rc == SQLITE_DONE && sqlite3_changes(t->e->db))
    t->topology_changed = t->structural = true;
  fsx_free(name);
  return fsx_db_status(rc);
}
static FsStatus inventory(void *context, const FsNtfsRecord *r) {
  Tx *t = context;
  if (++t->mft_records > t->n->config.max_mft_records)
    return FS_LIMIT;
  return r->attributes & FSX_NTFS_DIR ? directory(t, r) : FS_OK;
}
static FsStatus object(Tx *t, const FsObserved *o, sqlite3_int64 *key) {
  sqlite3_stmt *s;
  int rc = prepare(t, 1,
                   "INSERT INTO objects(source_id,file_id,size,mtime,flags) "
                   "VALUES(?1,?2,?3,?4,?5)"
                   " ON CONFLICT(source_id,file_id) DO UPDATE SET "
                   "size=excluded.size,mtime=excluded.mtime,"
                   "flags=excluded.flags RETURNING object_key",
                   &s);
  if (rc == SQLITE_OK) {
    unsigned char id[16] = {0};
    fsx_ntfs_put64(id, o->file_lo);
    sqlite3_bind_int64(s, 1, t->source);
    sqlite3_bind_blob(s, 2, id, 16, SQLITE_TRANSIENT);
    bind_id(s, 3, o->size);
    bind_id(s, 4, o->mtime);
    sqlite3_bind_int64(s, 5, o->flags);
    rc = sqlite3_step(s);
  }
  FsStatus status = rc == SQLITE_ROW    ? FS_OK
                    : rc == SQLITE_DONE ? FS_CORRUPT
                                        : fsx_db_status(rc);
  if (rc == SQLITE_ROW) {
    *key = sqlite3_column_int64(s, 0);
    rc = sqlite3_step(s);
  }
  if (status == FS_OK && rc != SQLITE_DONE)
    status = fsx_db_status(rc);
  return status;
}
static FsStatus alias(void *context, const FsObserved *o) {
  Tx *t = context;
  if (atomic_load(&t->e->stopping))
    return FS_CANCELLED;
  if (++t->enumerated > t->e->config.max_entries)
    return FS_LIMIT;
  if (!o->name || !fsx_utf8(o->name, strlen(o->name)) || !o->raw_name ||
      !o->raw_bytes || o->raw_bytes > 65534 || o->raw_bytes % 2 || o->file_hi ||
      o->flags & ~15u)
    return FS_CORRUPT;
  sqlite3_int64 key = 0;
  FsStatus status = object(t, o, &key);
  if (status != FS_OK)
    return status;
  sqlite3_stmt *s;
  int rc = prepare(
      t, 2,
      "INSERT INTO fsx_ntfs_names VALUES(?1,?2,?3,?4,?5,?6,"
      "(SELECT entry_key FROM nt_old_keys WHERE parent=?2 AND raw=?3),?7)"
      " ON CONFLICT(source_id,parent,raw) DO UPDATE SET "
      "frn=excluded.frn,name=excluded.name,"
      "object_key=excluded.object_key,seen=excluded.seen",
      &s);
  if (rc == SQLITE_OK) {
    sqlite3_bind_int64(s, 1, t->source);
    bind_id(s, 2, t->parent);
    sqlite3_bind_blob(s, 3, o->raw_name, (int)o->raw_bytes, SQLITE_TRANSIENT);
    bind_id(s, 4, o->file_lo);
    sqlite3_bind_text(s, 5, o->name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(s, 6, key);
    sqlite3_bind_int64(s, 7, (sqlite3_int64)t->revision);
    rc = sqlite3_step(s);
  }
  status = fsx_db_status(rc);
  if (status == FS_OK && o->flags & FS_ENTRY_DIRECTORY) {
    FsNtfsRecord r = {0};
    r.frn = o->file_lo;
    r.parent = t->parent;
    r.attributes =
        FSX_NTFS_DIR | (o->flags & FS_ENTRY_LINK ? FSX_NTFS_REPARSE : 0);
    r.name = o->raw_name;
    r.name_bytes = (uint16_t)o->raw_bytes;
    status = directory(t, &r);
  }
  if (status == FS_OK) {
    int step = prepare(t, 8, "INSERT OR IGNORE INTO nt_refresh VALUES(?1)", &s);
    if (step == SQLITE_OK) {
      bind_id(s, 1, o->file_lo);
      step = sqlite3_step(s);
    }
    status = fsx_db_status(step);
  }
  return status;
}
/* Count successful unique-parent insertions, including multi-row inserts.
 * This transaction owns the temp set; duplicate parents consume no capacity. */
static FsStatus parent_capacity(Tx *t) {
  uint64_t added = (uint64_t)sqlite3_changes(t->e->db);
  if (added > t->n->config.max_mft_records - t->dirty_parents)
    return FS_LIMIT;
  t->dirty_parents += added;
  return FS_OK;
}
static FsStatus dirty_parent(Tx *t, uint64_t parent) {
  sqlite3_stmt *s;
  int rc = prepare(t, 3, "INSERT OR IGNORE INTO nt_dirty VALUES(?1)", &s);
  if (rc == SQLITE_OK) {
    bind_id(s, 1, parent);
    rc = sqlite3_step(s);
  }
  return rc == SQLITE_DONE ? parent_capacity(t) : fsx_db_status(rc);
}
static FsStatus event(void *context, const FsNtfsRecord *r) {
  Tx *t = context;
  if (atomic_load(&t->e->stopping))
    return FS_CANCELLED;
  t->journal_records++;
  if (t->journal_records > t->n->config.max_mft_records)
    return FS_LIMIT;
  FsStatus status = FS_OK;
  uint32_t namespace_reasons = FSX_USN_CREATE | FSX_USN_DELETE | FSX_USN_OLD |
                               FSX_USN_NEW | FSX_USN_LINK;
  if (r->reason & namespace_reasons)
    status = dirty_parent(t, r->parent);
  if (status != FS_OK)
    return status;
  sqlite3_stmt *parent_refresh;
  int parent_step = prepare(t, 8, "INSERT OR IGNORE INTO nt_refresh VALUES(?1)",
                            &parent_refresh);
  if (parent_step == SQLITE_OK) {
    bind_id(parent_refresh, 1, r->parent);
    parent_step = sqlite3_step(parent_refresh);
  }
  status = fsx_db_status(parent_step);
  if (status != FS_OK)
    return status;
  if (!(r->attributes & FSX_NTFS_DIR) &&
      (r->reason & (FSX_USN_OLD | FSX_USN_NEW | FSX_USN_LINK))) {
    sqlite3_stmt *links;
    int step =
        prepare(t, 9, "INSERT OR IGNORE INTO nt_relinks VALUES(?1)", &links);
    if (step == SQLITE_OK) {
      bind_id(links, 1, r->frn);
      step = sqlite3_step(links);
    }
    status = fsx_db_status(step);
    if (status != FS_OK)
      return status;
  }
  sqlite3_stmt *s;
  int rc = prepare(
      t, 4,
      "INSERT INTO nt_changes VALUES(?1,?2) ON CONFLICT(frn)"
      " DO UPDATE SET attributes=nt_changes.attributes|excluded.attributes",
      &s);
  if (rc == SQLITE_OK) {
    bind_id(s, 1, r->frn);
    sqlite3_bind_int64(s, 2, r->attributes);
    rc = sqlite3_step(s);
  }
  return fsx_db_status(rc);
}
static FsStatus temp_tables(Tx *t, bool rebuild) {
  FsStatus status =
      sql(t->e,
          "CREATE TEMP TABLE IF NOT EXISTS nt_scope(frn BLOB PRIMARY KEY,path "
          "TEXT NOT NULL,depth INTEGER NOT NULL);"
          "CREATE TEMP TABLE IF NOT EXISTS nt_processed(frn BLOB PRIMARY KEY);"
          "CREATE TEMP TABLE IF NOT EXISTS nt_dirty(frn BLOB PRIMARY KEY);"
          "CREATE TEMP TABLE IF NOT EXISTS nt_relinks(frn BLOB PRIMARY KEY);"
          "CREATE TEMP TABLE IF NOT EXISTS nt_refresh(frn BLOB PRIMARY KEY);"
          "CREATE TEMP TABLE IF NOT EXISTS nt_changes(frn BLOB PRIMARY "
          "KEY,attributes INTEGER);"
          "CREATE TEMP TABLE IF NOT EXISTS nt_old_keys(parent BLOB,raw "
          "BLOB,entry_key INTEGER,PRIMARY KEY(parent,raw));"
          "CREATE TEMP TABLE IF NOT EXISTS nt_projection(parent BLOB,raw "
          "BLOB,entry_key INTEGER,name TEXT,path TEXT,"
          "name_fold TEXT,path_fold TEXT,object_key INTEGER,PRIMARY "
          "KEY(parent,raw));"
          "DELETE FROM nt_scope;DELETE FROM nt_processed;DELETE FROM "
          "nt_dirty;DELETE FROM nt_changes;DELETE FROM nt_refresh;DELETE FROM "
          "nt_relinks;"
          "DELETE FROM nt_old_keys;DELETE FROM nt_projection;"
          "CREATE UNIQUE INDEX IF NOT EXISTS nt_projection_key ON "
          "nt_projection(entry_key);");
  if (status != FS_OK)
    return status;
  char command[384];
  snprintf(command, sizeof(command),
           "INSERT INTO nt_old_keys SELECT parent,raw,entry_key FROM "
           "fsx_ntfs_names WHERE source_id=%u;"
           "%s",
           t->source, "");
  status = sql(t->e, command);
  if (status == FS_OK && rebuild) {
    snprintf(command, sizeof(command),
             "DELETE FROM fsx_ntfs_names WHERE source_id=%u;"
             "DELETE FROM fsx_ntfs_dirs WHERE source_id=%u;",
             t->source, t->source);
    status = sql(t->e, command);
  }
  return status;
}
/* For a scope with S directories and depth H in a volume graph of V nodes,
 * drive each level from scope parents and indexed (source,parent) lookups:
 * With C child records examined, O(H*S + S*log(V) + C), excluding path bytes.
 * Avoid a volume-wide range scan at every level, and rebuild only when
 * discovered topology changes. */
static FsStatus make_scope(Tx *t) {
  FsStatus status = sql(t->e, "DELETE FROM nt_scope");
  if (status != FS_OK)
    return status;
  sqlite3_stmt *s = NULL;
  int rc = sqlite3_prepare_v2(t->e->db, "INSERT INTO nt_scope VALUES(?1,?2,0)",
                              -1, &s, NULL);
  if (rc == SQLITE_OK) {
    bind_id(s, 1, t->n->identity.root_frn);
    sqlite3_bind_text(s, 2, t->n->root, -1, SQLITE_STATIC);
    rc = sqlite3_step(s);
  }
  sqlite3_finalize(s);
  if (rc != SQLITE_DONE)
    return fsx_db_status(rc);
  for (uint32_t depth = 0; depth <= t->e->config.max_scan_depth; depth++) {
    rc = sqlite3_prepare_v2(
        t->e->db,
        "SELECT count(*),coalesce(sum(d.name IS "
        "NULL),0),coalesce(max(length(CAST(p.path AS BLOB))"
        "+length(CAST(d.name AS BLOB))+1),0) FROM "
        "nt_scope p CROSS JOIN fsx_ntfs_dirs d INDEXED BY fsx_ntfs_dirs_parent"
        " WHERE d.parent=p.frn AND d.source_id=?1 AND p.depth=?2 AND "
        "d.frn!=?3 AND (d.attributes&1024)=0",
        -1, &s, NULL);
    if (rc == SQLITE_OK) {
      sqlite3_bind_int64(s, 1, t->source);
      sqlite3_bind_int64(s, 2, depth);
      bind_id(s, 3, t->n->identity.root_frn);
      rc = sqlite3_step(s);
    }
    if (rc != SQLITE_ROW) {
      sqlite3_finalize(s);
      return fsx_db_status(rc);
    }
    int64_t count = sqlite3_column_int64(s, 0),
            unsupported = sqlite3_column_int64(s, 1);
    int64_t length = sqlite3_column_int64(s, 2);
    sqlite3_finalize(s);
    s = NULL;
    if (!count)
      break;
    if (unsupported)
      return FS_UNSUPPORTED;
    if (depth + 1 >= t->e->config.max_scan_depth || length > 65535)
      return FS_LIMIT;
    rc = sqlite3_prepare_v2(
        t->e->db,
        "INSERT INTO nt_scope SELECT d.frn,p.path||CASE WHEN "
        "substr(p.path,-1)='/' THEN '' ELSE '/' END||d.name,?2+1"
        " FROM nt_scope p CROSS JOIN fsx_ntfs_dirs d INDEXED BY "
        "fsx_ntfs_dirs_parent WHERE "
        "d.parent=p.frn AND d.source_id=?1 AND p.depth=?2"
        " AND d.frn!=?3 AND (d.attributes&1024)=0",
        -1, &s, NULL);
    if (rc == SQLITE_OK) {
      sqlite3_bind_int64(s, 1, t->source);
      sqlite3_bind_int64(s, 2, depth);
      bind_id(s, 3, t->n->identity.root_frn);
      rc = sqlite3_step(s);
    }
    sqlite3_finalize(s);
    s = NULL;
    if (rc != SQLITE_DONE)
      return fsx_db_status(rc);
    int64_t total;
    status = number(t->e, "SELECT count(*) FROM nt_scope", &total);
    if (status != FS_OK)
      return status;
    if ((uint64_t)total > (uint64_t)t->e->config.max_entries + 1)
      return FS_LIMIT;
  }
  return FS_OK;
}
typedef struct Lookup {
  Tx *tx;
  uint64_t wanted;
  bool found;
} Lookup;
static FsStatus found_dir(void *context, const FsNtfsRecord *r) {
  Lookup *lookup = context;
  if (r->frn != lookup->wanted || !(r->attributes & FSX_NTFS_DIR))
    return FS_OK;
  lookup->found = true;
  FsStatus status = dirty_parent(lookup->tx, r->parent);
  return status == FS_OK ? directory(lookup->tx, r) : status;
}
static FsStatus changes(Tx *t) {
  sqlite3_stmt *rows = NULL;
  int rc = sqlite3_prepare_v2(t->e->db, "SELECT frn,attributes FROM nt_changes",
                              -1, &rows, NULL);
  FsStatus status = fsx_db_status(rc);
  while (status == FS_OK && (rc = sqlite3_step(rows)) == SQLITE_ROW) {
    uint64_t frn;
    if (!id_column(rows, 0, &frn)) {
      status = FS_CORRUPT;
      break;
    }
    uint32_t attributes = (uint32_t)sqlite3_column_int64(rows, 1);
    if (attributes & FSX_NTFS_DIR) {
      size_t bytes = 0;
      status =
          t->n->ops->mft(t->n->provider, frn & UINT64_C(0x0000ffffffffffff),
                         t->n->buffer, 4096, &bytes);
      Lookup lookup = {t, frn, false};
      if (status == FS_OK) {
        uint64_t next;
        uint32_t count;
        status = fsx_ntfs_records(
            t->n->buffer, bytes, false, frn & UINT64_C(0x0000ffffffffffff), 0,
            UINT32_MAX, found_dir, &lookup, &next, &count);
      } else if (status == FS_END)
        status = FS_OK;
      if (status == FS_OK && !lookup.found) {
        if (frn == t->n->identity.root_frn) {
          status = FS_STALE;
          break;
        }
        sqlite3_stmt *remove = NULL;
        int step = sqlite3_prepare_v2(
            t->e->db, "DELETE FROM fsx_ntfs_dirs WHERE source_id=?1 AND frn=?2",
            -1, &remove, NULL);
        if (step == SQLITE_OK) {
          sqlite3_bind_int64(remove, 1, t->source);
          bind_id(remove, 2, frn);
          step = sqlite3_step(remove);
        }
        if (step == SQLITE_DONE && sqlite3_changes(t->e->db))
          t->topology_changed = t->structural = true;
        sqlite3_finalize(remove);
        status = fsx_db_status(step);
      }
    }
  }
  if (status == FS_OK && rc != SQLITE_DONE)
    status = fsx_db_status(rc);
  sqlite3_finalize(rows);
  return status;
}

static FsStatus link_parent(void *context, uint64_t parent) {
  Tx *t = context;
  if (atomic_load(&t->e->stopping))
    return FS_CANCELLED;
  return dirty_parent(t, parent);
}
static FsStatus reconcile_links(Tx *t) {
  sqlite3_stmt *rows = NULL;
  int rc = sqlite3_prepare_v2(t->e->db, "SELECT frn FROM nt_relinks", -1, &rows,
                              NULL);
  if (rc != SQLITE_OK)
    return fsx_db_status(rc);
  FsStatus status = FS_OK;
  while ((rc = sqlite3_step(rows)) == SQLITE_ROW) {
    if (atomic_load(&t->e->stopping)) {
      status = FS_CANCELLED;
      break;
    }
    uint64_t frn;
    if (!id_column(rows, 0, &frn)) {
      status = FS_CORRUPT;
      break;
    }
    sqlite3_stmt *saved = NULL;
    int step = sqlite3_prepare_v2(
        t->e->db,
        "INSERT OR IGNORE INTO nt_dirty"
        " SELECT parent FROM fsx_ntfs_names WHERE source_id=?1 AND frn=?2",
        -1, &saved, NULL);
    if (step == SQLITE_OK) {
      sqlite3_bind_int64(saved, 1, t->source);
      bind_id(saved, 2, frn);
      step = sqlite3_step(saved);
    }
    sqlite3_finalize(saved);
    if (step != SQLITE_DONE) {
      status = fsx_db_status(step);
      break;
    }
    status = parent_capacity(t);
    if (status != FS_OK)
      break;
    status = t->n->ops->links
                 ? t->n->ops->links(t->n->provider, frn, link_parent, t)
                 : FS_UNSUPPORTED;
    if (status == FS_UNSUPPORTED || status == FS_DENIED) {
      t->all = true;
      status = FS_OK;
    } else if (status == FS_END)
      status = FS_OK;
    if (status != FS_OK)
      break;
  }
  if (status == FS_OK && rc != SQLITE_DONE)
    status = fsx_db_status(rc);
  sqlite3_finalize(rows);
  return status;
}
static FsStatus reconcile(Tx *t) {
  FsStatus status = make_scope(t);
  t->topology_changed = false;
  if (status != FS_OK)
    return status;
  char command[384];

  if (t->all) {
    status =
        sql(t->e, "INSERT OR IGNORE INTO nt_dirty SELECT frn FROM nt_scope");
    if (status == FS_OK)
      status = parent_capacity(t);
  }
  if (status != FS_OK)
    return status;
  for (;;) {
    sqlite3_stmt *pick = NULL;
    int rc = sqlite3_prepare_v2(
        t->e->db,
        "SELECT s.frn FROM nt_scope s LEFT JOIN nt_processed p ON p.frn=s.frn"
        " WHERE p.frn IS NULL AND (s.frn IN(SELECT frn FROM nt_dirty) OR NOT "
        "EXISTS"
        "(SELECT 1 FROM fsx_ntfs_scopes n WHERE n.source_id=?1 AND "
        "n.frn=s.frn)) LIMIT 1",
        -1, &pick, NULL);
    if (rc == SQLITE_OK) {
      sqlite3_bind_int64(pick, 1, t->source);
      rc = sqlite3_step(pick);
    }
    uint64_t frn = 0;
    if (rc == SQLITE_ROW && !id_column(pick, 0, &frn)) {
      sqlite3_finalize(pick);
      return FS_CORRUPT;
    }
    sqlite3_finalize(pick);
    if (rc == SQLITE_DONE)
      break;
    if (rc != SQLITE_ROW)
      return fsx_db_status(rc);
    t->parent = frn;
    sqlite3_stmt *clear = NULL;
    rc = sqlite3_prepare_v2(
        t->e->db, "DELETE FROM fsx_ntfs_names WHERE source_id=?1 AND parent=?2",
        -1, &clear, NULL);
    if (rc == SQLITE_OK) {
      sqlite3_bind_int64(clear, 1, t->source);
      bind_id(clear, 2, frn);
      rc = sqlite3_step(clear);
    }
    sqlite3_finalize(clear);
    if (rc != SQLITE_DONE)
      return fsx_db_status(rc);
    status = t->n->ops->list(t->n->provider, frn, alias, t);
    if (status == FS_END) {
      if (frn == t->n->identity.root_frn)
        return FS_STALE;
      sqlite3_stmt *remove = NULL;
      rc = sqlite3_prepare_v2(
          t->e->db, "DELETE FROM fsx_ntfs_dirs WHERE source_id=?1 AND frn=?2",
          -1, &remove, NULL);
      if (rc == SQLITE_OK) {
        sqlite3_bind_int64(remove, 1, t->source);
        bind_id(remove, 2, frn);
        rc = sqlite3_step(remove);
      }
      if (rc == SQLITE_DONE && sqlite3_changes(t->e->db))
        t->topology_changed = t->structural = true;
      sqlite3_finalize(remove);
      status = fsx_db_status(rc);
    } else if (status == FS_OK) {
      sqlite3_stmt *remove = NULL;
      rc = sqlite3_prepare_v2(t->e->db,
                              "DELETE FROM fsx_ntfs_names WHERE source_id=?1 "
                              "AND parent=?2 AND seen!=?3",
                              -1, &remove, NULL);
      if (rc == SQLITE_OK) {
        sqlite3_bind_int64(remove, 1, t->source);
        bind_id(remove, 2, frn);
        sqlite3_bind_int64(remove, 3, (sqlite3_int64)t->revision);
        rc = sqlite3_step(remove);
      }
      sqlite3_finalize(remove);
      status = fsx_db_status(rc);
    }
    if (status != FS_OK)
      return status;
    t->directories++;
    sqlite3_stmt *mark = NULL;
    rc = sqlite3_prepare_v2(t->e->db, "INSERT INTO nt_processed VALUES(?1)", -1,
                            &mark, NULL);
    if (rc == SQLITE_OK) {
      bind_id(mark, 1, frn);
      rc = sqlite3_step(mark);
    }
    sqlite3_finalize(mark);
    if (rc != SQLITE_DONE)
      return fsx_db_status(rc);
    if (t->topology_changed) {
      status = make_scope(t);
      t->topology_changed = false;
      if (status != FS_OK)
        return status;
    }
  }
  snprintf(command, sizeof(command),
           "DELETE FROM fsx_ntfs_names WHERE source_id=%u AND parent NOT "
           "IN(SELECT frn FROM nt_scope);",
           t->source);
  return sql(t->e, command);
}

/* Directory enumeration metadata is cached duplicate information on NTFS.
 * Refresh observed objects, changed objects and affected parent directories by
 * full identity after namespace reconciliation. Each identity is read once. */
static FsStatus refresh_metadata(Tx *t) {
  FsStatus status =
      sql(t->e, "INSERT OR IGNORE INTO nt_refresh SELECT frn FROM nt_changes;"
                "INSERT OR IGNORE INTO nt_refresh SELECT frn FROM nt_dirty;");
  if (status != FS_OK)
    return status;
  char parents[256];
  snprintf(parents, sizeof(parents),
           "INSERT OR IGNORE INTO nt_refresh SELECT n.parent"
           " FROM fsx_ntfs_names n JOIN nt_changes c ON n.frn=c.frn WHERE "
           "n.source_id=%u",
           t->source);
  status = sql(t->e, parents);
  if (status != FS_OK)
    return status;
  sqlite3_stmt *rows = NULL;
  int rc = sqlite3_prepare_v2(
      t->e->db,
      "SELECT r.frn FROM nt_refresh r WHERE EXISTS"
      "(SELECT 1 FROM fsx_ntfs_names n WHERE n.source_id=?1 AND n.frn=r.frn)",
      -1, &rows, NULL);
  if (rc == SQLITE_OK)
    sqlite3_bind_int64(rows, 1, t->source);
  else
    return fsx_db_status(rc);
  while ((rc = sqlite3_step(rows)) == SQLITE_ROW) {
    if (atomic_load(&t->e->stopping)) {
      status = FS_CANCELLED;
      break;
    }
    uint64_t frn;
    if (!id_column(rows, 0, &frn)) {
      status = FS_CORRUPT;
      break;
    }
    FsObserved observed = {0};
    status = t->n->ops->stat(t->n->provider, frn, &observed);
    sqlite3_stmt *change = NULL;
    int step;
    if (status == FS_END) {
      step = sqlite3_prepare_v2(
          t->e->db, "DELETE FROM fsx_ntfs_names WHERE source_id=?1 AND frn=?2",
          -1, &change, NULL);
      if (step == SQLITE_OK) {
        sqlite3_bind_int64(change, 1, t->source);
        bind_id(change, 2, frn);
        step = sqlite3_step(change);
      }
      if (step == SQLITE_DONE && sqlite3_changes(t->e->db))
        t->structural = true;
      sqlite3_finalize(change);
      change = NULL;
      if (step != SQLITE_DONE) {
        status = fsx_db_status(step);
        break;
      }
      step = sqlite3_prepare_v2(
          t->e->db, "DELETE FROM fsx_ntfs_dirs WHERE source_id=?1 AND frn=?2",
          -1, &change, NULL);
      if (step == SQLITE_OK) {
        sqlite3_bind_int64(change, 1, t->source);
        bind_id(change, 2, frn);
        step = sqlite3_step(change);
      }
      if (step == SQLITE_DONE && sqlite3_changes(t->e->db))
        t->topology_changed = t->structural = true;
      sqlite3_finalize(change);
      status = fsx_db_status(step);
    } else if (status == FS_OK) {
      if (observed.file_lo != frn || observed.file_hi ||
          observed.flags & ~15u) {
        status = FS_CORRUPT;
        break;
      }
      sqlite3_int64 key;
      status = object(t, &observed, &key);
      if (status == FS_OK) {
        step = sqlite3_prepare_v2(
            t->e->db,
            "UPDATE fsx_ntfs_dirs SET attributes=?3"
            " WHERE source_id=?1 AND frn=?2 AND (attributes&1040)!=?3",
            -1, &change, NULL);
        if (step == SQLITE_OK) {
          sqlite3_bind_int64(change, 1, t->source);
          bind_id(change, 2, frn);
          sqlite3_bind_int(
              change, 3,
              (observed.flags & FS_ENTRY_DIRECTORY ? FSX_NTFS_DIR : 0) |
                  (observed.flags & FS_ENTRY_LINK ? FSX_NTFS_REPARSE : 0));
          step = sqlite3_step(change);
        }
        if (step == SQLITE_DONE && sqlite3_changes(t->e->db))
          t->topology_changed = t->structural = true;
        sqlite3_finalize(change);
        status = fsx_db_status(step);
      }
    }
    if (status != FS_OK)
      break;
  }
  sqlite3_finalize(rows);
  if (status == FS_OK && rc != SQLITE_DONE)
    status = fsx_db_status(rc);
  if (status == FS_OK && t->topology_changed) {
    status = make_scope(t);
    t->topology_changed = false;
    if (status == FS_OK) {
      char command[160];
      snprintf(command, sizeof(command),
               "DELETE FROM fsx_ntfs_names WHERE source_id=%u"
               " AND parent NOT IN(SELECT frn FROM nt_scope)",
               t->source);
      status = sql(t->e, command);
    }
  }
  return status;
}

/* Projection is staged before changing unique paths. Entry identity follows the
 * parent/raw-name edge, so moving an ancestor keeps descendant entry keys. */
static FsStatus project(Tx *t) {
  sqlite3_stmt *rows = NULL;
  int rc = sqlite3_prepare_v2(
      t->e->db,
      "SELECT n.parent,n.raw,n.entry_key,n.name,s.path,n.object_key FROM "
      "fsx_ntfs_names n"
      " JOIN nt_scope s ON s.frn=n.parent WHERE n.source_id=?1",
      -1, &rows, NULL);
  if (rc != SQLITE_OK)
    return fsx_db_status(rc);
  sqlite3_bind_int64(rows, 1, t->source);
  FsStatus status = FS_OK;
  uint64_t count = 0;
  while ((rc = sqlite3_step(rows)) == SQLITE_ROW) {
    if (atomic_load(&t->e->stopping)) {
      status = FS_CANCELLED;
      break;
    }
    if (++count > t->e->config.max_entries) {
      status = FS_LIMIT;
      break;
    }
    const char *name = (const char *)sqlite3_column_text(rows, 3);
    const char *parent = (const char *)sqlite3_column_text(rows, 4);
    if (!name || !parent) {
      status = FS_CORRUPT;
      break;
    }
    size_t a = strlen(parent), b = strlen(name);
    bool slash = a && parent[a - 1] != '/';
    if (!a || a + b + slash > 65535) {
      status = FS_LIMIT;
      break;
    }
    char *path = fsx_alloc(t->e, a + b + slash + 1), *nf = NULL, *pf = NULL;
    if (!path) {
      status = FS_NOMEM;
      break;
    }
    memcpy(path, parent, a);
    if (slash)
      path[a++] = '/';
    memcpy(path + a, name, b + 1);
    status = fsx_fold(t->e, name, &nf);
    if (status == FS_OK)
      status = fsx_fold(t->e, path, &pf);
    sqlite3_stmt *put = NULL;
    if (status == FS_OK) {
      int step = prepare(t, 5,
                         "INSERT INTO nt_projection VALUES(?1,?2,"
                         "coalesce(?3,(SELECT entry_key FROM entries e WHERE "
                         "source_id=?9 AND path=?5"
                         " AND NOT EXISTS(SELECT 1 FROM fsx_ntfs_names x WHERE "
                         "x.source_id=?9 AND x.entry_key=e.entry_key))),"
                         "?4,?5,?6,?7,?8)",
                         &put);
      if (step == SQLITE_OK) {
        sqlite3_bind_value(put, 1, sqlite3_column_value(rows, 0));
        sqlite3_bind_value(put, 2, sqlite3_column_value(rows, 1));
        sqlite3_bind_value(put, 3, sqlite3_column_value(rows, 2));
        sqlite3_bind_text(put, 4, name, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(put, 5, path, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(put, 6, nf, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(put, 7, pf, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(put, 8, sqlite3_column_int64(rows, 5));
        sqlite3_bind_int64(put, 9, t->source);
        step = sqlite3_step(put);
      }
      status = fsx_db_status(step);
    }
    fsx_free(path);
    fsx_free(nf);
    fsx_free(pf);
    if (status != FS_OK)
      break;
  }
  sqlite3_finalize(rows);
  if (status != FS_OK)
    return status;
  if (rc != SQLITE_DONE)
    return fsx_db_status(rc);
  int64_t bad;
  char command[512];
  /* Each projection edge belongs to this source. Validate through primary-key
   * lookups, rather than searching the alias table once for every entry. */
  snprintf(command, sizeof(command),
           "SELECT count(*) FROM nt_projection p"
           " LEFT JOIN entries e ON p.entry_key=e.entry_key"
           " LEFT JOIN objects o ON p.object_key=o.object_key"
           " WHERE o.object_key IS NULL OR o.source_id!=%u OR (p.entry_key IS "
           "NOT NULL"
           " AND (e.entry_key IS NULL OR e.source_id!=%u))",
           t->source, t->source);
  status = number(t->e, command, &bad);
  if (status != FS_OK || bad)
    return status == FS_OK ? FS_CORRUPT : status;
  snprintf(command, sizeof(command),
           "DELETE FROM entries WHERE source_id=%u AND entry_key NOT IN"
           "(SELECT entry_key FROM nt_projection WHERE entry_key IS NOT NULL);"
           "UPDATE entries SET path=char(0)||hex(entry_key) WHERE source_id=%u "
           "AND EXISTS"
           "(SELECT 1 FROM nt_projection p WHERE p.entry_key=entries.entry_key "
           "AND p.path!=entries.path);",
           t->source, t->source);
  status = sql(t->e, command);
  if (status != FS_OK)
    return status;
  rc = sqlite3_prepare_v2(
      t->e->db,
      "SELECT "
      "p.parent,p.raw,p.entry_key,p.name,p.path,p.name_fold,p.path_fold,p."
      "object_key "
      "FROM nt_projection p LEFT JOIN entries e ON p.entry_key=e.entry_key "
      "WHERE e.entry_key IS NULL"
      " OR e.object_key!=p.object_key OR e.name!=p.name OR e.path!=p.path"
      " OR e.name_fold!=p.name_fold OR e.path_fold!=p.path_fold OR "
      "e.raw_name!=p.raw",
      -1, &rows, NULL);
  if (rc != SQLITE_OK)
    return fsx_db_status(rc);
  while ((rc = sqlite3_step(rows)) == SQLITE_ROW) {
    sqlite3_stmt *put;
    int step = prepare(t, 6,
                       "INSERT INTO "
                       "entries(entry_key,source_id,object_key,name,path,name_"
                       "fold,path_fold,raw_name,seen)"
                       " VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9) ON "
                       "CONFLICT(entry_key) DO UPDATE SET"
                       " object_key=excluded.object_key,name=excluded.name,"
                       "path=excluded.path,name_fold=excluded.name_fold,"
                       "path_fold=excluded.path_fold,raw_name=excluded.raw_"
                       "name,seen=excluded.seen RETURNING entry_key",
                       &put);
    sqlite3_int64 key = 0;
    if (step == SQLITE_OK) {
      sqlite3_bind_value(put, 1, sqlite3_column_value(rows, 2));
      sqlite3_bind_int64(put, 2, t->source);
      sqlite3_bind_value(put, 3, sqlite3_column_value(rows, 7));
      for (int i = 4; i <= 7; i++)
        sqlite3_bind_value(put, i, sqlite3_column_value(rows, i - 1));
      sqlite3_bind_value(put, 8, sqlite3_column_value(rows, 1));
      sqlite3_bind_int64(put, 9, (sqlite3_int64)t->revision);
      step = sqlite3_step(put);
      if (step == SQLITE_ROW) {
        key = sqlite3_column_int64(put, 0);
        step = sqlite3_step(put);
      }
    }
    if (step != SQLITE_DONE || key <= 0) {
      status = step == SQLITE_DONE ? FS_CORRUPT : fsx_db_status(step);
      break;
    }
    step = prepare(t, 7,
                   "UPDATE fsx_ntfs_names SET entry_key=?1 WHERE source_id=?2 "
                   "AND parent=?3 AND raw=?4",
                   &put);
    if (step == SQLITE_OK) {
      sqlite3_bind_int64(put, 1, key);
      sqlite3_bind_int64(put, 2, t->source);
      sqlite3_bind_value(put, 3, sqlite3_column_value(rows, 0));
      sqlite3_bind_value(put, 4, sqlite3_column_value(rows, 1));
      step = sqlite3_step(put);
    }
    if (step != SQLITE_DONE) {
      status = fsx_db_status(step);
      break;
    }
  }
  sqlite3_finalize(rows);
  if (status == FS_OK && rc != SQLITE_DONE)
    status = fsx_db_status(rc);
  if (status == FS_OK) {
    snprintf(command, sizeof(command),
             "UPDATE fsx_ntfs_names SET entry_key="
             "(SELECT p.entry_key FROM nt_projection p WHERE "
             "p.parent=fsx_ntfs_names.parent"
             " AND p.raw=fsx_ntfs_names.raw) WHERE source_id=%u AND entry_key "
             "IS NULL;",
             t->source);
    status = sql(t->e, command);
  }
  if (status == FS_OK)
    status = sql(t->e, "DELETE FROM objects WHERE NOT EXISTS"
                       "(SELECT 1 FROM entries e WHERE "
                       "e.object_key=objects.object_key) AND NOT EXISTS"
                       "(SELECT 1 FROM fsx_ntfs_names n WHERE "
                       "n.object_key=objects.object_key)");
  return status;
}

static FsStatus patch_metadata(Tx *t, FsSnapshot **out) {
  sqlite3_stmt *rows = NULL;
  int rc = sqlite3_prepare_v2(
      t->e->db,
      "SELECT "
      "e.entry_key,n.source_id,o.file_id,e.name,e.path,e.name_fold,e.path_fold,"
      "o.size,o.mtime,o.flags,n.name,"
      "p.path||CASE WHEN substr(p.path,-1)='/' THEN '' ELSE '/' END||n.name,"
      "e.source_id=n.source_id AND o.source_id=n.source_id AND "
      "e.object_key=n.object_key"
      " FROM nt_refresh r CROSS JOIN fsx_ntfs_names n INDEXED BY "
      "fsx_ntfs_names_file"
      " LEFT JOIN entries e ON e.entry_key=n.entry_key"
      " LEFT JOIN objects o ON o.object_key=n.object_key"
      " LEFT JOIN nt_scope p ON p.frn=n.parent"
      " WHERE n.source_id=?1 AND n.frn=r.frn ORDER BY e.entry_key",
      -1, &rows, NULL);
  if (rc != SQLITE_OK)
    return fsx_db_status(rc);
  sqlite3_bind_int64(rows, 1, t->source);
  FsStatus status = fsx_snapshot_patch(t->e, t->revision, rows, out);
  sqlite3_finalize(rows);
  return status;
}
static FsStatus save(Tx *t, const FsNtfsIdentity *id, int64_t cursor) {
  sqlite3_stmt *s = NULL;
  int rc = sqlite3_prepare_v2(
      t->e->db,
      "INSERT INTO fsx_ntfs_sources VALUES(?1,1,?2,?3,?4,?5,?6,?7,1)"
      " ON CONFLICT(source_id) DO UPDATE SET "
      "profile=1,volume_guid=excluded.volume_guid,"
      "serial=excluded.serial,root=excluded.root,volume_root=excluded.volume_"
      "root,"
      "journal=excluded.journal,next_usn=excluded.next_usn,valid=1",
      -1, &s, NULL);
  if (rc == SQLITE_OK) {
    sqlite3_bind_int64(s, 1, t->source);
    sqlite3_bind_text(s, 2, id->volume_guid, -1, SQLITE_STATIC);
    bind_id(s, 3, id->volume_serial);
    bind_id(s, 4, id->root_frn);
    bind_id(s, 5, id->volume_root_frn);
    bind_id(s, 6, id->journal_id);
    sqlite3_bind_int64(s, 7, cursor);
    rc = sqlite3_step(s);
  }
  sqlite3_finalize(s);
  if (rc != SQLITE_DONE)
    return fsx_db_status(rc);
  char command[512];
  snprintf(command, sizeof(command),
           "UPDATE sources SET complete=1,skipped=0,unsupported=0 WHERE "
           "source_id=%u;"
           "UPDATE fsx_meta SET revision=%llu WHERE id=1;"
           "DELETE FROM fsx_ntfs_scopes WHERE source_id=%u;"
           "INSERT INTO fsx_ntfs_scopes SELECT %u,frn FROM nt_scope;",
           t->source, (unsigned long long)t->revision, t->source, t->source);
  return sql(t->e, command);
}
static FsStatus read_events(Tx *t, int64_t *cursor, int64_t target,
                            bool rebuild) {
  while (*cursor < target) {
    size_t size = 0;
    uint64_t next;
    uint32_t count;
    FsStatus status =
        t->n->ops->read(t->n->provider, *cursor, t->n->identity.journal_id,
                        t->n->buffer, t->n->config.buffer_bytes, &size);
    if (status != FS_OK)
      return status;
    status =
        fsx_ntfs_records(t->n->buffer, size, true, (uint64_t)*cursor, target,
                         rebuild ? UINT32_MAX : t->n->config.max_batch_records,
                         event, t, &next, &count);
    if (status != FS_OK)
      return status;
    if (next <= (uint64_t)*cursor)
      return FS_STALE;
    *cursor = (int64_t)next;
    if (!rebuild)
      break;
  }
  return FS_OK;
}
FsStatus fsx_ntfs_store(FsNtfs *n, bool rebuild) {
  FsEngine *e = n->engine;
  FsStatus status = sql(e, "BEGIN IMMEDIATE");
  if (status != FS_OK)
    return status;
  Tx t = {0};
  t.n = n;
  t.e = e;
  FsSnapshot *snapshot = NULL;
  FsNtfsIdentity previous = n->identity, start = {0}, head = {0};
  Checkpoint saved = {0};
  int64_t revision = 0, cursor = 0;
  status = n->ops->query(n->provider, &start);
  FsNtfsIdentity expected = n->identity;
  expected.journal_id = start.journal_id;
  if (status == FS_OK && !fsx_ntfs_same_identity(&expected, &start))
    status = FS_STALE;
  if (status == FS_OK)
    status = checkpoint(n, &saved);
  if (status == FS_OK) {
    cursor = rebuild ? start.next_usn : saved.cursor;
    if (!rebuild && (!saved.present || !saved.valid ||
                     !fsx_ntfs_same_identity(&saved.identity, &start) ||
                     !covered(&start, cursor)))
      status = FS_STALE;
  }
  if (status == FS_OK)
    status = number(e, "SELECT revision FROM fsx_meta WHERE id=1", &revision);
  if (status == FS_OK && (revision < 0 || revision == INT64_MAX))
    status = FS_LIMIT;
  if (status == FS_OK && !rebuild && cursor == start.next_usn) {
    /* Another admitted database writer may already have advanced the durable
     * source. Never acknowledge its cursor while retaining an older mirror. */
    if (e->current->epoch != (uint64_t)revision)
      status = fsx_snapshot_load(e, &snapshot);
    if (status == FS_OK)
      status = sql(e, "COMMIT");
    if (status != FS_OK) {
      sql(e, "ROLLBACK");
      fsx_snapshot_release(snapshot);
      return status;
    }
    if (snapshot) {
      cmeta_mutex_lock(&e->mutex);
      FsSnapshot *old = e->current;
      e->current = snapshot;
      cmeta_mutex_unlock(&e->mutex);
      fsx_snapshot_release(old);
    }
    n->identity = start;
    n->info.source_id = saved.source;
    n->info.snapshot_epoch = (uint64_t)revision;
    n->info.next_usn = cursor;
    n->info.target_usn = cursor;
    n->info.journal_id = start.journal_id;
    n->info.caught_up = true;
    n->info.needs_rebuild = false;
    n->info.native_error = 0;
    n->info.mft_records = 0;
    n->info.journal_records = 0;
    n->info.directories_reconciled = 0;
    return FS_OK;
  }
  if (status == FS_OK) {
    n->identity = start;
    status = source(&t, &start);
  }
  t.revision = (uint64_t)revision + 1;
  if (status == FS_OK)
    status = temp_tables(&t, rebuild);
  if (status == FS_OK && rebuild) {
    uint64_t from = 0;
    for (;;) {
      if (atomic_load(&e->stopping)) {
        status = FS_CANCELLED;
        break;
      }
      size_t size = 0;
      status = n->ops->mft(n->provider, from, n->buffer, n->config.buffer_bytes,
                           &size);
      if (status == FS_END) {
        status = FS_OK;
        break;
      }
      if (status != FS_OK)
        break;
      uint64_t next;
      uint32_t count;
      status = fsx_ntfs_records(n->buffer, size, false, from, 0, UINT32_MAX,
                                inventory, &t, &next, &count);
      if (status != FS_OK)
        break;
      if (next <= from) {
        status = FS_CORRUPT;
        break;
      }
      from = next;
    }
    if (status == FS_OK) {
      t.all = true;
      status = reconcile(&t);
    }
  }
  head = start;
  if (status == FS_OK && rebuild)
    status = n->ops->query(n->provider, &head);
  if (status == FS_OK &&
      (!fsx_ntfs_same_identity(&start, &head) || !covered(&head, cursor)))
    status = FS_STALE;
  if (status == FS_OK)
    status = read_events(&t, &cursor, head.next_usn, rebuild);
  if (status == FS_OK)
    status = changes(&t);
  if (status == FS_OK)
    status = reconcile_links(&t);
  if (status == FS_OK) {
    t.enumerated = 0;
    status = sql(e, "DELETE FROM nt_processed");
  }
  if (status == FS_OK && (!rebuild || t.journal_records))
    status = reconcile(&t);
  if (status == FS_OK)
    status = refresh_metadata(&t);
  int64_t dirs = 0;
  if (status == FS_OK) {
    char command[96];
    snprintf(command, sizeof(command),
             "SELECT count(*) FROM fsx_ntfs_dirs WHERE source_id=%u", t.source);
    status = number(e, command, &dirs);
  }
  if (status == FS_OK && (uint64_t)dirs > n->config.max_mft_records)
    status = FS_LIMIT;
  bool metadata_only = !rebuild && e->current->epoch == (uint64_t)revision &&
                       !e->current->incomplete && !e->current->unsupported &&
                       !t.structural && !t.directories;
  if (status == FS_OK && !metadata_only)
    status = project(&t);
  FsNtfsIdentity final;
  if (status == FS_OK)
    status = n->ops->query(n->provider, &final);
  if (status == FS_OK &&
      (!fsx_ntfs_same_identity(&head, &final) || !covered(&final, cursor)))
    status = FS_STALE;
  if (status == FS_OK)
    status = save(&t, &head, cursor);
  if (status == FS_OK)
    status = metadata_only ? patch_metadata(&t, &snapshot)
                           : fsx_snapshot_load(e, &snapshot);
  finish(&t);
  if (status == FS_OK)
    status = sql(e, "COMMIT");
  if (status != FS_OK) {
    sql(e, "ROLLBACK");
    fsx_snapshot_release(snapshot);
    n->identity = previous;
    return status;
  }
  cmeta_mutex_lock(&e->mutex);
  FsSnapshot *old = e->current;
  e->current = snapshot;
  cmeta_mutex_unlock(&e->mutex);
  fsx_snapshot_release(old);
  n->info.source_id = t.source;
  n->info.next_usn = cursor;
  n->info.target_usn = head.next_usn;
  n->info.snapshot_epoch = t.revision;
  n->info.mft_records = t.mft_records;
  n->info.journal_records = t.journal_records;
  n->info.directories_reconciled = t.directories;
  n->info.journal_id = head.journal_id;
  n->info.native_error = 0;
  n->info.caught_up = cursor == head.next_usn;
  n->info.needs_rebuild = false;
  return FS_OK;
}

#include "raw.h"

enum { RAW_RUNS = 4096, RAW_LIST_BYTES = 1048576, RAW_LIST_ENTRIES = 4096 };
typedef struct Run {
  uint64_t vcn, lcn, count;
} Run;
typedef struct Stream {
  Run runs[RAW_RUNS];
  uint32_t count;
  uint64_t bytes, initialized;
  const unsigned char *resident;
  bool present;
} Stream;
typedef struct Attribute {
  const unsigned char *p, *value;
  uint32_t type, bytes, value_bytes;
  uint16_t instance;
  bool named, nonresident;
} Attribute;
typedef struct Raw {
  FsEngine *engine;
  const FsNtfsConfig *config;
  const FsNtfsRawGeometry *g;
  const FsNtfsRawIO *io;
  void *context;
  FsNtfsRawStats *stats;
  Stream data, bitmap, list;
  unsigned char *chunk, *spare, *bounce, *record, *extension, *list_bytes,
      *bits;
  void *chunk_owner, *spare_owner, *bounce_owner;
  bool pending;
  size_t pending_bytes;
  size_t chunk_bytes;
  uint64_t base_frn;
  unsigned char name[510], bitmap_value[65536];
  uint16_t name_bytes;
  uint64_t parent;
  uint32_t attributes;
  unsigned name_rank;
  bool standard, profile;
} Raw;
static uint16_t u16(const unsigned char *p) {
  return (uint16_t)(p[0] | p[1] << 8);
}
static uint32_t u32(const unsigned char *p) {
  return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
         (uint32_t)p[3] << 24;
}
static bool power2(uint32_t n) { return n && !(n & (n - 1)); }
static FsStatus stop(Raw *r) {
  return atomic_load(&r->engine->stopping) ? FS_CANCELLED : FS_OK;
}
/* Validate all trailers before changing any byte. NTFS USA stride is 512,
 * including on media whose logical/physical sector is larger. */
FsStatus fsx_ntfs_raw_fixup(void *buffer, size_t bytes) {
  if (!buffer || bytes < 512 || bytes > 65536 || bytes % 512)
    return FS_CORRUPT;
  unsigned char *p = buffer;
  if (memcmp(p, "FILE", 4))
    return FS_CORRUPT;
  uint32_t at = u16(p + 4), count = u16(p + 6);
  if (count != bytes / 512 + 1 || at < 48 || at % 2 || at + count * 2 > 510 ||
      at + count * 2 > bytes)
    return FS_CORRUPT;
  uint16_t sequence = u16(p + at);
  for (uint32_t i = 1; i < count; i++)
    if (u16(p + i * 512 - 2) != sequence)
      return FS_STALE;
  for (uint32_t i = 1; i < count; i++) {
    p[i * 512 - 2] = p[at + i * 2];
    p[i * 512 - 1] = p[at + i * 2 + 1];
  }
  return FS_OK;
}
static FsStatus header(Raw *r, const unsigned char *p, uint64_t slot,
                       uint64_t *frn, uint32_t *used, size_t *at) {
  uint32_t bytes = r->g->record_bytes;
  if (memcmp(p, "FILE", 4) || u32(p + 28) != bytes || u32(p + 24) > bytes ||
      u32(p + 24) < 56 || u32(p + 44) != (uint32_t)slot || !u16(p + 16))
    return FS_CORRUPT;
  *used = u32(p + 24);
  *at = u16(p + 20);
  uint32_t usa_end = u16(p + 4) + u16(p + 6) * 2u;
  if (*at < 48 || *at % 8 || *at < usa_end || *at > *used - 4)
    return FS_CORRUPT;
  *frn = slot | (uint64_t)u16(p + 16) << 48;
  return FS_OK;
}
/* END is the required attribute terminator, not buffer exhaustion. */
static FsStatus attribute(const unsigned char *p, uint32_t used, size_t *at,
                          Attribute *a) {
  if (*at > used || used - *at < 4)
    return FS_CORRUPT;
  p += *at;
  uint32_t type = u32(p);
  if (type == UINT32_MAX)
    return FS_END;
  if (used - *at < 24)
    return FS_CORRUPT;
  uint32_t bytes = u32(p + 4), form = p[8];
  if (!type || type % 16 || bytes < 24 || bytes % 8 || bytes > used - *at ||
      form > 1 || (form && bytes < 64))
    return FS_CORRUPT;
  uint32_t fixed = form ? 64 : 24, name_at = u16(p + 10),
           name_bytes = p[9] * 2u;
  if (name_bytes && (name_at < fixed || name_at % 2 || name_at > bytes ||
                     name_bytes > bytes - name_at))
    return FS_CORRUPT;
  *a = (Attribute){p,        NULL, type, bytes, 0, u16(p + 14), name_bytes != 0,
                   form != 0};
  if (!form) {
    uint32_t value_at = u16(p + 20), value_bytes = u32(p + 16);
    if (value_at < fixed || value_at > bytes ||
        value_bytes > bytes - value_at ||
        (name_bytes && value_at < name_at + name_bytes))
      return FS_CORRUPT;
    a->value = p + value_at;
    a->value_bytes = value_bytes;
  }
  *at += bytes;
  return FS_OK;
}
static uint64_t little(const unsigned char *p, unsigned n) {
  uint64_t value = 0;
  for (unsigned i = 0; i < n; i++)
    value |= (uint64_t)p[i] << (i * 8);
  return value;
}
static FsStatus stream_attribute(Raw *r, Stream *s, const Attribute *a) {
  if (a->named || u16(a->p + 12))
    return FS_UNSUPPORTED; /* sparse/compressed/encrypted metadata streams */
  if (!a->nonresident) {
    if (s->present)
      return FS_CORRUPT;
    s->present = true;
    s->resident = a->value;
    s->bytes = s->initialized = a->value_bytes;
    return FS_OK;
  }
  uint64_t vcn = fsx_ntfs_get64(a->p + 16), high = fsx_ntfs_get64(a->p + 24);
  uint32_t pairs = u16(a->p + 32);
  if (high < vcn || high >= INT64_MAX || pairs < 64 || pairs >= a->bytes ||
      (a->named && pairs < u16(a->p + 10) + a->p[9] * 2u))
    return FS_CORRUPT;
  if ((!s->present && vcn) || s->resident ||
      (s->count &&
       vcn != s->runs[s->count - 1].vcn + s->runs[s->count - 1].count))
    return FS_CORRUPT;
  if (!s->present) {
    s->bytes = fsx_ntfs_get64(a->p + 48);
    s->initialized = fsx_ntfs_get64(a->p + 56);
    if (s->bytes > INT64_MAX || s->initialized > s->bytes)
      return FS_CORRUPT;
    s->present = true;
  }
  uint64_t lcn = 0, total = r->g->volume_bytes / r->g->cluster_bytes;
  size_t at = pairs;
  for (;;) {
    if (at == a->bytes)
      return FS_CORRUPT;
    unsigned code = a->p[at++];
    if (!code)
      break;
    unsigned length_bytes = code & 15, offset_bytes = code >> 4;
    if (!length_bytes || length_bytes > 8 || offset_bytes > 8 ||
        length_bytes + offset_bytes > a->bytes - at)
      return FS_CORRUPT;
    if (!offset_bytes)
      return FS_UNSUPPORTED;
    uint64_t count = little(a->p + at, length_bytes);
    at += length_bytes;
    uint64_t delta = little(a->p + at, offset_bytes);
    bool negative = (a->p[at + offset_bytes - 1] & 128) != 0;
    at += offset_bytes;
    if (negative) {
      uint64_t mask = UINT64_MAX >> (64 - offset_bytes * 8);
      uint64_t magnitude = (~delta & mask) + 1;
      if (!magnitude || magnitude > lcn)
        return FS_CORRUPT;
      lcn -= magnitude;
    } else {
      if (delta > INT64_MAX - lcn)
        return FS_CORRUPT;
      lcn += delta;
    }
    if (!count || count > high - vcn + 1 || lcn > total || count > total - lcn)
      return FS_CORRUPT;
    if (s->count == RAW_RUNS)
      return FS_LIMIT;
    s->runs[s->count++] = (Run){vcn, lcn, count};
    vcn += count;
    if (vcn > high + 1)
      return FS_CORRUPT;
  }
  return vcn == high + 1 ? FS_OK : FS_CORRUPT;
}
static FsStatus physical(Raw *r, uint64_t offset, void *out, size_t bytes) {
  FsStatus s = stop(r);
  if (s != FS_OK)
    return s;
  if (offset > r->g->volume_bytes || bytes > r->g->volume_bytes - offset ||
      offset % r->g->sector_bytes || bytes % r->g->sector_bytes)
    return FS_CORRUPT;
  uint64_t started = r->profile ? fsx_engine_now_ns(r->engine) : 0;
  s = r->io->read(r->context, offset, out, bytes);
  if (r->profile)
    r->stats->read_ns += fsx_engine_now_ns(r->engine) - started;
  r->stats->read_calls++;
  if (s == FS_OK)
    r->stats->read_bytes += bytes;
  return s;
}
/* Resolve a logical byte interval, including records spanning extent/sector
 * boundaries. Only bounce unaligned pieces; bulk aligned chunks read directly.
 */
static FsStatus stream_read(Raw *r, const Stream *s, uint64_t offset, void *out,
                            size_t bytes) {
  if (!s->present || offset > s->initialized || bytes > s->initialized - offset)
    return FS_CORRUPT;
  if (s->resident) {
    memmove(out, s->resident + (size_t)offset, bytes);
    return FS_OK;
  }
  unsigned char *dest = out;
  uint32_t cluster = r->g->cluster_bytes, sector = r->g->sector_bytes;
  while (bytes) {
    uint64_t vcn = offset / cluster;
    uint32_t lo = 0, hi = s->count;
    while (lo < hi) {
      uint32_t mid = lo + (hi - lo) / 2;
      if (s->runs[mid].vcn + s->runs[mid].count <= vcn)
        lo = mid + 1;
      else
        hi = mid;
    }
    if (lo == s->count || s->runs[lo].vcn > vcn)
      return FS_CORRUPT;
    const Run *run = &s->runs[lo];
    uint64_t inside = offset - run->vcn * cluster;
    uint64_t address = run->lcn * cluster + inside;
    uint64_t available = run->count * cluster - inside;
    size_t take = bytes;
    if (take > available)
      take = (size_t)available;
    if (take > r->chunk_bytes)
      take = r->chunk_bytes;
    FsStatus status;
    if (!(address % sector) && !(take % sector) &&
        !((uintptr_t)dest % sector)) {
      status = physical(r, address, dest, take);
    } else {
      size_t prefix = (size_t)(address % sector);
      if (take > r->chunk_bytes - prefix)
        take = r->chunk_bytes - prefix;
      size_t read_bytes = (prefix + take + sector - 1) / sector * sector;
      status = physical(r, address - prefix, r->bounce, read_bytes);
      if (status == FS_OK)
        memcpy(dest, r->bounce + prefix, take);
    }
    if (status != FS_OK)
      return status;
    offset += take;
    dest += take;
    bytes -= take;
  }
  return FS_OK;
}
static FsStatus kernel_record(Raw *r, uint64_t frn, unsigned char *out) {
  FsStatus status = stop(r);
  if (status != FS_OK)
    return status;
  size_t bytes = 0;
  uint64_t started = r->profile ? fsx_engine_now_ns(r->engine) : 0;
  status = r->io->record(r->context, frn, out, r->g->record_bytes + 16, &bytes);
  if (r->profile)
    r->stats->kernel_ns += fsx_engine_now_ns(r->engine) - started;
  r->stats->kernel_records++;
  if (status != FS_OK)
    return status == FS_END ? FS_STALE : status;
  uint64_t found;
  uint32_t used;
  size_t at;
  if (bytes != r->g->record_bytes)
    return FS_CORRUPT;
  status =
      header(r, out, frn & UINT64_C(0x0000ffffffffffff), &found, &used, &at);
  if (status == FS_OK && (!(u16(out + 22) & 1) || (frn && found != frn)))
    status = FS_STALE;
  return status;
}
static FsStatus consume(Raw *r, const Attribute *a, bool system) {
  if (a->named)
    return FS_OK;
  if (a->type == 0x20)
    return stream_attribute(r, &r->list, a);
  if (system) {
    if (a->type == 0x80)
      return stream_attribute(r, &r->data, a);
    if (a->type == 0xb0) {
      FsStatus status = stream_attribute(r, &r->bitmap, a);
      if (status == FS_OK && r->bitmap.resident) {
        memcpy(r->bitmap_value, r->bitmap.resident, (size_t)r->bitmap.bytes);
        r->bitmap.resident = r->bitmap_value;
      }
      return status;
    }
    return FS_OK;
  }
  if (a->type != 0x10 && a->type != 0x30)
    return FS_OK;
  if (a->nonresident)
    return FS_CORRUPT;
  if (a->type == 0x10) {
    if (a->value_bytes < 36 || r->standard)
      return FS_CORRUPT;
    r->standard = true;
    r->attributes = u32(a->value + 32) | FSX_NTFS_DIR;
    return FS_OK;
  }
  if (a->value_bytes < 68)
    return FS_CORRUPT;
  uint32_t name_bytes = a->value[64] * 2u, space = a->value[65];
  uint64_t parent = fsx_ntfs_get64(a->value);
  if (!name_bytes || name_bytes > a->value_bytes - 66 || !parent || space > 3)
    return FS_CORRUPT;
  if (space == 2) /* DOS alias; an actual directory long name is required. */
    return FS_OK;
  unsigned rank = space == 0 ? 1 : 2;
  if (r->name_rank && (r->parent != parent || r->name_bytes != name_bytes ||
                       memcmp(r->name, a->value + 66, name_bytes)))
    return FS_UNSUPPORTED; /* Multiple directory parents/long names. */
  if (rank >= r->name_rank) {
    r->name_rank = rank;
    r->name_bytes = (uint16_t)name_bytes;
    r->parent = parent;
    memcpy(r->name, a->value + 66, name_bytes);
  }
  return FS_OK;
}
static bool wanted(uint32_t type, bool system) {
  return type == 0x20 ||
         (system ? type == 0x80 || type == 0xb0 : type == 0x10 || type == 0x30);
}
static FsStatus attributes(Raw *r, const unsigned char *base, bool system,
                           uint64_t frn) {
  r->list.count = 0;
  r->list.present = false;
  r->list.resident = NULL;
  r->standard = false;
  r->name_rank = r->name_bytes = 0;
  uint64_t found;
  uint32_t used;
  size_t at;
  FsStatus status =
      header(r, base, frn & UINT64_C(0x0000ffffffffffff), &found, &used, &at);
  if (status != FS_OK || found != frn || fsx_ntfs_get64(base + 32))
    return status != FS_OK ? status : FS_CORRUPT;
  Attribute a;
  while ((status = attribute(base, used, &at, &a)) == FS_OK) {
    if (stop(r) != FS_OK)
      return FS_CANCELLED;
    status = consume(r, &a, system);
    if (status != FS_OK)
      return status;
  }
  if (status != FS_END)
    return status;
  if (!r->list.present)
    return FS_OK;
  if (!r->list.bytes || r->list.bytes > RAW_LIST_BYTES)
    return FS_LIMIT;
  status = stream_read(r, &r->list, 0, r->list_bytes, (size_t)r->list.bytes);
  if (status != FS_OK)
    return status;
  size_t length = (size_t)r->list.bytes;
  unsigned entries = 0;
  for (size_t p = 0; p < length;) {
    if (stop(r) != FS_OK)
      return FS_CANCELLED;
    if (length - p < 26)
      return FS_CORRUPT;
    const unsigned char *item = r->list_bytes + p;
    uint32_t type = u32(item), bytes = u16(item + 4);
    uint32_t name_bytes = item[6] * 2u, name_at = item[7];
    uint64_t lowest = fsx_ntfs_get64(item + 8), ref = fsx_ntfs_get64(item + 16);
    uint16_t instance = u16(item + 24);
    if (!type || type % 16 || bytes < 32 || bytes % 8 || bytes > length - p ||
        (name_bytes &&
         (name_at < 26 || name_at > bytes || name_bytes > bytes - name_at)) ||
        !ref)
      return FS_CORRUPT;
    if (++entries > RAW_LIST_ENTRIES)
      return FS_LIMIT;
    p += bytes;
    if (name_bytes || !wanted(type, system))
      continue;
    if (type == 0x20)
      return FS_CORRUPT; /* The attribute list cannot contain itself. */
    const unsigned char *record = base;
    if (ref != frn) {
      status = kernel_record(r, ref, r->extension);
      if (status != FS_OK)
        return status;
      record = r->extension;
      if (fsx_ntfs_get64(record + 32) != frn)
        return FS_CORRUPT;
    }
    size_t pos;
    uint32_t bound;
    uint64_t actual;
    status = header(r, record, ref & UINT64_C(0x0000ffffffffffff), &actual,
                    &bound, &pos);
    if (status != FS_OK || actual != ref)
      return status != FS_OK ? status : FS_STALE;
    bool matched = false;
    while ((status = attribute(record, bound, &pos, &a)) == FS_OK) {
      if (a.type != type || a.named || a.instance != instance ||
          (a.nonresident ? fsx_ntfs_get64(a.p + 16) : 0) != lowest)
        continue;
      if (matched)
        return FS_CORRUPT;
      matched = true;
      if (ref != frn) {
        status = consume(r, &a, system);
        if (status != FS_OK)
          return status;
      }
    }
    if (status != FS_END || !matched)
      return status != FS_END ? status : FS_CORRUPT;
  }
  return FS_OK;
}
static FsStatus metadata(Raw *r) {
  r->data.count = r->bitmap.count = 0;
  r->data.present = r->bitmap.present = false;
  r->data.resident = r->bitmap.resident = NULL;
  FsStatus status = kernel_record(r, 0, r->record);
  if (status != FS_OK)
    return status;
  uint64_t frn = (uint64_t)u16(r->record + 16) << 48;
  if (r->base_frn && r->base_frn != frn)
    return FS_STALE;
  r->base_frn = frn;
  status = attributes(r, r->record, true, frn);
  if (status != FS_OK)
    return status;
  uint64_t needed = (r->g->mft_bytes / r->g->record_bytes + 7) / 8;
  if (!r->data.present || r->data.resident || !r->data.count ||
      r->data.runs[0].lcn != r->g->mft_lcn ||
      r->data.initialized < r->g->mft_bytes || !r->bitmap.present ||
      r->bitmap.initialized < needed)
    return FS_CORRUPT;
  return FS_OK;
}
static bool prefix_equal(const Run *a, uint32_t na, const Stream *b,
                         uint64_t clusters) {
  uint32_t i = 0, j = 0;
  uint64_t at = 0;
  while (at < clusters) {
    if (i == na || j == b->count || a[i].vcn > at || b->runs[j].vcn > at)
      return false;
    const Run *x = &a[i], *y = &b->runs[j];
    if (x->lcn + at - x->vcn != y->lcn + at - y->vcn)
      return false;
    uint64_t xe = x->vcn + x->count, ye = y->vcn + y->count;
    at = xe < ye ? xe : ye;
    if (at == xe)
      i++;
    if (at == ye)
      j++;
  }
  return true;
}
static bool any_allocated(const unsigned char *bits, uint64_t from,
                          uint64_t count) {
  uint64_t end = from + count;
  while (from < end && from % 8) {
    if (bits[from / 8] & (1u << (from % 8)))
      return true;
    from++;
  }
  while (end - from >= 8) {
    if (bits[from / 8])
      return true;
    from += 8;
  }
  while (from < end) {
    if (bits[from / 8] & (1u << (from % 8)))
      return true;
    from++;
  }
  return false;
}
static FsStatus next_chunk(Raw *r, uint64_t from, uint64_t *offset,
                           size_t *bytes) {
  while (from < r->g->mft_bytes) {
    if (stop(r) != FS_OK)
      return FS_CANCELLED;
    size_t take = r->chunk_bytes;
    if (take > r->g->mft_bytes - from)
      take = (size_t)(r->g->mft_bytes - from);
    if (any_allocated(r->bits, from / r->g->record_bytes,
                      take / r->g->record_bytes)) {
      *offset = from;
      *bytes = take;
      return FS_OK;
    }
    from += take;
  }
  return FS_END;
}
/* Only submit a whole aligned extent interval. Boundary-spanning chunks retain
 * the synchronous stream assembler; no partial async request is guessed. */
static bool contiguous(Raw *r, uint64_t offset, size_t bytes,
                       uint64_t *address) {
  uint32_t cluster = r->g->cluster_bytes, sector = r->g->sector_bytes;
  uint64_t vcn = offset / cluster;
  uint32_t lo = 0, hi = r->data.count;
  while (lo < hi) {
    uint32_t mid = lo + (hi - lo) / 2;
    if (r->data.runs[mid].vcn + r->data.runs[mid].count <= vcn)
      lo = mid + 1;
    else
      hi = mid;
  }
  if (lo == r->data.count || r->data.runs[lo].vcn > vcn)
    return false;
  const Run *run = &r->data.runs[lo];
  uint64_t inside = offset - run->vcn * cluster;
  if (bytes > run->count * cluster - inside)
    return false;
  *address = run->lcn * cluster + inside;
  return !(*address % sector) && !(bytes % sector) &&
         *address <= r->g->volume_bytes &&
         bytes <= r->g->volume_bytes - *address;
}
static FsStatus begin_read(Raw *r, uint64_t address, size_t bytes) {
  uint64_t started = r->profile ? fsx_engine_now_ns(r->engine) : 0;
  FsStatus status = r->io->begin(r->context, address, r->spare, bytes);
  if (r->profile)
    r->stats->read_ns += fsx_engine_now_ns(r->engine) - started;
  r->stats->read_calls++;
  if (status == FS_OK) {
    r->pending = true;
    r->pending_bytes = bytes;
    r->stats->prefetch_reads++;
  }
  return status;
}
static FsStatus finish_read(Raw *r, bool cancel) {
  uint64_t started = r->profile ? fsx_engine_now_ns(r->engine) : 0;
  FsStatus status = r->io->finish(r->context, cancel);
  if (r->profile)
    r->stats->read_ns += fsx_engine_now_ns(r->engine) - started;
  /* finish drains even on cancellation/error, ending the destination borrow. */
  r->pending = false;
  if (status == FS_OK)
    r->stats->read_bytes += r->pending_bytes;
  return status;
}
static void *aligned(FsEngine *e, size_t bytes, uint32_t alignment,
                     void **owner) {
  *owner = fsx_alloc(e, bytes + alignment - 1);
  if (!*owner)
    return NULL;
  uintptr_t p = (uintptr_t)*owner;
  return (void *)((p + alignment - 1) & ~((uintptr_t)alignment - 1));
}
FsStatus fsx_ntfs_raw_inventory(FsEngine *e, const FsNtfsConfig *config,
                                const FsNtfsRawGeometry *g,
                                const FsNtfsRawIO *io, void *context,
                                FsNtfsRecordFn visit, void *target,
                                bool profile, FsNtfsRawStats *stats) {
  if (!e || !config || !g || !io || !io->read || !io->record || !visit ||
      !stats)
    return FS_INVALID;
  memset(stats, 0, sizeof(*stats));
  if (config->raw_flags &
      ~(uint64_t)(FS_NTFS_RAW_PREFETCH | FS_NTFS_RAW_BATCH_DIRS))
    return FS_INVALID;
  bool prefetch = (config->raw_flags & FS_NTFS_RAW_PREFETCH) != 0;
  if (prefetch && (!io->begin || !io->finish))
    return FS_UNSUPPORTED;
  uint64_t started = profile ? fsx_engine_now_ns(e) : 0;
  if (!power2(g->sector_bytes) || g->sector_bytes < 512 ||
      g->sector_bytes > 4096 || !power2(g->cluster_bytes) ||
      g->cluster_bytes < g->sector_bytes || g->cluster_bytes > 2097152 ||
      !power2(g->record_bytes) || g->record_bytes < 512 ||
      g->record_bytes > 65536 || !g->mft_bytes ||
      g->mft_bytes % g->record_bytes || !g->volume_bytes ||
      g->volume_bytes > INT64_MAX || g->volume_bytes % g->sector_bytes ||
      g->mft_lcn >= g->volume_bytes / g->cluster_bytes)
    return FS_UNSUPPORTED;
  stats->slots = g->mft_bytes / g->record_bytes;
  if (!config->max_mft_records || stats->slots > config->max_mft_records)
    return FS_LIMIT;
  size_t chunk = config->buffer_bytes;
  if (chunk < g->record_bytes)
    chunk = g->record_bytes;
  if (chunk < g->sector_bytes)
    chunk = g->sector_bytes;
  chunk = (chunk + g->record_bytes - 1) / g->record_bytes * g->record_bytes;
  chunk = (chunk + g->sector_bytes - 1) / g->sector_bytes * g->sector_bytes;
  if (chunk > 1048576)
    return FS_LIMIT;
  Raw *r = fsx_alloc(e, sizeof(*r));
  if (!r)
    return FS_NOMEM;
  memset(r, 0, sizeof(*r));
  r->engine = e;
  r->profile = profile;
  r->config = config;
  r->g = g;
  r->io = io;
  r->context = context;
  r->stats = stats;
  r->chunk_bytes = chunk;
  r->chunk = aligned(e, chunk, g->sector_bytes, &r->chunk_owner);
  if (prefetch)
    r->spare = aligned(e, chunk, g->sector_bytes, &r->spare_owner);
  r->bounce = aligned(e, chunk, g->sector_bytes, &r->bounce_owner);
  r->record = fsx_alloc(e, g->record_bytes + 16);
  r->extension = fsx_alloc(e, g->record_bytes + 16);
  r->list_bytes = fsx_alloc(e, RAW_LIST_BYTES);
  size_t bitmap_bytes = (size_t)((stats->slots + 7) / 8);
  r->bits = fsx_alloc(e, bitmap_bytes);
  Run *saved = NULL;
  FsStatus status = FS_NOMEM;
  if (!r->chunk || !r->bounce || !r->record || !r->extension ||
      !r->list_bytes || !r->bits || (prefetch && !r->spare))
    goto done;
  status = metadata(r);
  if (status != FS_OK)
    goto done;
  status = stream_read(r, &r->bitmap, 0, r->bits, bitmap_bytes);
  if (status != FS_OK)
    goto done;
  if (!(r->bits[0] & 1)) {
    status = FS_CORRUPT;
    goto done;
  }
  uint32_t saved_count = r->data.count;
  saved = fsx_alloc(e, (size_t)saved_count * sizeof(*saved));
  if (!saved) {
    status = FS_NOMEM;
    goto done;
  }
  memcpy(saved, r->data.runs, (size_t)saved_count * sizeof(*saved));
  uint64_t offset;
  size_t bytes;
  status = next_chunk(r, 0, &offset, &bytes);
  if (status != FS_OK) {
    if (status == FS_END)
      status = FS_CORRUPT;
    goto done;
  }
  for (;;) {
    if (r->pending) {
      status = finish_read(r, false);
      if (status != FS_OK)
        goto done;
      unsigned char *completed = r->spare;
      r->spare = r->chunk;
      r->chunk = completed;
    } else {
      status = stream_read(r, &r->data, offset, r->chunk, bytes);
      if (status != FS_OK)
        goto done;
    }
    uint64_t following = 0, address = 0;
    size_t following_bytes = 0;
    FsStatus next = next_chunk(r, offset + bytes, &following, &following_bytes);
    if (next != FS_OK && next != FS_END) {
      status = next;
      goto done;
    }
    if (prefetch && next == FS_OK &&
        contiguous(r, following, following_bytes, &address)) {
      status = begin_read(r, address, following_bytes);
      if (status != FS_OK)
        goto done;
    }
    for (size_t at = 0; at < bytes; at += g->record_bytes) {
      uint64_t slot = (offset + at) / g->record_bytes;
      if (stop(r) != FS_OK) {
        status = FS_CANCELLED;
        goto done;
      }
      if (!(r->bits[slot / 8] & (1u << (slot % 8))))
        continue;
      unsigned char *p = r->chunk + at;
      status = fsx_ntfs_raw_fixup(p, g->record_bytes);
      if (status != FS_OK)
        goto done;
      uint64_t frn;
      uint32_t used;
      size_t attributes_at;
      status = header(r, p, slot, &frn, &used, &attributes_at);
      if (status != FS_OK)
        goto done;
      if (!(u16(p + 22) & 1) || fsx_ntfs_get64(p + 32))
        continue;
      stats->records++;
      if (!(u16(p + 22) & 2))
        continue;
      status = attributes(r, p, false, frn);
      if (status != FS_OK)
        goto done;
      if (!r->standard || !r->name_rank) {
        status = FS_CORRUPT;
        goto done;
      }
      FsNtfsRecord item = {0};
      item.frn = frn;
      item.parent = r->parent;
      item.attributes = r->attributes;
      item.length = g->record_bytes;
      item.name = r->name;
      item.name_bytes = r->name_bytes;
      uint64_t callback_started = profile ? fsx_engine_now_ns(e) : 0;
      status = visit(target, &item);
      if (profile)
        stats->callback_ns += fsx_engine_now_ns(e) - callback_started;
      stats->directories++;
      if (status != FS_OK)
        goto done;
    }
    if (next == FS_END)
      break;
    offset = following;
    bytes = following_bytes;
  }
  status = metadata(r);
  if (status == FS_OK &&
      !prefix_equal(saved, saved_count, &r->data,
                    (g->mft_bytes + g->cluster_bytes - 1) / g->cluster_bytes))
    status = FS_STALE;
done:
  if (r->pending) {
    FsStatus drained = finish_read(r, true);
    if (status == FS_OK)
      status = drained;
  }
  fsx_free(saved);
  fsx_free(r->bits);
  fsx_free(r->list_bytes);
  fsx_free(r->extension);
  fsx_free(r->record);
  fsx_free(r->bounce_owner);
  fsx_free(r->spare_owner);
  fsx_free(r->chunk_owner);
  fsx_free(r);
  if (profile)
    stats->elapsed_ns = fsx_engine_now_ns(e) - started;
  return status;
}

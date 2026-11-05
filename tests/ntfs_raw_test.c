#include "../src/source/ntfs/raw.h"
#include <tinytest.h>

enum { RECORD = 1024, CLUSTER = 4096, SLOTS = 16 };
static FsEngine *engine;
static unsigned char disk[65536], records[SLOTS][RECORD];
static FsNtfsRawGeometry geometry;
static FsNtfsConfig config;
static unsigned seen, bootstraps;
static bool fail_read, relocate, cancel_visit, bad_extension;
static bool measured_callback;
static FsStatus callback_status, begin_fault, finish_fault;
static struct {
  void *buffer;
  uint64_t offset;
  size_t bytes;
  unsigned seen_before;
} pending;
static unsigned begun, finished, cancelled, callbacks_overlapped;
static bool complete_inline, fault_on_cancel_only;
static uint64_t parents[3], identities[3];
static char names[3][32];
static void put16(unsigned char *p, unsigned value) {
  p[0] = (unsigned char)value;
  p[1] = (unsigned char)(value >> 8);
}
static void put32(unsigned char *p, uint32_t value) {
  for (unsigned i = 0; i < 4; i++)
    p[i] = (unsigned char)(value >> (i * 8));
}
static uint64_t id(unsigned slot) {
  return ((uint64_t)(slot == 0 || slot == 10 ? 1 : 0x8123) << 48) | slot;
}
static unsigned char *begin(unsigned slot, unsigned flags, uint64_t base) {
  unsigned char *p = records[slot];
  memset(p, 0, RECORD);
  memcpy(p, "FILE", 4);
  put16(p + 4, 48);
  put16(p + 6, 3);
  put16(p + 16, (unsigned)(id(slot) >> 48));
  put16(p + 20, 56);
  put16(p + 22, flags);
  put32(p + 28, RECORD);
  fsx_ntfs_put64(p + 32, base);
  put32(p + 44, slot);
  return p;
}
static size_t resident(unsigned char *p, size_t at, unsigned type,
                       unsigned instance, const void *value, size_t bytes) {
  size_t total = (24 + bytes + 7) & ~(size_t)7;
  put32(p + at, type);
  put32(p + at + 4, (uint32_t)total);
  put16(p + at + 14, instance);
  put32(p + at + 16, (uint32_t)bytes);
  put16(p + at + 20, 24);
  memcpy(p + at + 24, value, bytes);
  return at + total;
}
static size_t nonresident(unsigned char *p, size_t at, unsigned type,
                          unsigned instance, uint64_t low, uint64_t high,
                          const unsigned char *pairs, size_t bytes,
                          uint64_t stream_bytes) {
  size_t total = (64 + bytes + 7) & ~(size_t)7;
  put32(p + at, type);
  put32(p + at + 4, (uint32_t)total);
  p[at + 8] = 1;
  put16(p + at + 14, instance);
  fsx_ntfs_put64(p + at + 16, low);
  fsx_ntfs_put64(p + at + 24, high);
  put16(p + at + 32, 64);
  fsx_ntfs_put64(p + at + 40, stream_bytes);
  fsx_ntfs_put64(p + at + 48, stream_bytes);
  fsx_ntfs_put64(p + at + 56, stream_bytes);
  memcpy(p + at + 64, pairs, bytes);
  return at + total;
}
static void finish(unsigned char *p, size_t at) {
  put32(p + at, UINT32_MAX);
  put32(p + 24, (uint32_t)at + 8);
}
static void list_entry(unsigned char *p, unsigned type, unsigned instance,
                       uint64_t ref, uint64_t vcn) {
  memset(p, 0, 32);
  put32(p, type);
  put16(p + 4, 32);
  fsx_ntfs_put64(p + 8, vcn);
  fsx_ntfs_put64(p + 16, ref);
  put16(p + 24, instance);
}
static size_t filename(unsigned char *p, size_t at, unsigned instance,
                       uint64_t parent, const char *name, unsigned space) {
  unsigned char value[128] = {0};
  size_t n = strlen(name);
  fsx_ntfs_put64(value, parent);
  value[64] = (unsigned char)n;
  value[65] = (unsigned char)space;
  for (size_t i = 0; i < n; i++)
    put16(value + 66 + 2 * i, (unsigned char)name[i]);
  return resident(p, at, 0x30, instance, value, 66 + 2 * n);
}
static size_t standard(unsigned char *p, size_t at, unsigned flags) {
  unsigned char value[72] = {0};
  put32(value + 32, flags);
  return resident(p, at, 0x10, 0, value, sizeof(value));
}
static unsigned char *physical_record(unsigned slot) {
  unsigned logical = slot * RECORD;
  unsigned address = logical < CLUSTER * 2
                         ? CLUSTER * 8 + logical
                         : CLUSTER * 2 + logical - CLUSTER * 2;
  return disk + address;
}
static void protect_records(void) {
  memset(disk, 0, sizeof(disk));
  for (unsigned slot = 0; slot < SLOTS; slot++) {
    unsigned char *p = physical_record(slot);
    memcpy(p, records[slot], RECORD);
    if (memcmp(p, "FILE", 4))
      continue;
    put16(p + 48, 0xa55a);
    memcpy(p + 50, p + 510, 2);
    memcpy(p + 52, p + 1022, 2);
    put16(p + 510, 0xa55a);
    put16(p + 1022, 0xa55a);
  }
}
static FsStatus read_volume(void *ctx, uint64_t offset, void *out,
                            size_t bytes) {
  (void)ctx;
  check_equal(offset % geometry.sector_bytes, (uint64_t)0);
  check_equal(bytes % geometry.sector_bytes, (size_t)0);
  check_equal((uintptr_t)out % geometry.sector_bytes, (uintptr_t)0);
  if (fail_read)
    return FS_IO;
  if (offset > sizeof(disk) || bytes > sizeof(disk) - offset)
    return FS_IO;
  memcpy(out, disk + offset, bytes);
  return FS_OK;
}
static FsStatus begin_read(void *ctx, uint64_t offset, void *out,
                           size_t bytes) {
  (void)ctx;
  check_null(pending.buffer);
  if (begin_fault != FS_OK)
    return begin_fault;
  pending.buffer = out;
  pending.offset = offset;
  pending.bytes = bytes;
  pending.seen_before = seen;
  begun++;
  if (complete_inline) {
    FsStatus status = read_volume(NULL, offset, out, bytes);
    if (status != FS_OK)
      pending.buffer = NULL;
    return status;
  }
  return FS_OK;
}
static FsStatus finish_read(void *ctx, bool cancel) {
  (void)ctx;
  check_not_null(pending.buffer);
  void *buffer = pending.buffer;
  uint64_t offset = pending.offset;
  size_t bytes = pending.bytes;
  if (seen > pending.seen_before)
    callbacks_overlapped++;
  pending.buffer = NULL;
  finished++;
  if (cancel)
    cancelled++;
  if (cancel || (finish_fault != FS_OK && !fault_on_cancel_only)) {
    /* A late completion is allowed to write until finish has drained. */
    memset(buffer, 0xcc, bytes);
    return finish_fault != FS_OK ? finish_fault : FS_CANCELLED;
  }
  return complete_inline ? FS_OK : read_volume(NULL, offset, buffer, bytes);
}
static FsStatus get_record(void *ctx, uint64_t frn, void *out, size_t capacity,
                           size_t *bytes) {
  (void)ctx;
  check_true(capacity >= RECORD);
  unsigned slot = (unsigned)(frn & UINT64_C(0x0000ffffffffffff));
  if (slot >= SLOTS || (frn && frn != id(slot)))
    return FS_END;
  if (!slot)
    bootstraps++;
  memcpy(out, records[slot], RECORD);
  *bytes = RECORD;
  if (bad_extension && slot == 8)
    fsx_ntfs_put64((unsigned char *)out + 32, id(7));
  if (relocate && slot == 10 && bootstraps > 1) {
    /* Relocate the second extent, leaving bootstrap geometry unchanged. */
    ((unsigned char *)out)[56 + 64 + 2] = 3;
  }
  return FS_OK;
}
static FsStatus directory(void *ctx, const FsNtfsRecord *entry) {
  (void)ctx;
  if (measured_callback)
    cmeta_sleep_ms(1);
  if (callback_status != FS_OK)
    return callback_status;
  check_true(seen < 3);
  check_true(entry->name_bytes / 2 < sizeof(names[0]));
  check_true((entry->attributes & FSX_NTFS_DIR) != 0);
  identities[seen] = entry->frn;
  parents[seen] = entry->parent;
  for (unsigned i = 0; i < entry->name_bytes / 2; i++)
    names[seen][i] = (char)entry->name[i * 2];
  seen++;
  if (cancel_visit)
    atomic_store(&engine->stopping, true);
  return FS_OK;
}
static const FsNtfsRawIO io = {read_volume, get_record, begin_read,
                               finish_read};
static FsStatus inventory(FsNtfsRawStats *stats) {
  return fsx_ntfs_raw_inventory(engine, &config, &geometry, &io, NULL,
                                directory, NULL, false, stats);
}
static void setup(void) {
  FsEngineConfig ec = fsx_engine_config(":memory:");
  check_equal(fsx_engine_open(&ec, &engine), FS_OK);
  config = fsx_ntfs_config();
  config.buffer_bytes = 4096;
  geometry = (FsNtfsRawGeometry){sizeof(disk), 8,       RECORD * SLOTS,
                                 512,          CLUSTER, RECORD};
  seen = bootstraps = 0;
  pending.buffer = NULL;
  begun = finished = cancelled = callbacks_overlapped = 0;
  begin_fault = finish_fault = FS_OK;
  complete_inline = fault_on_cancel_only = false;
  fail_read = relocate = cancel_visit = bad_extension = false;
  measured_callback = false;
  callback_status = FS_OK;
  memset(records, 0, sizeof(records));
  memset(names, 0, sizeof(names));
  /* Fragmented DATA stream: first extent in the base, second in extension 10.
   */
  unsigned char first[] = {0x11, 2, 8, 0}, second[] = {0x11, 2, 2, 0};
  unsigned char list[64];
  list_entry(list, 0x80, 3, id(0), 0);
  list_entry(list + 32, 0x80, 4, id(10), 2);
  unsigned char bitmap[] = {0xe1, 0x07}; /* 0,5,6,7,8,9,10 */
  unsigned char *p = begin(0, 1, 0);
  size_t at =
      nonresident(p, 56, 0x80, 3, 0, 1, first, sizeof(first), RECORD * SLOTS);
  at = resident(p, at, 0x20, 1, list, sizeof(list));
  at = resident(p, at, 0xb0, 2, bitmap, sizeof(bitmap));
  finish(p, at);
  p = begin(10, 1, id(0));
  at = nonresident(p, 56, 0x80, 4, 2, 3, second, sizeof(second), 0);
  finish(p, at);
  p = begin(5, 3, 0);
  at = standard(p, 56, FSX_NTFS_DIR);
  at = filename(p, at, 1, id(5), ".", 3);
  finish(p, at);
  p = begin(6, 3, 0);
  at = standard(p, 56, FSX_NTFS_DIR);
  list_entry(list, 0x30, 2, id(8), 0);
  at = resident(p, at, 0x20, 1, list, 32);
  finish(p, at);
  p = begin(8, 1, id(6));
  at = filename(p, 56, 2, id(5), "child", 1);
  at = filename(p, at, 3, id(5), "CHILD~1", 2);
  finish(p, at);
  p = begin(7, 3, 0);
  at = standard(p, 56, FSX_NTFS_DIR | FSX_NTFS_REPARSE);
  at = filename(p, at, 1, id(5), "junction", 1);
  finish(p, at);
  p = begin(9, 1, 0);
  finish(p, 56);
  protect_records();
}
suite("Raw NTFS MFT reader") {
  before_each() { setup(); }
  after_each() {
    check_null(pending.buffer);
    atomic_store(&engine->stopping, false);
    check_equal(fsx_engine_close(&engine, FSX_DEADLINE_INFINITE), FS_OK);
  }
  it("reads fragmented DATA and directory extension attributes with full "
     "FRNs") {
    FsNtfsRawStats stats;
    uint64_t owned = engine->owned;
    check_equal(inventory(&stats), FS_OK);
    check_equal(engine->owned, owned);
    check_equal(stats.slots, (uint64_t)16);
    check_equal(stats.records, (uint64_t)5);
    check_equal(stats.kernel_records, (uint64_t)5);
    check_equal(seen, 3u);
    check_equal(names[0], ".");
    check_equal(names[1], "child");
    check_equal(names[2], "junction");
    check_equal(parents[1], id(5));
    check_equal(identities[1], id(6));
    check_equal(stats.read_bytes, (uint64_t)RECORD * 12);
  }
  it("partitions optional stage timings without changing raw inventory "
     "results") {
    /* Give this measured stage observable work on every native clock. */
    measured_callback = true;
    FsNtfsRawStats stats;
    check_equal(fsx_ntfs_raw_inventory(engine, &config, &geometry, &io, NULL,
                                       directory, NULL, true, &stats),
                FS_OK);
    check_equal(stats.directories, (uint64_t)3);
    check_equal(stats.records, (uint64_t)5);
    check_true(stats.elapsed_ns >=
               stats.read_ns + stats.kernel_ns + stats.callback_ns);
    check_true(stats.callback_ns > 0);
    seen = 0;
    memset(names, 0, sizeof(names));
    check_equal(inventory(&stats), FS_OK);
    check_equal(stats.read_ns, (uint64_t)0);
    check_equal(stats.kernel_ns, (uint64_t)0);
    check_equal(stats.callback_ns, (uint64_t)0);
    check_equal(stats.elapsed_ns, (uint64_t)0);
  }
  it("uses 512-byte USA strides on 4096-byte logical sectors") {
    geometry.sector_bytes = 4096;
    FsNtfsRawStats stats;
    check_equal(inventory(&stats), FS_OK);
    check_equal(seen, 3u);
  }
  it("decodes negative mapping-pair LCN deltas") {
    unsigned char pairs[] = {0x11, 2, 8, 0x11, 2, 0xfa, 0};
    unsigned char bits[] = {0xe1, 7};
    unsigned char *p = begin(0, 1, 0);
    size_t at =
        nonresident(p, 56, 0x80, 3, 0, 3, pairs, sizeof(pairs), RECORD * SLOTS);
    at = resident(p, at, 0xb0, 2, bits, sizeof(bits));
    finish(p, at);
    protect_records();
    FsNtfsRawStats stats;
    check_equal(inventory(&stats), FS_OK);
    check_equal(seen, 3u);
  }
  it("assembles FILE records split across discontiguous volume extents") {
    geometry.cluster_bytes = 512;
    unsigned char pairs[] = {0x11, 1, 8, 0x11, 31, 24, 0};
    unsigned char bits[] = {0xe1, 7};
    unsigned char *p = begin(0, 1, 0);
    size_t at = nonresident(p, 56, 0x80, 3, 0, 31, pairs, sizeof(pairs),
                            RECORD * SLOTS);
    at = resident(p, at, 0xb0, 2, bits, sizeof(bits));
    finish(p, at);
    protect_records();
    unsigned char protected_records[SLOTS][RECORD];
    for (unsigned i = 0; i < SLOTS; i++)
      memcpy(protected_records[i], physical_record(i), RECORD);
    memset(disk, 0, sizeof(disk));
    for (unsigned i = 0; i < SLOTS * 2; i++) {
      size_t address = (i ? 32 + i - 1 : 8) * 512;
      memcpy(disk + address, (unsigned char *)protected_records + i * 512, 512);
    }
    FsNtfsRawStats stats;
    check_equal(inventory(&stats), FS_OK);
    check_equal(seen, 3u);
    check_equal(names[1], "child");
  }
  it("skips completely free bitmap chunks before issuing volume reads") {
    FsNtfsRawStats stats;
    memset(physical_record(12), 0xcc, RECORD * 4);
    check_equal(inventory(&stats), FS_OK);
    check_equal(stats.read_calls, (uint64_t)3);
    check_equal(stats.read_bytes, (uint64_t)RECORD * 12);
  }
  it("rejects mismatched FILE slot numbers and malformed USA/header bounds") {
    unsigned char original[RECORD];
    memcpy(original, records[5], RECORD);
    for (unsigned fault = 0; fault < 4; fault++) {
      memcpy(records[5], original, RECORD);
      if (fault == 0)
        put32(records[5] + 44, 6);
      if (fault == 1)
        put32(records[5] + 24, RECORD + 1);
      if (fault == 2)
        put16(records[5] + 20, RECORD);
      protect_records();
      if (fault == 3)
        put16(physical_record(5) + 6, 9);
      seen = 0;
      FsNtfsRawStats stats;
      check_equal(inventory(&stats), FS_CORRUPT);
    }
  }
  it("rejects torn USA records without partially applying fixups") {
    unsigned char damaged[RECORD], original[RECORD];
    memcpy(damaged, physical_record(5), RECORD);
    damaged[1022] ^= 1;
    memcpy(original, damaged, RECORD);
    check_equal(fsx_ntfs_raw_fixup(damaged, RECORD), FS_STALE);
    check_equal(damaged, original, RECORD);
    physical_record(5)[510] ^= 1;
    FsNtfsRawStats stats;
    check_equal(inventory(&stats), FS_STALE);
  }
  it("rejects invalid raw attribute sizes") {
    put32(records[5] + 60, 0);
    protect_records();
    FsNtfsRawStats stats;
    check_equal(inventory(&stats), FS_CORRUPT);
  }
  it("rejects an extension belonging to another base identity") {
    bad_extension = true;
    FsNtfsRawStats stats;
    check_equal(inventory(&stats), FS_CORRUPT);
  }
  it("rejects sparse mapping pairs and incomplete stream coverage") {
    records[0][56 + 64] = 1; /* Sparse run. */
    FsNtfsRawStats stats;
    check_equal(inventory(&stats), FS_UNSUPPORTED);
  }
  it("reads nonresident bitmap and attribute-list streams with sector "
     "rounding") {
    unsigned char *p = begin(0, 1, 0);
    unsigned char data[] = {0x11, 2, 8, 0x11, 2, 0xfa, 0};
    unsigned char bitmap_run[] = {0x11, 1, 12, 0};
    size_t at =
        nonresident(p, 56, 0x80, 3, 0, 3, data, sizeof(data), RECORD * SLOTS);
    at = nonresident(p, at, 0xb0, 2, 0, 0, bitmap_run, sizeof(bitmap_run), 2);
    finish(p, at);
    p = begin(6, 3, 0);
    at = standard(p, 56, FSX_NTFS_DIR);
    unsigned char list_run[] = {0x11, 1, 13, 0};
    at = nonresident(p, at, 0x20, 1, 0, 0, list_run, sizeof(list_run), 32);
    finish(p, at);
    protect_records();
    disk[12 * CLUSTER] = 0xe1;
    disk[12 * CLUSTER + 1] = 7;
    list_entry(disk + 13 * CLUSTER, 0x30, 2, id(8), 0);
    FsNtfsRawStats stats;
    check_equal(inventory(&stats), FS_OK);
    check_equal(seen, 3u);
    check_equal(names[1], "child");
  }
  it("rejects a runlist that does not cover its claimed VCN interval") {
    records[0][56 + 64 + 1] = 1;
    FsNtfsRawStats stats;
    check_equal(inventory(&stats), FS_CORRUPT);
  }
  it("rejects a stale extension sequence") {
    put16(records[8] + 16, 0x8124);
    FsNtfsRawStats stats;
    check_equal(inventory(&stats), FS_STALE);
  }
  it("rejects capacity before reading and releases failed allocations") {
    FsNtfsRawStats stats;
    config.max_mft_records = SLOTS - 1;
    check_equal(inventory(&stats), FS_LIMIT);
    check_equal(bootstraps, 0u);
    config.max_mft_records = SLOTS;
    uint64_t budget = engine->config.owned_memory_budget, owned = engine->owned;
    engine->config.owned_memory_budget = owned + 1024;
    FsStatus status = inventory(&stats);
    engine->config.owned_memory_budget = budget;
    check_equal(status, FS_NOMEM);
    check_equal(engine->owned, owned);
  }
  it("fails a relocated MFT mapping instead of publishing a mixed prefix") {
    relocate = true;
    FsNtfsRawStats stats;
    check_equal(inventory(&stats), FS_STALE);
  }
  it("propagates partial native reads and callback rejection") {
    fail_read = true;
    FsNtfsRawStats stats;
    uint64_t owned = engine->owned;
    check_equal(inventory(&stats), FS_IO);
    check_equal(engine->owned, owned);
    fail_read = false;
    callback_status = FS_DENIED;
    check_equal(inventory(&stats), FS_DENIED);
    check_equal(engine->owned, owned);
  }
  it("observes STOPPING between raw records and releases borrowed state") {
    cancel_visit = true;
    FsNtfsRawStats stats;
    uint64_t owned = engine->owned;
    check_equal(inventory(&stats), FS_CANCELLED);
    check_equal(engine->owned, owned);
    check_equal(begun, finished);
    check_equal(cancelled, 1u);
  }

  it("matches serial inventory while bounding prefetch and overlapping "
     "callbacks") {
    FsNtfsRawStats serial, prefetched;
    config.raw_flags = 0;
    check_equal(inventory(&serial), FS_OK);
    check_equal(begun, 0u);
    char saved_names[sizeof(names)];
    memcpy(saved_names, names, sizeof(names));
    uint64_t saved_parents[3], saved_ids[3];
    memcpy(saved_parents, parents, sizeof(parents));
    memcpy(saved_ids, identities, sizeof(identities));
    seen = 0;
    memset(names, 0, sizeof(names));
    config.raw_flags = FS_NTFS_RAW_PREFETCH | FS_NTFS_RAW_BATCH_DIRS;
    uint64_t owned = engine->owned;
    check_equal(inventory(&prefetched), FS_OK);
    check_equal(seen, 3u);
    check_equal(memcmp(saved_names, names, sizeof(names)), 0);
    check_equal(memcmp(saved_parents, parents, sizeof(parents)), 0);
    check_equal(memcmp(saved_ids, identities, sizeof(identities)), 0);
    check_equal(serial.slots, prefetched.slots);
    check_equal(serial.records, prefetched.records);
    check_equal(serial.read_bytes, prefetched.read_bytes);
    check_equal(serial.read_calls, prefetched.read_calls);
    check_equal(serial.kernel_records, prefetched.kernel_records);
    check_true(begun > 0);
    check_equal(begun, finished);
    check_equal(cancelled, 0u);
    check_true(callbacks_overlapped > 0);
    check_equal(engine->owned, owned);
  }
  it("reaps immediate completions exactly once before releasing their buffer") {
    complete_inline = true;
    uint64_t owned = engine->owned;
    FsNtfsRawStats stats;
    check_equal(inventory(&stats), FS_OK);
    check_equal(seen, 3u);
    check_true(begun > 0);
    check_equal(begun, finished);
    check_equal(cancelled, 0u);
    check_equal(engine->owned, owned);
    check_null(pending.buffer);
  }
  it("rejects missing asynchronous capabilities and unknown flags without "
     "fallback") {
    FsNtfsRawStats stats;
    FsNtfsRawIO incomplete = io;
    incomplete.finish = NULL;
    check_equal(fsx_ntfs_raw_inventory(engine, &config, &geometry, &incomplete,
                                       NULL, directory, NULL, false, &stats),
                FS_UNSUPPORTED);
    incomplete = io;
    incomplete.begin = NULL;
    check_equal(fsx_ntfs_raw_inventory(engine, &config, &geometry, &incomplete,
                                       NULL, directory, NULL, false, &stats),
                FS_UNSUPPORTED);
    config.raw_flags = UINT64_C(4);
    check_equal(inventory(&stats), FS_INVALID);
    check_equal(begun, 0u);
    check_equal(seen, 0u);
    config.raw_flags = 0;
    check_equal(fsx_ntfs_raw_inventory(engine, &config, &geometry, &incomplete,
                                       NULL, directory, NULL, false, &stats),
                FS_OK);
    check_equal(seen, 3u);
  }
  it("returns a submission failure without creating a pending borrow or "
     "retrying") {
    begin_fault = FS_DENIED;
    uint64_t owned = engine->owned;
    FsNtfsRawStats stats;
    check_equal(inventory(&stats), FS_DENIED);
    check_equal(seen, 0u);
    check_equal(begun, 0u);
    check_equal(finished, 0u);
    check_null(pending.buffer);
    check_equal(engine->owned, owned);
  }
  it("drains a failed completion including its late write before freeing "
     "buffers") {
    finish_fault = FS_IO;
    uint64_t owned = engine->owned;
    FsNtfsRawStats stats;
    check_equal(inventory(&stats), FS_IO);
    check_true(begun > 0);
    check_equal(begun, finished);
    check_equal(cancelled, 0u);
    check_equal(seen, 0u);
    check_null(pending.buffer);
    check_equal(engine->owned, owned);
  }
  it("preserves the callback error when cancellation drain also fails") {
    callback_status = FS_DENIED;
    finish_fault = FS_IO;
    fault_on_cancel_only = true;
    uint64_t owned = engine->owned;
    FsNtfsRawStats stats;
    check_equal(inventory(&stats), FS_DENIED);
    check_true(begun > 0);
    check_equal(begun, finished);
    check_equal(cancelled, 1u);
    check_null(pending.buffer);
    check_equal(engine->owned, owned);
  }
  it("charges the spare chunk to the budget and allows serial recovery") {
    config.buffer_bytes = 1048576;
    uint64_t owned = engine->owned, budget = engine->config.owned_memory_budget;
    engine->config.owned_memory_budget = owned + 4u * 1024u * 1024u;
    FsNtfsRawStats stats;
    config.raw_flags = 0;
    FsStatus serial = inventory(&stats);
    seen = 0;
    memset(names, 0, sizeof(names));
    config.raw_flags = FS_NTFS_RAW_PREFETCH;
    FsStatus prefetched = inventory(&stats);
    engine->config.owned_memory_budget = budget;
    check_equal(serial, FS_OK);
    check_equal(prefetched, FS_NOMEM);
    check_equal(engine->owned, owned);
    check_null(pending.buffer);
    check_equal(begun, 0u);
    check_equal(inventory(&stats), FS_OK);
    check_equal(seen, 3u);
    check_equal(engine->owned, owned);
  }
}

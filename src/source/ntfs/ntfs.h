#ifndef FSX_NTFS_INTERNAL_H
#define FSX_NTFS_INTERNAL_H
#include "../../internal.h"
enum {
  FSX_NTFS_DIR = 0x10,
  FSX_NTFS_REPARSE = 0x400,
  FSX_USN_CREATE = 0x100,
  FSX_USN_DELETE = 0x200,
  FSX_USN_OLD = 0x1000,
  FSX_USN_NEW = 0x2000,
  FSX_USN_LINK = 0x10000
};
typedef struct FsNtfsIdentity {
  uint64_t volume_serial, common_volume, root_frn, volume_root_frn, journal_id;
  int64_t first_usn, lowest_usn, next_usn;
  char volume_guid[96];
} FsNtfsIdentity;
typedef struct FsNtfsRecord {
  uint64_t frn, parent;
  int64_t usn;
  uint32_t reason, attributes, length;
  const unsigned char *name;
  uint16_t name_bytes;
} FsNtfsRecord;
typedef FsStatus (*FsNtfsRecordFn)(void *, const FsNtfsRecord *);
typedef FsStatus (*FsNtfsParentFn)(void *, uint64_t);

typedef struct FsNtfsRawStats {
  uint64_t slots, records, read_calls, read_bytes, kernel_records;
} FsNtfsRawStats;
typedef struct FsNtfsOps {
  FsStatus (*query)(void *, FsNtfsIdentity *);
  FsStatus (*mft)(void *, uint64_t, void *, size_t, size_t *);
  FsStatus (*read)(void *, int64_t, uint64_t, void *, size_t, size_t *);
  FsStatus (*list)(void *, uint64_t, FsObserve, void *);
  FsStatus (*stat)(void *, uint64_t, FsObserved *);
  uint32_t (*error)(void *);
  void (*close)(void *);
  FsStatus (*links)(void *, uint64_t, FsNtfsParentFn, void *);
  FsStatus (*inventory)(void *, FsNtfsRecordFn, void *, FsNtfsRawStats *);
} FsNtfsOps;
struct FsNtfs {
  FsEngine *engine;
  FsNtfs *next;
  FsRootHealth *health;
  const FsNtfsOps *ops;
  void *provider;
  char *root;
  unsigned char *buffer;
  FsNtfsConfig config;
  FsNtfsIdentity identity;
  FsNtfsInfo info;
  bool dirty;
};
static inline uint64_t fsx_ntfs_get64(const unsigned char *p) {
  uint64_t value = 0;
  for (unsigned i = 0; i < 8; i++)
    value |= (uint64_t)p[i] << (8 * i);
  return value;
}
static inline void fsx_ntfs_put64(unsigned char p[8], uint64_t value) {
  for (unsigned i = 0; i < 8; i++)
    p[i] = (unsigned char)(value >> (8 * i));
}
FsStatus fsx_ntfs_records(const void *, size_t, bool journal, uint64_t from,
                          int64_t target, uint32_t limit, FsNtfsRecordFn,
                          void *, uint64_t *next, uint32_t *count);
FsStatus fsx_ntfs_link_records(const void *, size_t, uint32_t, FsNtfsParentFn,
                               void *, uint32_t *);
FsStatus fsx_ntfs_text(FsEngine *, const void *, size_t, char **);
FsStatus fsx_ntfs_open_provider(FsEngine *, const char *, const FsNtfsConfig *,
                                const FsNtfsOps *, void *, FsNtfs **);
FsStatus fsx_ntfs_native_open(FsEngine *, const char *, const FsNtfsConfig *,
                              const FsNtfsOps **, void **);
FsStatus fsx_ntfs_schema(FsEngine *);
FsStatus fsx_ntfs_checkpoint(FsNtfs *);
FsStatus fsx_ntfs_store(FsNtfs *, bool rebuild);
bool fsx_ntfs_same_identity(const FsNtfsIdentity *, const FsNtfsIdentity *);
#endif

#ifndef FSX_NTFS_RAW_H
#define FSX_NTFS_RAW_H
#include "ntfs.h"
typedef struct FsNtfsRawGeometry {
  uint64_t volume_bytes, mft_lcn, mft_bytes;
  uint32_t sector_bytes, cluster_bytes, record_bytes;
} FsNtfsRawGeometry;
typedef struct FsNtfsRawIO {
  /* Exact aligned volume read. Synchronous, no retained buffer view. */
  FsStatus (*read)(void *, uint64_t, void *, size_t);
  /* Exact full-FRN lookup; 0 bootstraps $MFT. Returns kernel-normalized FILE.
   */
  FsStatus (*record)(void *, uint64_t, void *, size_t, size_t *);
  /* At most one pending read. A successful begin pins its buffer until finish.
   * finish(cancel=true) must also drain; no borrow survives its return. */
  FsStatus (*begin)(void *, uint64_t, void *, size_t);
  FsStatus (*finish)(void *, bool cancel);
} FsNtfsRawIO;
FsStatus fsx_ntfs_raw_inventory(FsEngine *, const FsNtfsConfig *,
                                const FsNtfsRawGeometry *, const FsNtfsRawIO *,
                                void *, FsNtfsRecordFn, void *, bool profile,
                                FsNtfsRawStats *);
FsStatus fsx_ntfs_raw_fixup(void *, size_t);
#endif

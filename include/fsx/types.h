#ifndef FSX_TYPES_H
#define FSX_TYPES_H
#include <cmeta/enum.h>
#include <cmeta/struct.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define FSX_API_VERSION 1u
#define FSX_DEADLINE_INFINITE UINT64_MAX
// clang-format off
cmeta_enum(FsStatus,
  (FS_OK, 0, "ok"), (FS_WAIT, 1, "wait"), (FS_END, 2, "end"),
  (FS_INVALID, -1, "invalid"), (FS_NOMEM, -2, "no_memory"),
  (FS_LIMIT, -3, "limit"), (FS_IO, -4, "io"), (FS_DENIED, -5, "denied"),
  (FS_CANCELLED, -6, "cancelled"), (FS_STALE, -7, "stale"),
  (FS_UNSUPPORTED, -8, "unsupported"), (FS_BUSY, -9, "busy"),
  (FS_CORRUPT, -10, "corrupt"), (FS_TIMEOUT, -11, "timeout"));
cmeta_struct(FsFileKey,
  cmeta_field(uint32_t, source_id)
  cmeta_field(uint64_t, file_id_lo)
  cmeta_field(uint64_t, file_id_hi));
cmeta_struct(FsHit,
  cmeta_field(uint64_t, snapshot_epoch)
  cmeta_field(uint32_t, entry_id)
  cmeta_field(uint32_t, reserved));
cmeta_struct(FsQueryOptions,
  cmeta_field(uint32_t, struct_size)
  cmeta_field(uint32_t, api_version)
  cmeta_field(uint32_t, flags)
  cmeta_field(uint32_t, sort_key)
  cmeta_field(uint32_t, offset)
  cmeta_field(uint32_t, limit)
  cmeta_field(uint64_t, deadline_ns)
  cmeta_field(int32_t, timezone_offset_minutes));
// clang-format on

enum { FS_SORT_NAME, FS_SORT_PATH, FS_SORT_SIZE, FS_SORT_MTIME };
enum {
  FS_QUERY_DESCENDING = 1u,
  FS_QUERY_CASE_SENSITIVE = 2u,
  FS_QUERY_ALLOW_PARTIAL_TEXT = 4u
};
enum {
  FS_ENGINE_REGEX_JIT = 1u,
  FS_ENGINE_CREATE_DATABASE = 2u
};
enum {
  FS_ENTRY_DIRECTORY = 1u,
  FS_ENTRY_LINK = 2u,
  FS_ENTRY_SIZE_VALID = 4u,
  FS_ENTRY_MTIME_VALID = 8u
};
enum {
  FS_QUERY_QUEUED,
  FS_QUERY_RUNNING,
  FS_QUERY_READY,
  FS_QUERY_FAILED,
  FS_QUERY_CANCELLED
};
#endif

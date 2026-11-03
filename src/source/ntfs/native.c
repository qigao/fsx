#if defined(_WIN32) && (!defined(_WIN32_WINNT) || _WIN32_WINNT < 0x0602)
#undef _WIN32_WINNT
#define _WIN32_WINNT 0x0602
#endif
#include "raw.h"
#include <stdio.h>
#include <stdlib.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <wchar.h>
#include <windows.h>
#include <winioctl.h>
#include <winternl.h>
typedef struct Native {
  FsEngine *engine;
  HANDLE volume, async_volume, raw_event;
  OVERLAPPED raw_overlapped;
  DWORD raw_pending_bytes;
  bool raw_pending;
  wchar_t *root;
  FsNtfsIdentity identity;
  uint32_t error, buffer_bytes;
  FsNtfsConfig config;
} Native;
static FsStatus error_status(Native *n, DWORD error) {
  n->error = error;
  if (error == ERROR_ACCESS_DENIED || error == ERROR_PRIVILEGE_NOT_HELD)
    return FS_DENIED;
  if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ||
      error == ERROR_DELETE_PENDING)
    return FS_END;
  if (error == ERROR_JOURNAL_DELETE_IN_PROGRESS ||
      error == ERROR_JOURNAL_NOT_ACTIVE || error == ERROR_JOURNAL_ENTRY_DELETED)
    return FS_STALE;
  if (error == ERROR_INVALID_FUNCTION || error == ERROR_NOT_SUPPORTED)
    return FS_UNSUPPORTED;
  if (error == ERROR_NOT_ENOUGH_MEMORY || error == ERROR_OUTOFMEMORY)
    return FS_NOMEM;
  return FS_IO;
}
/* A constructed FileIdType descriptor and the pinned NTFS volume are already
 * admitted. Windows returns INVALID_PARAMETER for a deleted/reused historical
 * FRN as well as FILE_NOT_FOUND. Restrict this classification to OpenFileById;
 * an invalid FSCTL request must still fail. Final identity/journal checks
 * remain mandatory before committing any removal. */
static FsStatus id_error(Native *n, DWORD error) {
  if (error == ERROR_INVALID_PARAMETER) {
    n->error = error;
    return FS_END;
  }
  return error_status(n, error);
}
static uint64_t file_id(const BY_HANDLE_FILE_INFORMATION *info) {
  return (uint64_t)info->nFileIndexHigh << 32 | info->nFileIndexLow;
}
static HANDLE by_id(Native *n, uint64_t frn, DWORD access) {
  FILE_ID_DESCRIPTOR id = {0};
  id.dwSize = sizeof(id);
  id.Type = FileIdType;
  id.FileId.QuadPart = (LONGLONG)frn;
  return OpenFileById(
      n->volume, &id, access,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
      FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT);
}
static FsStatus query(void *context, FsNtfsIdentity *out) {
  Native *n = context;
  HANDLE root = CreateFileW(
      n->root, FILE_READ_ATTRIBUTES,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
      OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
      NULL);
  if (root == INVALID_HANDLE_VALUE) {
    FsStatus status = error_status(n, GetLastError());
    return status == FS_END ? FS_STALE : status;
  }
  BY_HANDLE_FILE_INFORMATION info;
  BOOL ok = GetFileInformationByHandle(root, &info);
  DWORD error = GetLastError();
  CloseHandle(root);
  if (!ok)
    return error_status(n, error);
  if (file_id(&info) != n->identity.root_frn ||
      info.dwVolumeSerialNumber != n->identity.common_volume ||
      !(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
      info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
    return FS_STALE;
  USN_JOURNAL_DATA data;
  DWORD bytes;
  if (!DeviceIoControl(n->volume, FSCTL_QUERY_USN_JOURNAL, NULL, 0, &data,
                       sizeof(data), &bytes, NULL))
    return error_status(n, GetLastError());
  if (bytes < sizeof(data))
    return FS_CORRUPT;
  *out = n->identity;
  out->journal_id = data.UsnJournalID;
  out->first_usn = data.FirstUsn;
  out->lowest_usn = data.LowestValidUsn;
  out->next_usn = data.NextUsn;
  if (out->first_usn < 0 || out->lowest_usn < 0 ||
      out->next_usn < out->first_usn || out->next_usn < out->lowest_usn)
    return FS_CORRUPT;
  n->error = 0;
  return FS_OK;
}
static FsStatus mft(void *context, uint64_t from, void *buffer, size_t capacity,
                    size_t *size) {
  Native *n = context;
  MFT_ENUM_DATA request = {0};
  request.StartFileReferenceNumber = from;
  request.LowUsn = 0;
  request.HighUsn = INT64_MAX;
  DWORD bytes = 0;
  BOOL ok =
      DeviceIoControl(n->volume, FSCTL_ENUM_USN_DATA, &request, sizeof(request),
                      buffer, (DWORD)capacity, &bytes, NULL);
  *size = bytes;
  if (!ok) {
    DWORD error = GetLastError();
    if (error == ERROR_HANDLE_EOF || error == ERROR_NO_MORE_FILES)
      return FS_END;
    return error_status(n, error);
  }
  return FS_OK;
}
static FsStatus read_journal(void *context, int64_t from, uint64_t journal,
                             void *buffer, size_t capacity, size_t *size) {
  Native *n = context;
  /* READ_USN_JOURNAL_DATA_V1 layout, also supported by the selected older
   * MinGW headers which only name the V0 structure. */
  struct Request {
    int64_t start;
    uint32_t reasons, close_only;
    uint64_t timeout, wait_bytes, journal;
    uint16_t min_major, max_major;
  } request = {0};
  request.start = from;
  request.reasons = UINT32_MAX;
  request.journal = journal;
  request.min_major = request.max_major = 2;
  DWORD bytes = 0;
  BOOL ok =
      DeviceIoControl(n->volume, FSCTL_READ_USN_JOURNAL, &request,
                      sizeof(request), buffer, (DWORD)capacity, &bytes, NULL);
  *size = bytes;
  return ok ? FS_OK : error_status(n, GetLastError());
}
static FsStatus stat_file(void *context, uint64_t frn, FsObserved *out) {
  Native *n = context;
  HANDLE file = by_id(n, frn, FILE_READ_ATTRIBUTES);
  if (file == INVALID_HANDLE_VALUE)
    return id_error(n, GetLastError());
  BY_HANDLE_FILE_INFORMATION info;
  BOOL ok = GetFileInformationByHandle(file, &info);
  DWORD error = GetLastError();
  CloseHandle(file);
  if (!ok)
    return error_status(n, error);
  if (file_id(&info) != frn)
    return FS_END;
  memset(out, 0, sizeof(*out));
  out->file_lo = frn;
  out->size = (uint64_t)info.nFileSizeHigh << 32 | info.nFileSizeLow;
  out->mtime = (uint64_t)info.ftLastWriteTime.dwHighDateTime << 32 |
               info.ftLastWriteTime.dwLowDateTime;
  out->flags =
      FS_ENTRY_MTIME_VALID |
      (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY ? FS_ENTRY_DIRECTORY
                                                        : FS_ENTRY_SIZE_VALID) |
      (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT ? FS_ENTRY_LINK
                                                            : 0);
  return FS_OK;
}
static FsStatus list(void *context, uint64_t frn, FsObserve visit,
                     void *target) {
  Native *n = context;
  HANDLE directory = by_id(n, frn, FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES);
  if (directory == INVALID_HANDLE_VALUE)
    return id_error(n, GetLastError());
  BY_HANDLE_FILE_INFORMATION identity;
  FsStatus status = FS_OK;
  if (!GetFileInformationByHandle(directory, &identity))
    status = error_status(n, GetLastError());
  else if (file_id(&identity) != frn ||
           !(identity.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
           identity.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
    status = FS_END;
  enum { CAPACITY = 4096 };
  unsigned char *buffer =
      status == FS_OK ? fsx_alloc(n->engine, CAPACITY) : NULL;
  if (status == FS_OK && !buffer)
    status = FS_NOMEM;
  bool first = true;
  while (status == FS_OK) {
    if (fsx_writer_stopping(n->engine)) {
      status = FS_CANCELLED;
      break;
    }
    memset(buffer, 0, CAPACITY);
    BOOL ok = GetFileInformationByHandleEx(
        directory,
        first ? FileIdBothDirectoryRestartInfo : FileIdBothDirectoryInfo,
        buffer, CAPACITY);
    first = false;
    if (!ok) {
      DWORD error = GetLastError();
      if (error != ERROR_NO_MORE_FILES)
        status = error_status(n, error);
      break;
    }
    size_t at = 0, fixed = offsetof(FILE_ID_BOTH_DIR_INFO, FileName);
    for (;;) {
      if (CAPACITY - at < fixed) {
        status = FS_CORRUPT;
        break;
      }
      const FILE_ID_BOTH_DIR_INFO *record = (const void *)(buffer + at);
      size_t length = record->FileNameLength;
      if (!length || length % 2 || length > CAPACITY - at - fixed ||
          (record->NextEntryOffset &&
           (record->NextEntryOffset % 8 ||
            record->NextEntryOffset < fixed + length ||
            record->NextEntryOffset > CAPACITY - at - fixed))) {
        status = FS_CORRUPT;
        break;
      }
      bool dot = length == 2 && record->FileName[0] == L'.';
      bool dotdot = length == 4 && record->FileName[0] == L'.' &&
                    record->FileName[1] == L'.';
      if (!dot && !dotdot) {
        char *name = NULL;
        status = fsx_ntfs_text(n->engine, record->FileName, length, &name);
        if (status == FS_OK) {
          FsObserved item = {0};
          item.name = name;
          item.raw_name = record->FileName;
          item.raw_bytes = length;
          item.file_lo = (uint64_t)record->FileId.QuadPart;
          item.size = (uint64_t)record->EndOfFile.QuadPart;
          item.mtime = (uint64_t)record->LastWriteTime.QuadPart;
          item.flags = FS_ENTRY_MTIME_VALID |
                       (record->FileAttributes & FILE_ATTRIBUTE_DIRECTORY
                            ? FS_ENTRY_DIRECTORY
                            : FS_ENTRY_SIZE_VALID) |
                       (record->FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT
                            ? FS_ENTRY_LINK
                            : 0);
          status = visit(target, &item);
        }
        fsx_free(name);
      }
      if (status != FS_OK || !record->NextEntryOffset)
        break;
      at += record->NextEntryOffset;
    }
  }
  fsx_free(buffer);
  CloseHandle(directory);
  return status;
}

static FsStatus link_parents(void *context, uint64_t frn, FsNtfsParentFn visit,
                             void *target) {
  Native *n = context;
  if (fsx_writer_stopping(n->engine))
    return FS_CANCELLED;
  HANDLE file = by_id(n, frn, FILE_READ_ATTRIBUTES);
  if (file == INVALID_HANDLE_VALUE)
    return id_error(n, GetLastError());
  unsigned char *buffer = fsx_alloc(n->engine, n->buffer_bytes);
  if (!buffer) {
    CloseHandle(file);
    return FS_NOMEM;
  }
  IO_STATUS_BLOCK io = {0};
  NTSTATUS result = NtQueryInformationFile(file, &io, buffer, n->buffer_bytes,
                                           FileHardLinkInformation);
  CloseHandle(file);
  FsStatus status;
  if (result < 0) {
    if ((uint32_t)result == UINT32_C(0x80000005) ||
        (uint32_t)result == UINT32_C(0xc0000023))
      status = FS_LIMIT;
    else
      status = error_status(n, RtlNtStatusToDosError(result));
  } else if (io.Information > n->buffer_bytes)
    status = FS_CORRUPT;
  else {
    uint32_t count;
    status = fsx_ntfs_link_records(buffer, (size_t)io.Information,
                                   n->engine->config.max_entries, visit, target,
                                   &count);
  }
  fsx_free(buffer);
  return status;
}
static FsStatus raw_read(void *context, uint64_t offset, void *out,
                         size_t bytes) {
  Native *n = context;
  LARGE_INTEGER position;
  position.QuadPart = (LONGLONG)offset;
  if (!SetFilePointerEx(n->volume, position, NULL, FILE_BEGIN))
    return error_status(n, GetLastError());
  DWORD returned = 0;
  if (!ReadFile(n->volume, out, (DWORD)bytes, &returned, NULL))
    return error_status(n, GetLastError());
  return returned == bytes ? FS_OK : FS_IO;
}
static FsStatus raw_begin(void *context, uint64_t offset, void *out,
                          size_t bytes) {
  Native *n = context;
  if (n->raw_pending)
    return FS_BUSY;
  if (fsx_writer_stopping(n->engine))
    return FS_CANCELLED;
  if (n->async_volume == INVALID_HANDLE_VALUE || !n->raw_event)
    return FS_UNSUPPORTED;
  if (!ResetEvent(n->raw_event))
    return error_status(n, GetLastError());
  memset(&n->raw_overlapped, 0, sizeof(n->raw_overlapped));
  n->raw_overlapped.Offset = (DWORD)offset;
  n->raw_overlapped.OffsetHigh = (DWORD)(offset >> 32);
  n->raw_overlapped.hEvent = n->raw_event;
  BOOL ok =
      ReadFile(n->async_volume, out, (DWORD)bytes, NULL, &n->raw_overlapped);
  if (!ok) {
    DWORD error = GetLastError();
    if (error != ERROR_IO_PENDING)
      return error_status(n, error);
  }
  n->raw_pending_bytes = (DWORD)bytes;
  n->raw_pending =
      true; /* Includes immediate completion; finish still reaps it. */
  return FS_OK;
}
static FsStatus raw_finish(void *context, bool cancel) {
  Native *n = context;
  if (!n->raw_pending)
    return FS_INVALID;
  DWORD returned = 0, error = 0, wait_error = 0;
  bool cancelled = false;
  BOOL ok = FALSE;
  for (;;) {
    if (cancel || fsx_writer_stopping(n->engine)) {
      CancelIoEx(n->async_volume, &n->raw_overlapped);
      cancelled = true;
      break;
    }
    /* Immediate completion does not require waiting on an event. */
    ok = GetOverlappedResult(n->async_volume, &n->raw_overlapped, &returned,
                             FALSE);
    if (ok)
      break;
    error = GetLastError();
    if (error != ERROR_IO_INCOMPLETE)
      break;
    DWORD waited = WaitForSingleObject(n->raw_event, 50);
    if (waited != WAIT_OBJECT_0 && waited != WAIT_TIMEOUT) {
      wait_error = GetLastError();
      if (!wait_error)
        wait_error = ERROR_GEN_FAILURE;
      CancelIoEx(n->async_volume, &n->raw_overlapped);
      break;
    }
  }
  if (cancelled || wait_error) {
    /* A cancel request is not completion. Drain before ending either borrow. */
    ok = GetOverlappedResult(n->async_volume, &n->raw_overlapped, &returned,
                             TRUE);
    error = GetLastError();
  }
  n->raw_pending = false;
  if (wait_error)
    return error_status(n, wait_error);
  if (cancelled)
    return FS_CANCELLED;
  if (!ok)
    return error_status(n, error);
  return returned == n->raw_pending_bytes ? FS_OK : FS_IO;
}
static FsStatus raw_record(void *context, uint64_t frn, void *out,
                           size_t capacity, size_t *bytes) {
  Native *n = context;
  NTFS_FILE_RECORD_INPUT_BUFFER request = {0};
  request.FileReferenceNumber.QuadPart = (LONGLONG)frn;
  DWORD returned = 0;
  if (!DeviceIoControl(n->volume, FSCTL_GET_NTFS_FILE_RECORD, &request,
                       sizeof(request), out, (DWORD)capacity, &returned, NULL))
    return error_status(n, GetLastError());
  const NTFS_FILE_RECORD_OUTPUT_BUFFER *reply = out;
  size_t fixed = offsetof(NTFS_FILE_RECORD_OUTPUT_BUFFER, FileRecordBuffer);
  if (returned < fixed || reply->FileRecordLength > returned - fixed)
    return FS_CORRUPT;
  if (((uint64_t)reply->FileReferenceNumber.QuadPart &
       UINT64_C(0x0000ffffffffffff)) != (frn & UINT64_C(0x0000ffffffffffff)))
    return FS_END; /* The IOCTL may return a lower allocated record. */
  *bytes = reply->FileRecordLength;
  memmove(out, reply->FileRecordBuffer, *bytes);
  /* The kernel supplies a normalized record. Some systems return the protected
   * form instead. Only fix it when ALL trailers match; raw reads are stricter.
   */
  unsigned char *p = out;
  if (*bytes >= 512 && *bytes <= 65536 && *bytes % 512 == 0) {
    uint32_t usa = (uint32_t)p[4] | (uint32_t)p[5] << 8;
    uint32_t count = (uint32_t)p[6] | (uint32_t)p[7] << 8;
    if (usa >= 48 && usa + count * 2 <= 510 && count == *bytes / 512 + 1) {
      bool protected = true;
      for (uint32_t i = 1; i < count; i++)
        if (memcmp(p + i * 512 - 2, p + usa, 2))
          protected = false;
      if (protected)
        return fsx_ntfs_raw_fixup(out, *bytes);
    }
  }
  return FS_OK;
}
static FsStatus raw_geometry(Native *n, FsNtfsRawGeometry *out) {
  struct VolumeData {
    NTFS_VOLUME_DATA_BUFFER basic;
    NTFS_EXTENDED_VOLUME_DATA extended;
  } data = {0};
  DWORD bytes = 0;
  if (!DeviceIoControl(n->volume, FSCTL_GET_NTFS_VOLUME_DATA, NULL, 0, &data,
                       sizeof(data), &bytes, NULL))
    return error_status(n, GetLastError());
  if (bytes < sizeof(data) || data.extended.ByteCount < sizeof(data.extended))
    return FS_CORRUPT;
  if (data.extended.MajorVersion != 3 || data.extended.MinorVersion != 1)
    return FS_UNSUPPORTED;
  if ((uint64_t)data.basic.VolumeSerialNumber.QuadPart !=
      n->identity.volume_serial)
    return FS_STALE;
  if (data.basic.NumberSectors.QuadPart <= 0 || !data.basic.BytesPerSector ||
      (uint64_t)data.basic.NumberSectors.QuadPart >
          INT64_MAX / data.basic.BytesPerSector ||
      data.basic.MftStartLcn.QuadPart < 0 ||
      data.basic.MftValidDataLength.QuadPart <= 0)
    return FS_CORRUPT;
  *out = (FsNtfsRawGeometry){(uint64_t)data.basic.NumberSectors.QuadPart *
                                 data.basic.BytesPerSector,
                             (uint64_t)data.basic.MftStartLcn.QuadPart,
                             (uint64_t)data.basic.MftValidDataLength.QuadPart,
                             data.basic.BytesPerSector,
                             data.basic.BytesPerCluster,
                             data.basic.BytesPerFileRecordSegment};
  return FS_OK;
}
static FsStatus raw_inventory(void *context, FsNtfsRecordFn visit, void *target,
                              FsNtfsRawStats *stats) {
  Native *n = context;
  FsNtfsRawGeometry before, after;
  FsStatus status = raw_geometry(n, &before);
  if (status != FS_OK)
    return status;
  static const FsNtfsRawIO io = {raw_read, raw_record, raw_begin, raw_finish};
  const char *setting = getenv("FSX_NTFS_RAW_PROFILE");
  bool profile = setting && !strcmp(setting, "1");
  status = fsx_ntfs_raw_inventory(n->engine, &n->config, &before, &io, n, visit,
                                  target, profile, stats);
  if (status == FS_OK)
    status = raw_geometry(n, &after);
  if (status == FS_OK && (before.volume_bytes != after.volume_bytes ||
                          before.mft_lcn != after.mft_lcn ||
                          before.sector_bytes != after.sector_bytes ||
                          before.cluster_bytes != after.cluster_bytes ||
                          before.record_bytes != after.record_bytes ||
                          after.mft_bytes < before.mft_bytes))
    status = FS_STALE;
  return status;
}
static uint32_t last_error(void *context) { return ((Native *)context)->error; }
static void close_native(void *context) {
  Native *n = context;
  if (n->raw_pending)
    raw_finish(n, true);
  if (n->raw_event)
    CloseHandle(n->raw_event);
  if (n->async_volume != INVALID_HANDLE_VALUE)
    CloseHandle(n->async_volume);
  if (n->volume != INVALID_HANDLE_VALUE)
    CloseHandle(n->volume);
  fsx_free(n->root);
  fsx_free(n);
}
static const FsNtfsOps ops = {query,        mft,          read_journal,
                              list,         stat_file,    last_error,
                              close_native, link_parents, raw_inventory};

static FsStatus database_outside(Native *n, HANDLE root) {
  if (!n->engine->reader_path)
    return FS_OK;
  wchar_t *path = fsx_wide(n->engine, n->engine->reader_path);
  if (!path)
    return FS_NOMEM;
  HANDLE db =
      CreateFileW(path, FILE_READ_ATTRIBUTES,
                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                  OPEN_EXISTING, 0, NULL);
  fsx_free(path);
  if (db == INVALID_HANDLE_VALUE)
    return error_status(n, GetLastError());
  DWORD flags = FILE_NAME_NORMALIZED | VOLUME_NAME_GUID;
  DWORD a = GetFinalPathNameByHandleW(root, NULL, 0, flags),
        b = GetFinalPathNameByHandleW(db, NULL, 0, flags);
  FsStatus status = FS_OK;
  wchar_t *parent = NULL, *child = NULL;
  if (!a || !b || a > 32767 || b > 32767)
    status = FS_IO;
  if (status == FS_OK) {
    parent = fsx_alloc(n->engine, ((size_t)a + 1) * sizeof(wchar_t));
    child = fsx_alloc(n->engine, ((size_t)b + 1) * sizeof(wchar_t));
    if (!parent || !child)
      status = FS_NOMEM;
  }
  if (status == FS_OK) {
    DWORD x = GetFinalPathNameByHandleW(root, parent, a + 1, flags);
    DWORD y = GetFinalPathNameByHandleW(db, child, b + 1, flags);
    if (!x || x > a || !y || y > b)
      status = FS_IO;
    else if (y >= x &&
             CompareStringOrdinal(parent, (int)x, child, (int)x, TRUE) ==
                 CSTR_EQUAL &&
             (parent[x - 1] == L'\\' || child[x] == L'\\' || !child[x]))
      status = FS_INVALID;
  }
  fsx_free(parent);
  fsx_free(child);
  CloseHandle(db);
  return status;
}
FsStatus fsx_ntfs_native_open(FsEngine *e, const char *root,
                              const FsNtfsConfig *config,
                              const FsNtfsOps **out_ops, void **out) {
  const char *phase = "root admission";
  *out = NULL;
  Native *n = fsx_alloc(e, sizeof(*n));
  if (!n)
    return FS_NOMEM;
  memset(n, 0, sizeof(*n));
  n->engine = e;
  n->buffer_bytes = config->buffer_bytes;
  n->config = *config;
  n->config.open_error = NULL;
  n->volume = n->async_volume = INVALID_HANDLE_VALUE;
  char *canonical = NULL;
  FsStatus status = fsx_native_root(e, root, &canonical);
  if (status != FS_OK) {
    close_native(n);
    return status;
  }
  n->root = fsx_wide(e, canonical);
  fsx_free(canonical);
  if (!n->root) {
    close_native(n);
    return FS_NOMEM;
  }
  HANDLE handle = CreateFileW(
      n->root, FILE_READ_ATTRIBUTES,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
      OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
      NULL);
  if (handle == INVALID_HANDLE_VALUE) {
    status = error_status(n, GetLastError());
    goto failed;
  }
  phase = "database scope";
  status = database_outside(n, handle);
  if (status != FS_OK) {
    CloseHandle(handle);
    goto failed;
  }
  FILE_ID_INFO id;
  BY_HANDLE_FILE_INFORMATION basic;
  wchar_t filesystem[32], mount[32768], guid[96];
  phase = "file identity";
  BOOL info =
      GetFileInformationByHandleEx(handle, FileIdInfo, &id, sizeof(id)) &&
      GetFileInformationByHandle(handle, &basic);
  if (info) {
    phase = "filesystem identity";
    info = GetVolumeInformationByHandleW(handle, NULL, 0, NULL, NULL, NULL,
                                         filesystem, 32);
  }
  DWORD error = GetLastError();
  CloseHandle(handle);
  if (!info) {
    status = error_status(n, error);
    goto failed;
  }
  phase = "NTFS filesystem profile";
  if (wcscmp(filesystem, L"NTFS")) {
    status = FS_UNSUPPORTED;
    goto failed;
  }
  n->identity.volume_serial = id.VolumeSerialNumber;
  n->identity.common_volume = basic.dwVolumeSerialNumber;
  n->identity.root_frn = file_id(&basic);
  phase = "volume GUID";
  if (!GetVolumePathNameW(n->root, mount, 32768) ||
      !GetVolumeNameForVolumeMountPointW(mount, guid, 96)) {
    status = error_status(n, GetLastError());
    goto failed;
  }
  phase = "volume root identity";
  HANDLE volume_root = CreateFileW(
      guid, FILE_READ_ATTRIBUTES,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
      OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
      NULL);
  if (volume_root == INVALID_HANDLE_VALUE) {
    status = error_status(n, GetLastError());
    goto failed;
  }
  info = GetFileInformationByHandle(volume_root, &basic);
  error = GetLastError();
  CloseHandle(volume_root);
  if (!info) {
    status = error_status(n, error);
    goto failed;
  }
  n->identity.volume_root_frn = file_id(&basic);
  char *text = fsx_utf8_from_wide(e, guid);
  if (!text) {
    status = FS_NOMEM;
    goto failed;
  }
  if (strlen(text) >= sizeof(n->identity.volume_guid)) {
    fsx_free(text);
    status = FS_LIMIT;
    goto failed;
  }
  strcpy(n->identity.volume_guid, text);
  fsx_free(text);
  size_t length = wcslen(guid);
  if (!length || guid[length - 1] != L'\\') {
    status = FS_CORRUPT;
    goto failed;
  }
  guid[length - 1] = 0;
  phase = "volume journal handle";
  n->volume =
      CreateFileW(guid, GENERIC_READ,
                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                  OPEN_EXISTING, 0, NULL);
  if (n->volume == INVALID_HANDLE_VALUE) {
    status = error_status(n, GetLastError());
    goto failed;
  }
  phase = "journal query";
  FsNtfsIdentity journal;
  status = query(n, &journal);
  if (status != FS_OK)
    goto failed;
  if (config->mft_reader == FS_NTFS_MFT_RAW &&
      (config->raw_flags & FS_NTFS_RAW_PREFETCH)) {
    phase = "raw prefetch volume";
    n->async_volume =
        CreateFileW(guid, GENERIC_READ,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
    if (n->async_volume == INVALID_HANDLE_VALUE) {
      status = error_status(n, GetLastError());
      goto failed;
    }
    phase = "raw prefetch completion event";
    n->raw_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!n->raw_event) {
      status = error_status(n, GetLastError());
      goto failed;
    }
  }
  *out_ops = &ops;
  *out = n;
  return FS_OK;
failed:
  if (config->open_error) {
    config->open_error->status = status;
    config->open_error->native_code = (int)n->error;
    snprintf(config->open_error->diagnostic,
             sizeof(config->open_error->diagnostic), "%s", phase);
  }
  close_native(n);
  return status;
}
#else
FsStatus fsx_ntfs_native_open(FsEngine *e, const char *root,
                              const FsNtfsConfig *config, const FsNtfsOps **ops,
                              void **context) {
  (void)e;
  (void)root;
  (void)config;
  (void)ops;
  *context = NULL;
  return FS_UNSUPPORTED;
}
#endif

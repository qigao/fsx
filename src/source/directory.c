#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#endif
#include "internal.h"
#include <errno.h>
#include <stdlib.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winternl.h>

wchar_t *fsx_wide(FsEngine *e, const char *text) {
  int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, NULL, 0);
  if (n <= 0)
    return NULL;
  if (n > 32767) {
    SetLastError(ERROR_FILENAME_EXCED_RANGE);
    return NULL;
  }
  wchar_t *w = fsx_alloc(e, ((size_t)n + 8) * sizeof(*w));
  if (!w) {
    SetLastError(ERROR_NOT_ENOUGH_MEMORY);
    return NULL;
  }
  if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, w, n)) {
    fsx_free(w);
    return NULL;
  }
  for (int i = 0; i < n; i++)
    if (w[i] == L'/')
      w[i] = L'\\';
  if (n > 2 && w[1] == L':' && w[2] == L'\\') {
    memmove(w + 4, w, (size_t)n * sizeof(*w));
    memcpy(w, L"\\\\?\\", 4 * sizeof(*w));
  } else if (n > 2 && w[0] == L'\\' && w[1] == L'\\' && w[2] != L'?') {
    memmove(w + 8, w + 2, ((size_t)n - 2) * sizeof(*w));
    memcpy(w, L"\\\\?\\UNC\\", 8 * sizeof(*w));
  }
  return w;
}
char *fsx_utf8_from_wide(FsEngine *e, const wchar_t *w) {
  int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, w, -1, NULL, 0,
                              NULL, NULL);
  if (n <= 0 || n > 65536)
    return NULL;
  char *s = fsx_alloc(e, (size_t)n);
  if (!s) {
    SetLastError(ERROR_NOT_ENOUGH_MEMORY);
    return NULL;
  }
  if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, w, -1, s, n, NULL,
                           NULL)) {
    fsx_free(s);
    return NULL;
  }
  return s;
}
static FsStatus win_status(DWORD error) {
  if (error == ERROR_ACCESS_DENIED || error == ERROR_SHARING_VIOLATION)
    return FS_DENIED;
  if (error == ERROR_NOT_ENOUGH_MEMORY || error == ERROR_OUTOFMEMORY)
    return FS_NOMEM;
  if (error == ERROR_NO_UNICODE_TRANSLATION)
    return FS_UNSUPPORTED;
  if (error == ERROR_FILENAME_EXCED_RANGE)
    return FS_LIMIT;
  return FS_IO;
}
FsStatus fsx_native_root(FsEngine *e, const char *input, char **out) {
  *out = NULL;
  int n =
      MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input, -1, NULL, 0);
  if (n <= 0 || n > 32767)
    return FS_INVALID;
  wchar_t *w = fsx_alloc(e, (size_t)n * sizeof(*w));
  if (!w)
    return FS_NOMEM;
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input, -1, w, n);
  DWORD needed = GetFullPathNameW(w, 0, NULL, NULL);
  if (!needed || needed > 32767) {
    fsx_free(w);
    return FS_INVALID;
  }
  wchar_t *full = fsx_alloc(e, ((size_t)needed + 1) * sizeof(*full));
  if (!full) {
    fsx_free(w);
    return FS_NOMEM;
  }
  DWORD written = GetFullPathNameW(w, needed + 1, full, NULL);
  fsx_free(w);
  if (!written || written > needed) {
    fsx_free(full);
    return FS_IO;
  }
  DWORD attrs = GetFileAttributesW(full);
  if (attrs == INVALID_FILE_ATTRIBUTES) {
    FsStatus s = win_status(GetLastError());
    fsx_free(full);
    return s;
  }
  if (!(attrs & FILE_ATTRIBUTE_DIRECTORY) ||
      attrs & FILE_ATTRIBUTE_REPARSE_POINT) {
    fsx_free(full);
    return FS_UNSUPPORTED;
  }
  HANDLE handle = CreateFileW(
      full, FILE_READ_ATTRIBUTES,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
      OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
      NULL);
  fsx_free(full);
  if (handle == INVALID_HANDLE_VALUE)
    return win_status(GetLastError());
  BY_HANDLE_FILE_INFORMATION root_info;
  if (!GetFileInformationByHandle(handle, &root_info)) {
    FsStatus status = win_status(GetLastError());
    CloseHandle(handle);
    return status;
  }
  if (!(root_info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
      root_info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
    CloseHandle(handle);
    return FS_UNSUPPORTED;
  }
  DWORD required = GetFinalPathNameByHandleW(
      handle, NULL, 0, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
  if (!required || required > 32767) {
    CloseHandle(handle);
    return FS_IO;
  }
  wchar_t *resolved = fsx_alloc(e, ((size_t)required + 1) * sizeof(wchar_t));
  if (!resolved) {
    CloseHandle(handle);
    return FS_NOMEM;
  }
  DWORD final_size = GetFinalPathNameByHandleW(
      handle, resolved, required + 1, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
  DWORD final_error = GetLastError();
  CloseHandle(handle);
  if (!final_size || final_size > required) {
    fsx_free(resolved);
    return win_status(final_error);
  }
  const wchar_t *display = resolved;
  bool unc = wcsncmp(resolved, L"\\\\?\\UNC\\", 8) == 0;
  if (unc) {
    resolved[6] = L'\\';
    display = resolved + 6;
  } else if (!wcsncmp(resolved, L"\\\\?\\", 4))
    display = resolved + 4;
  char *p = fsx_utf8_from_wide(e, display);
  fsx_free(resolved);
  if (!p)
    return win_status(GetLastError());
  for (char *c = p; *c; c++)
    if (*c == '\\')
      *c = '/';
  size_t length = strlen(p);
  while (length > 3 && p[length - 1] == '/')
    p[--length] = 0;
  *out = p;
  return FS_OK;
}
FsStatus fsx_native_volume(FsEngine *e, const char *root, uint64_t *out) {
  wchar_t *w = fsx_wide(e, root);
  if (!w)
    return win_status(GetLastError());
  HANDLE h = CreateFileW(
      w, FILE_READ_ATTRIBUTES,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
      OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
      NULL);
  fsx_free(w);
  if (h == INVALID_HANDLE_VALUE)
    return win_status(GetLastError());
  FILE_ID_INFO info;
  BOOL ok = GetFileInformationByHandleEx(h, FileIdInfo, &info, sizeof(info));
  DWORD err = GetLastError();
  CloseHandle(h);
  if (!ok)
    return win_status(err);
  *out = info.VolumeSerialNumber;
  return FS_OK;
}

static bool skipped_error(DWORD error) {
  return error == ERROR_ACCESS_DENIED || error == ERROR_SHARING_VIOLATION ||
         error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ||
         error == ERROR_DIRECTORY || error == ERROR_DELETE_PENDING;
}
/* ObjectName is one validated component, never an absolute or reconstructed
 * path. Every parent is a live, verified directory handle. */
static HANDLE open_child(HANDLE parent, wchar_t *name, USHORT bytes,
                         bool listing, DWORD *error) {
  UNICODE_STRING component = {bytes, bytes, name};
  OBJECT_ATTRIBUTES attributes;
  memset(&attributes, 0, sizeof(attributes));
  attributes.Length = sizeof(attributes);
  attributes.RootDirectory = parent;
  attributes.ObjectName = &component;
  IO_STATUS_BLOCK io;
  HANDLE file = INVALID_HANDLE_VALUE;
  NTSTATUS status = NtCreateFile(
      &file,
      FILE_READ_ATTRIBUTES | SYNCHRONIZE | (listing ? FILE_LIST_DIRECTORY : 0),
      &attributes, &io, NULL, 0,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, FILE_OPEN,
      FILE_OPEN_REPARSE_POINT | FILE_OPEN_FOR_BACKUP_INTENT |
          FILE_SYNCHRONOUS_IO_NONALERT | (listing ? FILE_DIRECTORY_FILE : 0),
      NULL, 0);
  if (status < 0) {
    *error = RtlNtStatusToDosError(status);
    return INVALID_HANDLE_VALUE;
  }
  return file;
}
static bool same_file(const BY_HANDLE_FILE_INFORMATION *a,
                      const BY_HANDLE_FILE_INFORMATION *b) {
  return a->dwVolumeSerialNumber == b->dwVolumeSerialNumber &&
         a->nFileIndexHigh == b->nFileIndexHigh &&
         a->nFileIndexLow == b->nFileIndexLow;
}
static FsStatus walk_dir(FsWalk *, HANDLE, const char *, unsigned);
static FsStatus visit(FsWalk *walk, HANDLE parent, const char *root,
                      const FILE_ID_BOTH_DIR_INFO *record, unsigned depth) {
  FsEngine *e = walk->engine;
  wchar_t *raw = fsx_alloc(e, (size_t)record->FileNameLength + sizeof(wchar_t));
  if (!raw)
    return FS_NOMEM;
  memcpy(raw, record->FileName, record->FileNameLength);
  raw[record->FileNameLength / sizeof(wchar_t)] = 0;
  char *name = fsx_utf8_from_wide(e, raw), *path = NULL;
  HANDLE file = INVALID_HANDLE_VALUE, listing = INVALID_HANDLE_VALUE;
  FsStatus status = FS_OK;
  if (!name) {
    DWORD error = GetLastError();
    if (error == ERROR_NO_UNICODE_TRANSLATION) {
      walk->skipped++;
      walk->unsupported++;
    } else
      status = win_status(error);
    goto done;
  }
  size_t length = strlen(root), bytes = strlen(name);
  if (length > 65534 || bytes > 65534 - length) {
    status = FS_LIMIT;
    goto done;
  }
  bool separator = length && root[length - 1] != '/';
  path = fsx_alloc(e, length + bytes + 2);
  if (!path) {
    status = FS_NOMEM;
    goto done;
  }
  memcpy(path, root, length);
  if (separator)
    path[length] = '/';
  memcpy(path + length + (separator ? 1 : 0), name, bytes + 1);
  DWORD error = 0;
  file = open_child(parent, raw, (USHORT)record->FileNameLength, false, &error);
  if (file == INVALID_HANDLE_VALUE) {
    if (skipped_error(error))
      walk->skipped++;
    else
      status = win_status(error);
    goto done;
  }
  BY_HANDLE_FILE_INFORMATION info;
  if (!GetFileInformationByHandle(file, &info)) {
    status = win_status(GetLastError());
    goto done;
  }
  bool directory = (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
  bool reparse = (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
  if (directory && !reparse) {
    listing =
        open_child(parent, raw, (USHORT)record->FileNameLength, true, &error);
    if (listing == INVALID_HANDLE_VALUE) {
      if (skipped_error(error))
        walk->skipped++;
      else {
        status = win_status(error);
        goto done;
      }
    } else {
      BY_HANDLE_FILE_INFORMATION checked;
      if (!GetFileInformationByHandle(listing, &checked)) {
        status = win_status(GetLastError());
        goto done;
      }
      if (!same_file(&info, &checked) ||
          !(checked.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
          checked.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
        /* A changed namespace must not switch the object being descended. */
        walk->skipped++;
        CloseHandle(listing);
        listing = INVALID_HANDLE_VALUE;
      }
    }
  }
  FsObserved observed = {0};
  observed.name = name;
  observed.path = path;
  observed.raw_name = raw;
  observed.raw_bytes = record->FileNameLength;
  observed.file_lo = ((uint64_t)info.nFileIndexHigh << 32) | info.nFileIndexLow;
  observed.size = ((uint64_t)info.nFileSizeHigh << 32) | info.nFileSizeLow;
  observed.mtime = ((uint64_t)info.ftLastWriteTime.dwHighDateTime << 32) |
                   info.ftLastWriteTime.dwLowDateTime;
  observed.flags = FS_ENTRY_MTIME_VALID |
                   (directory ? FS_ENTRY_DIRECTORY : FS_ENTRY_SIZE_VALID) |
                   (reparse ? FS_ENTRY_LINK : 0);
  status = walk->observe(walk->context, &observed);
  /* Hold the verified listing handle through the callback and recursion.
   * Namespace replacement after observation cannot redirect this descent. */
  if (status == FS_OK && listing != INVALID_HANDLE_VALUE)
    status = walk_dir(walk, listing, path, depth + 1);
done:
  if (listing != INVALID_HANDLE_VALUE)
    CloseHandle(listing);
  if (file != INVALID_HANDLE_VALUE)
    CloseHandle(file);
  fsx_free(path);
  fsx_free(name);
  fsx_free(raw);
  return status;
}
static FsStatus walk_dir(FsWalk *walk, HANDLE directory, const char *root,
                         unsigned depth) {
  if (fsx_writer_stopping(walk->engine))
    return FS_CANCELLED;
  if (depth >= walk->engine->config.max_scan_depth)
    return FS_LIMIT;
  enum { BUFFER_BYTES = 4096 };
  unsigned char *buffer = fsx_alloc(walk->engine, BUFFER_BYTES);
  if (!buffer)
    return FS_NOMEM;
  FsStatus status = FS_OK;
  bool first = true;
  for (;;) {
    if (fsx_writer_stopping(walk->engine)) {
      status = FS_CANCELLED;
      break;
    }
    memset(buffer, 0, BUFFER_BYTES);
    BOOL read = GetFileInformationByHandleEx(
        directory,
        first ? FileIdBothDirectoryRestartInfo : FileIdBothDirectoryInfo,
        buffer, BUFFER_BYTES);
    first = false;
    if (!read) {
      DWORD error = GetLastError();
      if (error == ERROR_NO_MORE_FILES)
        break;
      if (skipped_error(error))
        walk->skipped++;
      else
        status = win_status(error);
      break;
    }
    size_t offset = 0, fixed = offsetof(FILE_ID_BOTH_DIR_INFO, FileName);
    for (;;) {
      if (BUFFER_BYTES - offset < fixed) {
        status = FS_CORRUPT;
        break;
      }
      const FILE_ID_BOTH_DIR_INFO *record = (const void *)(buffer + offset);
      size_t bytes = record->FileNameLength;
      if (!bytes || bytes % sizeof(wchar_t) || bytes > USHRT_MAX ||
          bytes > BUFFER_BYTES - offset - fixed ||
          (record->NextEntryOffset &&
           (record->NextEntryOffset % 8 ||
            record->NextEntryOffset < fixed + bytes ||
            record->NextEntryOffset > BUFFER_BYTES - offset - fixed))) {
        status = FS_CORRUPT;
        break;
      }
      size_t units = bytes / sizeof(wchar_t);
      bool dot = units == 1 && record->FileName[0] == L'.';
      bool dotdot = units == 2 && record->FileName[0] == L'.' &&
                    record->FileName[1] == L'.';
      bool component = true;
      for (size_t i = 0; i < units; i++)
        if (!record->FileName[i] || record->FileName[i] == L'/' ||
            record->FileName[i] == L'\\' || record->FileName[i] == L':')
          component = false;
      if (!component) {
        walk->skipped++;
        walk->unsupported++;
      } else if (!dot && !dotdot)
        status = visit(walk, directory, root, record, depth);
      if (status != FS_OK || !record->NextEntryOffset)
        break;
      offset += record->NextEntryOffset;
    }
    if (status != FS_OK)
      break;
    /* A zero NextEntryOffset ends this batch, not the directory. */
  }
  fsx_free(buffer);
  return status;
}
static FsStatus walk_root(FsWalk *walk, const char *root) {
  wchar_t *path = fsx_wide(walk->engine, root);
  if (!path)
    return win_status(GetLastError());
  HANDLE directory = CreateFileW(
      path, FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
      OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
      NULL);
  DWORD error = GetLastError();
  fsx_free(path);
  if (directory == INVALID_HANDLE_VALUE) {
    if (skipped_error(error)) {
      walk->skipped++;
      return FS_OK;
    }
    return win_status(error);
  }
  BY_HANDLE_FILE_INFORMATION info;
  FsStatus status;
  if (!GetFileInformationByHandle(directory, &info))
    status = win_status(GetLastError());
  else if (!(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
           info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
    status = FS_UNSUPPORTED;
  else
    status = walk_dir(walk, directory, root, 0);
  CloseHandle(directory);
  return status;
}

#else
#include "../platform_stat.h"
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
static FsStatus posix_status(int err) {
  return err == EACCES || err == EPERM ? FS_DENIED
                                       : (err == ENOMEM ? FS_NOMEM : FS_IO);
}
static bool posix_skipped(int error) {
  return error == EACCES || error == EPERM || error == ENOENT ||
         error == ENOTDIR || error == ELOOP;
}
FsStatus fsx_native_root(FsEngine *e, const char *input, char **out) {
  *out = NULL;
  char *real = realpath(input, NULL);
  if (!real)
    return posix_status(errno);
  struct stat st;
  FsStatus status = FS_OK;
  if (lstat(real, &st))
    status = posix_status(errno);
  else if (!S_ISDIR(st.st_mode))
    status = FS_UNSUPPORTED;
  else if (!fsx_utf8(real, strlen(real)))
    status = FS_UNSUPPORTED;
  else {
    *out = fsx_copy(e, real, strlen(real));
    if (!*out)
      status = FS_NOMEM;
  }
  free(real);
  return status;
}
FsStatus fsx_native_volume(FsEngine *e, const char *root, uint64_t *out) {
  (void)e;
  struct stat st;
  if (stat(root, &st))
    return posix_status(errno);
  *out = (uint64_t)st.st_dev;
  return FS_OK;
}

static FsStatus walk_dir(FsWalk *walk, int descriptor, const char *root,
                         unsigned depth) {
  FsEngine *e = walk->engine;
  if (fsx_writer_stopping(e)) {
    close(descriptor);
    return FS_CANCELLED;
  }
  if (depth >= e->config.max_scan_depth) {
    close(descriptor);
    return FS_LIMIT;
  }
  DIR *dir = fdopendir(descriptor);
  if (!dir) {
    int error = errno;
    close(descriptor);
    return posix_status(error);
  }
  FsStatus status = FS_OK;
  if (walk->hooks) {
    status = walk->hooks->directory(walk->hooks->context, descriptor, depth);
    if (status != FS_OK) {
      closedir(dir);
      return status;
    }
  }
  unsigned visited = 0;
  for (;;) {
    if (walk->hooks && visited++ % 256 == 0) {
      status = walk->hooks->checkpoint(walk->hooks->context);
      if (status != FS_OK)
        break;
    }
    errno = 0;
    struct dirent *d = readdir(dir);
    if (!d) {
      if (errno)
        status = posix_status(errno);
      break;
    }
    if (!strcmp(d->d_name, ".") || !strcmp(d->d_name, ".."))
      continue;
    if (fsx_writer_stopping(e)) {
      status = FS_CANCELLED;
      break;
    }
    size_t a = strlen(root), b = strlen(d->d_name);
    if (!fsx_utf8(d->d_name, b)) {
      walk->skipped++;
      walk->unsupported++;
      continue;
    }
    if (a > 65534 || b > 65534 - a) {
      status = FS_LIMIT;
      break;
    }
    char *path = fsx_alloc(e, a + b + 2);
    if (!path) {
      status = FS_NOMEM;
      break;
    }
    memcpy(path, root, a);
    bool separator = a && root[a - 1] != '/';
    if (separator)
      path[a] = '/';
    memcpy(path + a + (separator ? 1 : 0), d->d_name, b + 1);
    struct stat st;
    if (fstatat(descriptor, d->d_name, &st, AT_SYMLINK_NOFOLLOW)) {
      if (posix_skipped(errno))
        walk->skipped++;
      else
        status = posix_status(errno);
      fsx_free(path);
      if (status != FS_OK)
        break;
      continue;
    }
    int child = -1;
    if (S_ISDIR(st.st_mode)) {
      child = openat(descriptor, d->d_name,
                     O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
      if (child < 0) {
        if (posix_skipped(errno))
          walk->skipped++;
        else
          status = posix_status(errno);
      } else {
        struct stat checked;
        if (fstat(child, &checked)) {
          status = posix_status(errno);
          close(child);
          child = -1;
        } else if (!S_ISDIR(checked.st_mode) || checked.st_dev != st.st_dev ||
                   checked.st_ino != st.st_ino) {
          walk->skipped++;
          close(child);
          child = -1;
        }
      }
    }
    if (status == FS_OK) {
      FsObserved o = {0};
      o.name = d->d_name;
      o.path = path;
      o.raw_name = d->d_name;
      o.raw_bytes = b;
      o.file_lo = (uint64_t)st.st_ino;
      o.file_hi = (uint64_t)st.st_dev;
      o.size = st.st_size >= 0 ? (uint64_t)st.st_size : 0;
      o.flags = FS_ENTRY_MTIME_VALID;
      int64_t sec = (int64_t)fsx_stat_mtime(&st).tv_sec;
      if (sec >= -11644473600LL && sec <= 1728223453709LL)
        o.mtime = (uint64_t)(sec + 11644473600LL) * 10000000ULL +
                  (uint64_t)fsx_stat_mtime(&st).tv_nsec / 100;
      else
        o.flags = 0;
      if (S_ISDIR(st.st_mode))
        o.flags |= FS_ENTRY_DIRECTORY;
      else if (st.st_size >= 0)
        o.flags |= FS_ENTRY_SIZE_VALID;
      if (S_ISLNK(st.st_mode))
        o.flags |= FS_ENTRY_LINK;
      status = walk->observe(walk->context, &o);
      if (status == FS_OK && child >= 0) {
        status = walk_dir(walk, child, path, depth + 1);
        child = -1; /* walk_dir consumes the descriptor on every path. */
      }
    }
    if (child >= 0)
      close(child);
    fsx_free(path);
    if (status != FS_OK)
      break;
  }
  closedir(dir);
  return status;
}
static FsStatus walk_root(FsWalk *walk, const char *root) {
  int descriptor = open(root, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (descriptor < 0) {
    if (errno == EACCES || errno == ENOENT) {
      walk->skipped++;
      return FS_OK;
    }
    return posix_status(errno);
  }
  return walk_dir(walk, descriptor, root, 0);
}

#endif
FsStatus fsx_walk(FsWalk *walk, const char *root) {
  return walk_root(walk, root);
}

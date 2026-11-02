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
  BY_HANDLE_FILE_INFORMATION info;
  BOOL ok = GetFileInformationByHandle(h, &info);
  DWORD err = GetLastError();
  CloseHandle(h);
  if (!ok)
    return win_status(err);
  *out = info.dwVolumeSerialNumber;
  return FS_OK;
}
static FsStatus walk_dir(FsWalk *walk, const char *root, unsigned depth) {
  if (atomic_load(&walk->engine->stopping))
    return FS_CANCELLED;
  if (depth >= walk->engine->config.max_scan_depth)
    return FS_LIMIT;
  FsEngine *e = walk->engine;
  size_t len = strlen(root);
  if (len > 65530)
    return FS_LIMIT;
  char *pattern = fsx_alloc(e, len + 3);
  if (!pattern)
    return FS_NOMEM;
  memcpy(pattern, root, len);
  bool separator = len && root[len - 1] != '/';
  memcpy(pattern + len, separator ? "/*" : "*", separator ? 3 : 2);
  wchar_t *wide_pattern = fsx_wide(e, pattern);
  fsx_free(pattern);
  if (!wide_pattern)
    return win_status(GetLastError());
  WIN32_FIND_DATAW data;
  HANDLE find =
      FindFirstFileExW(wide_pattern, FindExInfoBasic, &data,
                       FindExSearchNameMatch, NULL, FIND_FIRST_EX_LARGE_FETCH);
  DWORD err = GetLastError();
  fsx_free(wide_pattern);
  if (find == INVALID_HANDLE_VALUE) {
    if (err == ERROR_FILE_NOT_FOUND)
      return FS_OK;
    if (err == ERROR_ACCESS_DENIED || err == ERROR_PATH_NOT_FOUND) {
      walk->skipped++;
      return FS_OK;
    }
    return win_status(err);
  }
  FsStatus status = FS_OK;
  for (;;) {
    if (atomic_load(&e->stopping)) {
      status = FS_CANCELLED;
      break;
    }
    if (wcscmp(data.cFileName, L".") && wcscmp(data.cFileName, L"..")) {
      char *name = fsx_utf8_from_wide(e, data.cFileName);
      if (!name) {
        if (GetLastError() == ERROR_NO_UNICODE_TRANSLATION) {
          walk->skipped++;
          walk->unsupported++;
        } else {
          status = win_status(GetLastError());
          break;
        }
      } else {
        size_t n = strlen(name);
        if (n > 65535 - len - 1) {
          fsx_free(name);
          status = FS_LIMIT;
          break;
        }
        char *path = fsx_alloc(e, len + n + 2);
        if (!path) {
          fsx_free(name);
          status = FS_NOMEM;
          break;
        }
        memcpy(path, root, len);
        if (separator)
          path[len] = '/';
        memcpy(path + len + (separator ? 1 : 0), name, n + 1);
        wchar_t *wide_path = fsx_wide(e, path);
        if (!wide_path) {
          fsx_free(name);
          fsx_free(path);
          status = win_status(GetLastError());
          break;
        }
        HANDLE file = CreateFileW(
            wide_path, FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
            OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
        fsx_free(wide_path);
        if (file == INVALID_HANDLE_VALUE) {
          err = GetLastError();
          if (err == ERROR_ACCESS_DENIED || err == ERROR_SHARING_VIOLATION ||
              err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND)
            walk->skipped++;
          else
            status = win_status(err);
        } else {
          BY_HANDLE_FILE_INFORMATION info;
          if (!GetFileInformationByHandle(file, &info))
            status = win_status(GetLastError());
          else {
            FsObserved o = {0};
            o.name = name;
            o.path = path;
            o.raw_name = data.cFileName;
            o.raw_bytes = wcslen(data.cFileName) * sizeof(wchar_t);
            o.file_lo =
                ((uint64_t)info.nFileIndexHigh << 32) | info.nFileIndexLow;
            o.file_hi = 0;
            o.size = ((uint64_t)info.nFileSizeHigh << 32) | info.nFileSizeLow;
            o.mtime = ((uint64_t)info.ftLastWriteTime.dwHighDateTime << 32) |
                      info.ftLastWriteTime.dwLowDateTime;
            o.flags = FS_ENTRY_MTIME_VALID;
            if (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
              o.flags |= FS_ENTRY_DIRECTORY;
            else
              o.flags |= FS_ENTRY_SIZE_VALID;
            if (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
              o.flags |= FS_ENTRY_LINK;
            status = walk->observe(walk->context, &o);
            /* Do not recurse while retaining the metadata handle. */
          }
          CloseHandle(file);
          if (status == FS_OK &&
              data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY &&
              !(data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
            status = walk_dir(walk, path, depth + 1);
        }
        fsx_free(path);
        fsx_free(name);
        if (status != FS_OK)
          break;
      }
    }
    if (!FindNextFileW(find, &data)) {
      err = GetLastError();
      if (err != ERROR_NO_MORE_FILES)
        status = win_status(err);
      break;
    }
  }
  FindClose(find);
  return status;
}
#else
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
static FsStatus posix_status(int err) {
  return err == EACCES || err == EPERM ? FS_DENIED
                                       : (err == ENOMEM ? FS_NOMEM : FS_IO);
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
static FsStatus walk_dir(FsWalk *walk, const char *root, unsigned depth) {
  FsEngine *e = walk->engine;
  if (atomic_load(&e->stopping))
    return FS_CANCELLED;
  if (depth >= e->config.max_scan_depth)
    return FS_LIMIT;
  DIR *dir = opendir(root);
  if (!dir) {
    if (errno == EACCES || errno == ENOENT) {
      walk->skipped++;
      return FS_OK;
    }
    return posix_status(errno);
  }
  FsStatus status = FS_OK;
  for (;;) {
    errno = 0;
    struct dirent *d = readdir(dir);
    if (!d) {
      if (errno)
        status = posix_status(errno);
      break;
    }
    if (!strcmp(d->d_name, ".") || !strcmp(d->d_name, ".."))
      continue;
    if (atomic_load(&e->stopping)) {
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
    if (lstat(path, &st)) {
      walk->skipped++;
      fsx_free(path);
      continue;
    }
    FsObserved o = {0};
    o.name = d->d_name;
    o.path = path;
    o.raw_name = d->d_name;
    o.raw_bytes = b;
    o.file_lo = (uint64_t)st.st_ino;
    o.file_hi = (uint64_t)st.st_dev;
    o.size = st.st_size >= 0 ? (uint64_t)st.st_size : 0;
    o.flags = FS_ENTRY_MTIME_VALID;
    int64_t sec = (int64_t)st.st_mtim.tv_sec;
    if (sec >= -11644473600LL && sec <= 1728223453709LL)
      o.mtime = (uint64_t)(sec + 11644473600LL) * 10000000ULL +
                (uint64_t)st.st_mtim.tv_nsec / 100;
    else
      o.flags = 0;
    if (S_ISDIR(st.st_mode))
      o.flags |= FS_ENTRY_DIRECTORY;
    else if (st.st_size >= 0)
      o.flags |= FS_ENTRY_SIZE_VALID;
    if (S_ISLNK(st.st_mode))
      o.flags |= FS_ENTRY_LINK;
    status = walk->observe(walk->context, &o);
    if (status == FS_OK && S_ISDIR(st.st_mode))
      status = walk_dir(walk, path, depth + 1);
    fsx_free(path);
    if (status != FS_OK)
      break;
  }
  closedir(dir);
  return status;
}
#endif
FsStatus fsx_walk(FsWalk *walk, const char *root) {
  return walk_dir(walk, root, 0);
}

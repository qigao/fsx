#define WIN32_LEAN_AND_MEAN
#include "content.h"
#include <stdio.h>
#include <windows.h>
#include <winternl.h>

static FsStatus native_status(DWORD error) {
  if (error == ERROR_ACCESS_DENIED || error == ERROR_SHARING_VIOLATION)
    return FS_DENIED;
  if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ||
      error == ERROR_DELETE_PENDING || error == ERROR_DIRECTORY)
    return FS_STALE;
  if (error == ERROR_NOT_ENOUGH_MEMORY || error == ERROR_OUTOFMEMORY)
    return FS_NOMEM;
  return FS_IO;
}
static FsStatus report(FsContentQuery *q, const char *phase, DWORD error) {
  cmeta_mutex_lock(&q->mutex);
  if (!q->info.diagnostic[0])
    snprintf(q->info.diagnostic, sizeof(q->info.diagnostic), "Windows %s: %lu",
             phase, (unsigned long)error);
  cmeta_mutex_unlock(&q->mutex);
  return native_status(error);
}
static FsStatus file_info(FsContentQuery *q, HANDLE file,
                          BY_HANDLE_FILE_INFORMATION *id,
                          FILE_BASIC_INFO *basic, FILE_ID_INFO *volume) {
  if (!GetFileInformationByHandle(file, id) ||
      !GetFileInformationByHandleEx(file, FileBasicInfo, basic,
                                    sizeof(*basic)) ||
      !GetFileInformationByHandleEx(file, FileIdInfo, volume, sizeof(*volume)))
    return report(q, "file information", GetLastError());
  if (id->dwFileAttributes &
      (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_OFFLINE |
       FILE_ATTRIBUTE_RECALL_ON_OPEN | FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS))
    return FS_UNSUPPORTED;
  return FS_OK;
}
static FsStatus open_file(FsContentQuery *q, const FsEntry *entry, HANDLE *out,
                          BY_HANDLE_FILE_INFORMATION *id,
                          FILE_BASIC_INFO *basic, FILE_ID_INFO *volume) {
  wchar_t *path = fsx_wide(q->engine, entry->path);
  if (!path)
    return report(q, "path conversion", GetLastError());
  wchar_t *component;
  if (!wcsncmp(path, L"\\\\?\\UNC\\", 8)) {
    component = wcschr(path + 8, L'\\');
    component = component ? wcschr(component + 1, L'\\') : NULL;
    if (component)
      component++;
  } else
    component = wcslen(path) > 7 && path[5] == L':' ? path + 7 : NULL;
  if (!component) {
    fsx_free(path);
    return FS_UNSUPPORTED;
  }
  wchar_t saved = *component;
  *component = 0;
  HANDLE parent = CreateFileW(
      path, FILE_READ_ATTRIBUTES | FILE_TRAVERSE,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
      OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
      NULL);
  *component = saved;
  FsStatus status = parent == INVALID_HANDLE_VALUE
                        ? report(q, "root open", GetLastError())
                        : FS_OK;
  while (status == FS_OK) {
    BY_HANDLE_FILE_INFORMATION directory;
    if (!GetFileInformationByHandle(parent, &directory)) {
      status = report(q, "directory information", GetLastError());
      break;
    }
    if (!(directory.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
        directory.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
      status = FS_UNSUPPORTED;
      break;
    }
    wchar_t *end = wcschr(component, L'\\');
    size_t length = end ? (size_t)(end - component) : wcslen(component);
    if (!length || length > 32767 || (length == 1 && component[0] == L'.') ||
        (length == 2 && component[0] == L'.' && component[1] == L'.') ||
        wmemchr(component, L':', length)) {
      status = FS_UNSUPPORTED;
      break;
    }
    UNICODE_STRING name = {(USHORT)(length * sizeof(wchar_t)),
                           (USHORT)(length * sizeof(wchar_t)), component};
    OBJECT_ATTRIBUTES attributes = {0};
    attributes.Length = sizeof(attributes);
    attributes.RootDirectory = parent;
    attributes.ObjectName = &name;
    IO_STATUS_BLOCK io;
    HANDLE next = INVALID_HANDLE_VALUE;
    NTSTATUS rc = NtCreateFile(
        &next,
        FILE_READ_ATTRIBUTES | SYNCHRONIZE |
            (end ? FILE_TRAVERSE : FILE_READ_DATA),
        &attributes, &io, NULL, 0,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, FILE_OPEN,
        FILE_SYNCHRONOUS_IO_NONALERT | FILE_OPEN_REPARSE_POINT |
            (end ? FILE_DIRECTORY_FILE
                 : FILE_NON_DIRECTORY_FILE | FILE_OPEN_NO_RECALL),
        NULL, 0);
    CloseHandle(parent);
    parent = next;
    if (rc < 0) {
      status = report(q, "relative open", RtlNtStatusToDosError(rc));
      break;
    }
    if (end) {
      component = end + 1;
      continue;
    }
    status = file_info(q, parent, id, basic, volume);
    if (status == FS_OK) {
      uint64_t expected = 0;
      bool found = false;
      for (uint32_t i = 0; i < q->snapshot->sources; i++)
        if (q->snapshot->identities[i].id == entry->key.source_id) {
          expected = q->snapshot->identities[i].volume;
          found = true;
          break;
        }
      uint64_t index = ((uint64_t)id->nFileIndexHigh << 32) | id->nFileIndexLow;
      if (!found)
        status = FS_CORRUPT;
      else if (index != entry->key.file_id_lo || entry->key.file_id_hi ||
               volume->VolumeSerialNumber != expected)
        status = FS_STALE;
    }
    break;
  }
  fsx_free(path);
  if (status == FS_OK)
    *out = parent;
  else if (parent != INVALID_HANDLE_VALUE)
    CloseHandle(parent);
  return status;
}
static bool same_file(const BY_HANDLE_FILE_INFORMATION *a,
                      const FILE_BASIC_INFO *ab, const FILE_ID_INFO *av,
                      const BY_HANDLE_FILE_INFORMATION *b,
                      const FILE_BASIC_INFO *bb, const FILE_ID_INFO *bv) {
  return av->VolumeSerialNumber == bv->VolumeSerialNumber &&
         !memcmp(&av->FileId, &bv->FileId, sizeof(av->FileId)) &&
         a->nFileSizeHigh == b->nFileSizeHigh &&
         a->nFileSizeLow == b->nFileSizeLow &&
         ab->LastWriteTime.QuadPart == bb->LastWriteTime.QuadPart &&
         ab->ChangeTime.QuadPart == bb->ChangeTime.QuadPart;
}
typedef struct Pipe {
  HANDLE handle, child;
  OVERLAPPED io;
  bool pending;
  unsigned char bytes[8192];
} Pipe;
static atomic_uint pipe_sequence;
static FsStatus make_pipe(Pipe *p, unsigned index) {
  wchar_t name[160];
  swprintf(name, 160, L"\\\\.\\pipe\\fsx-content-%lu-%u-%u",
           (unsigned long)GetCurrentProcessId(),
           atomic_fetch_add(&pipe_sequence, 1), index);
  p->io.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
  if (!p->io.hEvent)
    return native_status(GetLastError());
  p->handle =
      CreateNamedPipeW(name,
                       (index ? PIPE_ACCESS_INBOUND : PIPE_ACCESS_OUTBOUND) |
                           FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
                       PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT |
                           PIPE_REJECT_REMOTE_CLIENTS,
                       1, 65536, 65536, 0, NULL);
  if (p->handle == INVALID_HANDLE_VALUE)
    return native_status(GetLastError());
  SECURITY_ATTRIBUTES inherit = {sizeof(inherit), NULL, TRUE};
  p->child = CreateFileW(name, index ? GENERIC_WRITE : GENERIC_READ, 0,
                         &inherit, OPEN_EXISTING, 0, NULL);
  if (p->child == INVALID_HANDLE_VALUE)
    return native_status(GetLastError());
  if (!ConnectNamedPipe(p->handle, &p->io) &&
      GetLastError() != ERROR_PIPE_CONNECTED)
    return native_status(GetLastError());
  return FS_OK;
}
static wchar_t *wide_text(FsEngine *e, const char *text) {
  int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, NULL, 0);
  if (!n)
    return NULL;
  wchar_t *wide = fsx_alloc(e, (size_t)n * sizeof(*wide));
  if (wide)
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, wide, n);
  return wide;
}
/* Quote every argv element using the Windows CRT backslash/quote rules. */
static FsStatus command_line(FsContentQuery *q, wchar_t **out) {
  const char *args[] = {q->tool,
                        "-J1",
                        q->request.mode == FS_CONTENT_LITERAL ? "-F" : "-P",
                        "--color=never",
                        "--line-buffered",
                        "--label=fsx-input",
                        "--format={\"line\":%n,\"offset\":%b,\"text\":%J}%~",
                        "-e",
                        q->pattern,
                        "--",
                        "-"};
  wchar_t *line = fsx_alloc(q->engine, 32768 * sizeof(*line));
  if (!line)
    return FS_NOMEM;
  size_t used = 0;
  for (unsigned i = 0; i < sizeof(args) / sizeof(*args); i++) {
    wchar_t *arg = wide_text(q->engine, args[i]);
    if (!arg) {
      fsx_free(line);
      return FS_NOMEM;
    }
    if (i)
      line[used++] = L' ';
    line[used++] = L'"';
    size_t at = 0;
    do {
      size_t slashes = 0;
      while (arg[at] == L'\\') {
        slashes++;
        at++;
      }
      size_t emit = arg[at] == L'"' || !arg[at] ? slashes * 2 : slashes;
      if (used + emit + 4 >= 32768) {
        fsx_free(arg);
        fsx_free(line);
        return FS_LIMIT;
      }
      while (emit--)
        line[used++] = L'\\';
      if (arg[at] == L'"')
        line[used++] = L'\\';
      if (!arg[at])
        break;
      line[used++] = arg[at++];
    } while (true);
    line[used++] = L'"';
    fsx_free(arg);
  }
  line[used] = 0;
  *out = line;
  return FS_OK;
}
static FsStatus output_bytes(FsContentQuery *q, FsContentBatch *b,
                             unsigned index, const unsigned char *bytes,
                             DWORD n, size_t *diagnostic) {
  if (index == 1) {
    if (n > q->request.max_output_bytes - b->used)
      return FS_LIMIT;
    memcpy(b->wire + b->used, bytes, n);
    b->used += n;
  } else {
    size_t copy = sizeof(b->diagnostic) - 1;
    if (*diagnostic < copy) {
      copy -= *diagnostic;
      if (copy > n)
        copy = n;
      memcpy(b->diagnostic + *diagnostic, bytes, copy);
      b->diagnostic[*diagnostic + copy] = 0;
    }
    *diagnostic += n;
    if (*diagnostic > 65536)
      return FS_LIMIT;
  }
  return FS_OK;
}
static FsStatus execute(FsContentQuery *q, HANDLE file, FsContentBatch *b) {
  Pipe pipes[3] = {0};
  for (unsigned i = 0; i < 3; i++)
    pipes[i].handle = pipes[i].child = INVALID_HANDLE_VALUE;
  HANDLE job = NULL;
  PROCESS_INFORMATION child = {0};
  STARTUPINFOEXW startup = {0};
  bool attributes_ready = false, counted = false;
  wchar_t *line = NULL, *tool = NULL;
  unsigned char *input = NULL;
  FsStatus status = command_line(q, &line);
  if (status != FS_OK)
    goto cleanup;
  tool = fsx_wide(q->engine, q->tool);
  input = fsx_alloc(q->engine, 65536);
  if (!tool || !input) {
    status = FS_NOMEM;
    goto cleanup;
  }
  for (unsigned i = 0; i < 3; i++) {
    status = make_pipe(&pipes[i], i);
    if (status != FS_OK)
      goto cleanup;
  }
  job = CreateJobObjectW(NULL, NULL);
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {0};
  limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
  if (!job || !SetInformationJobObject(job, JobObjectExtendedLimitInformation,
                                       &limits, sizeof(limits))) {
    status = native_status(GetLastError());
    goto cleanup;
  }
  SIZE_T bytes = 0;
  InitializeProcThreadAttributeList(NULL, 2, 0, &bytes);
  startup.lpAttributeList = fsx_alloc(q->engine, bytes);
  if (!startup.lpAttributeList) {
    status = FS_NOMEM;
    goto cleanup;
  }
  if (!InitializeProcThreadAttributeList(startup.lpAttributeList, 2, 0,
                                         &bytes)) {
    status = native_status(GetLastError());
    goto cleanup;
  }
  attributes_ready = true;
  HANDLE handles[] = {pipes[0].child, pipes[1].child, pipes[2].child};
  if (!UpdateProcThreadAttribute(startup.lpAttributeList, 0,
                                 PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handles,
                                 sizeof(handles), NULL, NULL) ||
      !UpdateProcThreadAttribute(startup.lpAttributeList, 0,
                                 PROC_THREAD_ATTRIBUTE_JOB_LIST, &job,
                                 sizeof(job), NULL, NULL)) {
    status = native_status(GetLastError());
    goto cleanup;
  }
  startup.StartupInfo.cb = sizeof(startup);
  startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
  startup.StartupInfo.hStdInput = handles[0];
  startup.StartupInfo.hStdOutput = handles[1];
  startup.StartupInfo.hStdError = handles[2];
  wchar_t environment[320] = L"LANG=C\0LC_ALL=C\0SystemRoot=";
  const size_t prefix =
      sizeof(L"LANG=C\0LC_ALL=C\0SystemRoot=") / sizeof(wchar_t) - 1;
  UINT n = GetWindowsDirectoryW(environment + prefix, 320 - (UINT)prefix - 1);
  if (!n || n >= 320 - prefix - 1) {
    status = FS_LIMIT;
    goto cleanup;
  }
  environment[prefix + n + 1] = 0;
  if (!CreateProcessW(tool, line, NULL, NULL, TRUE,
                      CREATE_SUSPENDED | CREATE_NO_WINDOW |
                          CREATE_UNICODE_ENVIRONMENT |
                          EXTENDED_STARTUPINFO_PRESENT,
                      environment, NULL, &startup.StartupInfo, &child)) {
    status = FS_IO;
    goto cleanup;
  }
  cmeta_mutex_lock(&q->mutex);
  q->info.processes_started++;
  q->info.running_processes++;
  counted = true;
  cmeta_mutex_unlock(&q->mutex);
  for (unsigned i = 0; i < 3; i++) {
    CloseHandle(pipes[i].child);
    pipes[i].child = INVALID_HANDLE_VALUE;
  }
  if (ResumeThread(child.hThread) == (DWORD)-1) {
    status = FS_IO;
    goto cleanup;
  }
  size_t used = 0, sent = 0, diagnostic = 0;
  uint64_t read_bytes = 0;
  bool first = true;
  FsTextState utf8 = {0};
  while (status == FS_OK && (pipes[0].handle != INVALID_HANDLE_VALUE ||
                             pipes[1].handle != INVALID_HANDLE_VALUE ||
                             pipes[2].handle != INVALID_HANDLE_VALUE)) {
    status = fsx_content_stop(q);
    if (status != FS_OK)
      break;
    if (pipes[0].handle != INVALID_HANDLE_VALUE && !pipes[0].pending &&
        sent == used) {
      DWORD read = 0;
      if (!ReadFile(file, input, 65536, &read, NULL)) {
        status = native_status(GetLastError());
        break;
      }
      if (!read) {
        if (utf8.remaining) {
          status = FS_UNSUPPORTED;
          break;
        }
        CloseHandle(pipes[0].handle);
        pipes[0].handle = INVALID_HANDLE_VALUE;
      } else {
        read_bytes += read;
        if (read_bytes > q->request.max_file_bytes) {
          status = FS_LIMIT;
          break;
        }
        used = read;
        sent = first && used >= 3 && !memcmp(input, "\xef\xbb\xbf", 3) ? 3 : 0;
        first = false;
        if (!fsx_content_text(&utf8, input + sent, used - sent)) {
          status = FS_UNSUPPORTED;
          break;
        }
      }
    }
    bool progressed = false;
    HANDLE events[3];
    DWORD count = 0;
    for (unsigned i = 0; i < 3 && status == FS_OK; i++) {
      Pipe *p = &pipes[i];
      if (p->handle == INVALID_HANDLE_VALUE)
        continue;
      DWORD transferred = 0;
      BOOL complete;
      if (p->pending)
        complete = GetOverlappedResult(p->handle, &p->io, &transferred, FALSE);
      else {
        ResetEvent(p->io.hEvent);
        complete = i ? ReadFile(p->handle, p->bytes, sizeof(p->bytes),
                                &transferred, &p->io)
                     : WriteFile(p->handle, input + sent, (DWORD)(used - sent),
                                 &transferred, &p->io);
      }
      if (!complete) {
        DWORD error = GetLastError();
        if (error == ERROR_IO_PENDING || error == ERROR_IO_INCOMPLETE) {
          p->pending = true;
          events[count++] = p->io.hEvent;
          continue;
        }
        p->pending = false;
        if (i && error == ERROR_BROKEN_PIPE) {
          CloseHandle(p->handle);
          p->handle = INVALID_HANDLE_VALUE;
          continue;
        }
        status = FS_IO;
        break;
      }
      p->pending = false;
      progressed = true;
      if (!i)
        sent += transferred;
      else if (!transferred) {
        CloseHandle(p->handle);
        p->handle = INVALID_HANDLE_VALUE;
      } else
        status = output_bytes(q, b, i, p->bytes, transferred, &diagnostic);
    }
    if (status == FS_OK && count && !progressed &&
        WaitForMultipleObjects(count, events, FALSE, 25) == WAIT_FAILED)
      status = FS_IO;
  }
  while (status == FS_OK &&
         WaitForSingleObject(child.hProcess, 25) == WAIT_TIMEOUT)
    status = fsx_content_stop(q);
  DWORD exit_code = 0;
  if (status == FS_OK &&
      (!GetExitCodeProcess(child.hProcess, &exit_code) || exit_code > 1 ||
       diagnostic || (exit_code == 1 && b->used) ||
       (exit_code == 0 && !b->used)))
    status = FS_IO;
cleanup:
  if (child.hProcess) {
    TerminateJobObject(job, ERROR_CANCELLED);
    WaitForSingleObject(child.hProcess, INFINITE);
    JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting;
    do {
      if (!QueryInformationJobObject(job, JobObjectBasicAccountingInformation,
                                     &accounting, sizeof(accounting), NULL)) {
        if (status == FS_OK)
          status = FS_IO;
        break;
      }
      if (accounting.ActiveProcesses)
        cmeta_sleep_ms(1);
    } while (accounting.ActiveProcesses);
    DWORD code = 0;
    GetExitCodeProcess(child.hProcess, &code);
    cmeta_mutex_lock(&q->mutex);
    q->info.tool_exit_code = (int)code;
    if (counted)
      q->info.running_processes--;
    cmeta_mutex_unlock(&q->mutex);
    CloseHandle(child.hThread);
    CloseHandle(child.hProcess);
  }
  for (unsigned i = 0; i < 3; i++) {
    Pipe *p = &pipes[i];
    if (p->handle != INVALID_HANDLE_VALUE) {
      if (p->pending) {
        CancelIoEx(p->handle, &p->io);
        DWORD ignored;
        GetOverlappedResult(p->handle, &p->io, &ignored, TRUE);
      }
      CloseHandle(p->handle);
    }
    if (p->child != INVALID_HANDLE_VALUE)
      CloseHandle(p->child);
    if (p->io.hEvent)
      CloseHandle(p->io.hEvent);
  }
  if (job)
    CloseHandle(job);
  if (attributes_ready)
    DeleteProcThreadAttributeList(startup.lpAttributeList);
  fsx_free(startup.lpAttributeList);
  fsx_free(input);
  fsx_free(tool);
  fsx_free(line);
  return status;
}
FsStatus fsx_content_file(FsContentQuery *q, const FsEntry *entry,
                          FsContentBatch *b) {
  if (entry->flags & (FS_ENTRY_DIRECTORY | FS_ENTRY_LINK))
    return FS_UNSUPPORTED;
  HANDLE file = INVALID_HANDLE_VALUE;
  BY_HANDLE_FILE_INFORMATION before, after;
  FILE_BASIC_INFO before_basic, after_basic;
  FILE_ID_INFO before_volume, after_volume;
  FsStatus status =
      open_file(q, entry, &file, &before, &before_basic, &before_volume);
  if (status != FS_OK)
    return status;
  b->info.read_start_ns = cmeta_hrtime();
  uint64_t size = ((uint64_t)before.nFileSizeHigh << 32) | before.nFileSizeLow;
  status = size > q->request.max_file_bytes ? FS_LIMIT : execute(q, file, b);
  if (status == FS_OK) {
    status = file_info(q, file, &after, &after_basic, &after_volume);
    if (status == FS_OK && !same_file(&before, &before_basic, &before_volume,
                                      &after, &after_basic, &after_volume))
      status = FS_STALE;
  }
  CloseHandle(file);
  if (status == FS_OK) {
    status = open_file(q, entry, &file, &after, &after_basic, &after_volume);
    if (status == FS_OK) {
      if (!same_file(&before, &before_basic, &before_volume, &after,
                     &after_basic, &after_volume))
        status = FS_STALE;
      CloseHandle(file);
    }
  }
  b->info.read_end_ns = cmeta_hrtime();
  return status;
}

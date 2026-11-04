#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <shellapi.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
/* Source-owned native peer for handle inheritance and job-tree cancellation. */
int main(void) {
  int argc;
  wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  if (!argv)
    return 90;
  if (argc == 2 && !wcscmp(argv[1], L"--hold")) {
    LocalFree(argv);
    Sleep(INFINITE);
    return 91;
  }
  if (argc != 11) {
    LocalFree(argv);
    return 92;
  }
  bool tree = !wcsncmp(argv[8], L"tree:", 5);
  if (tree) {
    wchar_t self[2048], line[2100];
    if (!GetModuleFileNameW(NULL, self, 2048))
      return 93;
    swprintf(line, 2100, L"\"%ls\" --hold", self);
    STARTUPINFOW startup = {0};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION child = {0};
    if (!CreateProcessW(self, line, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL,
                        NULL, &startup, &child))
      return 94;
    HANDLE file = CreateFileW(argv[8] + 5, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                              CREATE_ALWAYS, 0, NULL);
    if (file == INVALID_HANDLE_VALUE)
      return 95;
    DWORD written;
    BOOL ok = WriteFile(file, &child.dwProcessId, sizeof(child.dwProcessId),
                        &written, NULL);
    CloseHandle(file);
    CloseHandle(child.hThread);
    CloseHandle(child.hProcess);
    if (!ok || written != sizeof(DWORD))
      return 96;
  } else if (!wcsncmp(argv[8], L"handle:", 7))
    SetEvent((HANDLE)(uintptr_t)wcstoull(argv[8] + 7, NULL, 16));
  else
    return 97;
  LocalFree(argv);
  unsigned char bytes[8192];
  DWORD read;
  while (ReadFile(GetStdHandle(STD_INPUT_HANDLE), bytes, sizeof(bytes), &read,
                  NULL) &&
         read) {
  }
  if (tree) {
    Sleep(INFINITE);
    return 98;
  }
  const char frame[] = "{\"line\":1,\"offset\":0,\"text\":\"checked\"}\n";
  DWORD written;
  return WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), frame, sizeof(frame) - 1,
                   &written, NULL)
             ? 0
             : 99;
}

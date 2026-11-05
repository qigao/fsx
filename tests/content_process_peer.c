#define _POSIX_C_SOURCE 200809L
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static int parallel(const char *root, bool fail) {
  int first = getchar();
  unsigned char input[8192];
  while (fread(input, 1, sizeof(input), stdin)) {
  }
  char path[4096];
  snprintf(path, sizeof(path), "%s/%ld.ready", root, (long)getpid());
  FILE *ready = fopen(path, "wb");
  if (!ready)
    return 94;
  fclose(ready);
  snprintf(path, sizeof(path), "%s/go", root);
  struct timespec delay = {0, 1000000};
  while ((fail && first != 'A') || access(path, F_OK))
    nanosleep(&delay, NULL);
  if (fail) {
    fputs("injected parallel failure\n", stderr);
    return 2;
  }
  fputs("{\"line\":1,\"offset\":0,\"text\":\"checked\"}\n", stdout);
  return 0;
}

/* Native fault-injection peer, only for the formal content protocol tests.
 * argv is the exact bfs invocation; the selected fixture root owns its mode. */
int main(int argc, char **argv) {
  if (argc == 11 && !strncmp(argv[8], "parallel:", 9))
    return parallel(argv[8] + 9, false);
  if (argc == 11 && !strncmp(argv[8], "parallel-fail:", 14))
    return parallel(argv[8] + 14, true);
  if (argc != 9)
    return 90;
  char path[4096];
  snprintf(path, sizeof(path), "%s/protocol-case", argv[4]);
  FILE *control = fopen(path, "r");
  if (!control)
    return 91;
  unsigned mode = 0;
  int read = fscanf(control, "%u", &mode);
  int descriptor = -1;
  unsigned long long device = 0, inode = 0;
  if (read == 1 && mode == 9)
    read = fscanf(control, "%d %llu %llu", &descriptor, &device, &inode) == 3;
  fclose(control);
  if (read != 1)
    return 92;
  switch (mode) {
  case 1:
    printf("%s/file", argv[4]); /* Missing NUL delimiter. */
    return 0;
  case 2:
    fwrite("/fsx-outside\0", 1, sizeof("/fsx-outside"), stdout);
    return 0;
  case 3:
    printf("%s/../escape%c", argv[4], 0);
    return 0;
  case 4:
    for (unsigned i = 0; i < 65536; i++)
      putchar('x');
    putchar(0);
    return 0;
  case 5:
    fputs("native traversal permission failure\n", stderr);
    return 1;
  case 6:
    return 7;
  case 7:
    for (;;)
      pause();
  case 8:
    putchar(0);
    return 0;
  case 9: {
    struct stat info;
    return fstat(descriptor, &info) == 0 &&
                   (unsigned long long)info.st_dev == device &&
                   (unsigned long long)info.st_ino == inode
               ? 97
               : 0;
  }
  default:
    return 93;
  }
}

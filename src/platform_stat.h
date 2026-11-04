#ifndef FSX_PLATFORM_STAT_H
#define FSX_PLATFORM_STAT_H
#include <sys/stat.h>
#include <time.h>
static inline struct timespec fsx_stat_mtime(const struct stat *info) {
#ifdef __APPLE__
  return info->st_mtimespec;
#else
  return info->st_mtim;
#endif
}
static inline struct timespec fsx_stat_ctime(const struct stat *info) {
#ifdef __APPLE__
  return info->st_ctimespec;
#else
  return info->st_ctim;
#endif
}
#endif

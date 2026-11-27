/* Explicit configuration for FSX's Linux/macOS traversal subset. No optional
 * predicate libraries or configure-time executable probes are required. */
#ifndef BFS_CONFIG_H
#define BFS_CONFIG_H

#define BFS_WITH_LIBURING 0
#define BFS_USE_STATX 0
#define BFS_HAS_STATX 0
#define BFS_HAS_STATX_SYSCALL 0
#define BFS_USE_TARGET_CLONES 0
#define BFS_HAS_DPRINTF 1
#define BFS_HAS_PRAGMA_NOUNROLL 1
#define BFS_HAS_GETPROGNAME 0
#define BFS_HAS_GETPROGNAME_GNU 0
#define BFS_HAS_GETMNTENT_1 0
#define BFS_HAS_GETMNTENT_2 0
#define BFS_HAS_GETMNTINFO 0
#define BFS_HAS_PTHREAD_SETNAME_NP 0
#define BFS_HAS_PTHREAD_SET_NAME_NP 0
#define BFS_HAS_GETDENTS 0
#define BFS_HAS_GETDENTS64 0
#define BFS_HAS_POSIX_GETDENTS 0
#define BFS_HAS_FDCLOSEDIR 0
#define BFS_HAS_ST_BIRTHTIM 0
#define BFS_HAS___ST_BIRTHTIM 0
#define BFS_HAS_BUILTIN_RISCV_PAUSE 0
#define BFS_HAS_STRERROR_L 0
#define BFS_HAS_USELOCALE 0

#if defined(__linux__)
#define BFS_HAS_GETDENTS64_SYSCALL 1
#define BFS_HAS_ST_ACMTIM 1
#define BFS_HAS_ST_ACMTIMESPEC 0
#define BFS_HAS_ST_BIRTHTIMESPEC 0
#define BFS_HAS_ST_FLAGS 0
#elif defined(__APPLE__)
#define BFS_HAS_GETDENTS64_SYSCALL 0
#define BFS_HAS_ST_ACMTIM 0
#define BFS_HAS_ST_ACMTIMESPEC 1
#define BFS_HAS_ST_BIRTHTIMESPEC 1
#define BFS_HAS_ST_FLAGS 1
#else
#error "FSX's bfs traversal subset supports Linux and macOS"
#endif

#if defined(__GLIBC__)
#define BFS_HAS_STRERROR_R_GNU 1
#define BFS_HAS_STRERROR_R_POSIX 0
#else
#define BFS_HAS_STRERROR_R_GNU 0
#define BFS_HAS_STRERROR_R_POSIX 1
#endif

#endif

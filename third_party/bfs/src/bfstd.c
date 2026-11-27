// Copyright © Tavian Barnes <tavianator@tavianator.com>
// SPDX-License-Identifier: 0BSD
// FSX: traversal-only subset of upstream bfstd.c. See UPSTREAM.md.

#include "bfstd.h"
#include "diag.h"
#include <errno.h>
#include <fcntl.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

bool error_is_like(int error, int category) {
	if (error == category) {
		return true;
	}

	switch (category) {
	case ENOENT:
		return error == ENOTDIR;

	case ENOSYS:
		// https://github.com/opencontainers/runc/issues/2151
		return error == EPERM;

#if __DragonFly__
	// https://twitter.com/tavianator/status/1742991411203485713
	case ENAMETOOLONG:
		return error == EFAULT;
#endif
	}

	return false;
}

bool errno_is_like(int category) {
	return error_is_like(errno, category);
}

int try(int ret) {
	if (ret >= 0) {
		return ret;
	} else {
		bfs_assert(errno > 0, "errno should be positive, was %d\n", errno);
		return -errno;
	}
}

char *xdirname(const char *path) {
	size_t i = xbaseoff(path);

	// Skip trailing slashes
	while (i > 0 && path[i - 1] == '/') {
		--i;
	}

	if (i > 0) {
		return strndup(path, i);
	} else if (path[i] == '/') {
		return strdup("/");
	} else {
		return strdup(".");
	}
}

size_t xbaseoff(const char *path) {
	size_t i = strlen(path);

	// Skip trailing slashes
	while (i > 0 && path[i - 1] == '/') {
		--i;
	}

	// Find the beginning of the name
	while (i > 0 && path[i - 1] != '/') {
		--i;
	}

	// Skip leading slashes
	while (path[i] == '/' && path[i + 1]) {
		++i;
	}

	return i;
}

FILE *xfopen(const char *path, int flags) {
	char mode[4];

	switch (flags & O_ACCMODE) {
	case O_RDONLY:
		strcpy(mode, "rb");
		break;
	case O_WRONLY:
		strcpy(mode, "wb");
		break;
	case O_RDWR:
		strcpy(mode, "r+b");
		break;
	default:
		bfs_bug("Invalid access mode");
		errno = EINVAL;
		return NULL;
	}

	if (flags & O_APPEND) {
		mode[0] = 'a';
	}

	int fd;
	if (flags & O_CREAT) {
		fd = open(path, flags, 0666);
	} else {
		fd = open(path, flags);
	}

	if (fd < 0) {
		return NULL;
	}

	FILE *ret = fdopen(fd, mode);
	if (!ret) {
		close_quietly(fd);
		return NULL;
	}

	return ret;
}

const char *xgetprogname(void) {
	const char *cmd = NULL;
#if BFS_HAS_GETPROGNAME
	cmd = getprogname();
#elif BFS_HAS_GETPROGNAME_GNU
	cmd = program_invocation_short_name;
#endif

	if (!cmd) {
		cmd = BFS_COMMAND;
	}

	return cmd;
}

const char *xstrerror(int errnum) {
	int saved = errno;
	const char *ret = NULL;
	static thread_local char buf[256];

	// On FreeBSD with MemorySanitizer, duplocale() triggers
	// https://github.com/llvm/llvm-project/issues/65532
#if BFS_HAS_STRERROR_L && !(__FreeBSD__ && __SANITIZE_MEMORY__)
#  if BFS_HAS_USELOCALE
	locale_t loc = uselocale((locale_t)0);
#  else
	locale_t loc = LC_GLOBAL_LOCALE;
#  endif

	bool free_loc = false;
	if (loc == LC_GLOBAL_LOCALE) {
		loc = duplocale(loc);
		free_loc = true;
	}

	if (loc != (locale_t)0) {
		ret = strerror_l(errnum, loc);
		if (free_loc) {
			freelocale(loc);
		}
	}
#elif BFS_HAS_STRERROR_R_POSIX
	if (strerror_r(errnum, buf, sizeof(buf)) == 0) {
		ret = buf;
	}
#elif BFS_HAS_STRERROR_R_GNU
	ret = strerror_r(errnum, buf, sizeof(buf));
#endif

	if (!ret) {
		// Fallback for strerror_[lr]() or duplocale() failures
		snprintf(buf, sizeof(buf), "Unknown error %d", errnum);
		ret = buf;
	}

	errno = saved;
	return ret;
}

const char *errstr(void) {
	return xstrerror(errno);
}

int dup_cloexec(int fd) {
#ifdef F_DUPFD_CLOEXEC
	return fcntl(fd, F_DUPFD_CLOEXEC, 0);
#else
	int ret = dup(fd);
	if (ret < 0) {
		return -1;
	}

	if (fcntl(ret, F_SETFD, FD_CLOEXEC) == -1) {
		close_quietly(ret);
		return -1;
	}

	return ret;
#endif
}

void close_quietly(int fd) {
	int error = errno;
	xclose(fd);
	errno = error;
}

int xclose(int fd) {
	int ret = close(fd);
	if (ret != 0) {
		bfs_verify(errno != EBADF);
	}
	return ret;
}


// Copyright © Tavian Barnes <tavianator@tavianator.com>
// SPDX-License-Identifier: 0BSD

#include "diag.h"

#include "bfs.h"
#include "bfstd.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

/**
 * Print an error using dprintf() if possible, because it's more likely to be
 * async-signal-safe in practice.
 */
#if BFS_HAS_DPRINTF
#  define veprintf(...) vdprintf(STDERR_FILENO, __VA_ARGS__)
#else
#  define veprintf(...) vfprintf(stderr, __VA_ARGS__)
#endif

void bfs_diagf(const char *format, ...) {
	va_list args;
	va_start(args, format);
	veprintf(format, args);
	va_end(args);
}

[[_noreturn]]
void bfs_abortf(const char *format, ...) {
	va_list args;
	va_start(args, format);
	veprintf(format, args);
	va_end(args);

	abort();
}


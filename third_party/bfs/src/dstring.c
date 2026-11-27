// Copyright © Tavian Barnes <tavianator@tavianator.com>
// SPDX-License-Identifier: 0BSD

#include "dstring.h"

#include "alloc.h"
#include "bit.h"
#include "diag.h"

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/**
 * The memory representation of a dynamic string.  Users get a pointer to str.
 */
struct dstring {
	/** Capacity of the string, *including* the terminating NUL. */
	size_t cap;
	/** Length of the string, *excluding* the terminating NUL. */
	size_t len;
	/** The string itself. */
	[[_counted_by(cap)]]
	alignas(dchar) char str[];
};

#define DSTR_OFFSET offsetof(struct dstring, str)

/** Back up to the header from a pointer to dstring::str. */
static struct dstring *dstrheader(const dchar *dstr) {
	return (struct dstring *)(dstr - DSTR_OFFSET);
}

/**
 * In some provenance models, the expression `header->str` has its provenance
 * restricted to just the `str` field itself, making a future dstrheader()
 * illegal.  This alternative is guaranteed to preserve provenance for the entire
 * allocation.
 *
 * - https://stackoverflow.com/q/25296019
 */
static dchar *dstrdata(struct dstring *header) {
	return (char *)header + DSTR_OFFSET;
}

/** Set the length of a dynamic string. */
static void dstrsetlen(struct dstring *header, size_t len) {
	bfs_assert(len < header->cap);
	header->len = len;
	header->str[len] = '\0';
}

/** Allocate a dstring with the given contents. */
static dchar *dstralloc_impl(size_t cap, size_t len, const char *str) {
	// Avoid reallocations for small strings
	if (cap < DSTR_OFFSET) {
		cap = DSTR_OFFSET;
	}

	struct dstring *header = ALLOC_FLEX(struct dstring, str, cap);
	if (!header) {
		return NULL;
	}

	header->cap = cap;
	dstrsetlen(header, len);

	dchar *ret = dstrdata(header);
	memcpy(ret, str, len);
	return ret;
}

dchar *dstralloc(size_t cap) {
	return dstralloc_impl(cap + 1, 0, "");
}

dchar *dstrdup(const char *str) {
	return dstrxdup(str, strlen(str));
}

dchar *dstrndup(const char *str, size_t n) {
	return dstrxdup(str, strnlen(str, n));
}

dchar *dstrddup(const dchar *dstr) {
	return dstrxdup(dstr, dstrlen(dstr));
}

dchar *dstrxdup(const char *str, size_t len) {
	return dstralloc_impl(len + 1, len, str);
}

size_t dstrlen(const dchar *dstr) {
	return dstrheader(dstr)->len;
}

int dstreserve(dchar **dstr, size_t cap) {
	if (!*dstr) {
		*dstr = dstralloc(cap);
		return *dstr ? 0 : -1;
	}

	struct dstring *header = dstrheader(*dstr);
	size_t old_cap = header->cap;
	size_t new_cap = cap + 1; // Terminating NUL
	if (old_cap >= new_cap) {
		return 0;
	}

	new_cap = bit_ceil(new_cap);
	header = REALLOC_FLEX(struct dstring, str, header, old_cap, new_cap);
	if (!header) {
		return -1;
	}

	header->cap = new_cap;
	*dstr = dstrdata(header);
	return 0;
}

int dstresize(dchar **dstr, size_t len) {
	if (dstreserve(dstr, len) != 0) {
		return -1;
	}

	struct dstring *header = dstrheader(*dstr);
	dstrsetlen(header, len);
	return 0;
}

void dstrshrink(dchar *dstr, size_t len) {
	struct dstring *header = dstrheader(dstr);
	bfs_assert(len <= header->len);
	dstrsetlen(header, len);
}

int dstrcat(dchar **dest, const char *src) {
	return dstrxcat(dest, src, strlen(src));
}

int dstrncat(dchar **dest, const char *src, size_t n) {
	return dstrxcat(dest, src, strnlen(src, n));
}

int dstrdcat(dchar **dest, const dchar *src) {
	return dstrxcat(dest, src, dstrlen(src));
}

int dstrxcat(dchar **dest, const char *src, size_t len) {
	size_t oldlen = dstrlen(*dest);
	size_t newlen = oldlen + len;

	if (dstresize(dest, newlen) != 0) {
		return -1;
	}

	memcpy(*dest + oldlen, src, len);
	return 0;
}

int dstrapp(dchar **str, char c) {
	return dstrxcat(str, &c, 1);
}

int dstrcpy(dchar **dest, const char *src) {
	return dstrxcpy(dest, src, strlen(src));
}

int dstrncpy(dchar **dest, const char *src, size_t n) {
	return dstrxcpy(dest, src, strnlen(src, n));
}

int dstrdcpy(dchar **dest, const dchar *src) {
	return dstrxcpy(dest, src, dstrlen(src));
}

int dstrxcpy(dchar **dest, const char *src, size_t len) {
	if (dstresize(dest, len) != 0) {
		return -1;
	}

	memcpy(*dest, src, len);
	return 0;
}

void dstrfree(dchar *dstr) {
	if (dstr) {
		free(dstrheader(dstr));
	}
}

dchar *dstrepeat(const char *str, size_t n) {
	size_t len = strlen(str);
	dchar *ret = dstralloc(n * len);
	if (!ret) {
		return NULL;
	}

	for (size_t i = 0; i < n; ++i) {
		if (dstrxcat(&ret, str, len) < 0) {
			dstrfree(ret);
			return NULL;
		}
	}

	return ret;
}

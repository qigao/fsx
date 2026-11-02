#include "internal.h"
#include <stddef.h>
#include <stdlib.h>
typedef union FsAllocation {
  struct {
    FsEngine *engine;
    size_t size;
  } owner;
  max_align_t alignment;
} FsAllocation;
bool fsx_reserve(FsEngine *e, size_t bytes) {
  cmeta_mutex_lock(&e->budget_mutex);
  bool ok = bytes <= e->config.owned_memory_budget - e->owned;
  if (ok)
    e->owned += bytes;
  cmeta_mutex_unlock(&e->budget_mutex);
  return ok;
}
void fsx_unreserve(FsEngine *e, size_t bytes) {
  cmeta_mutex_lock(&e->budget_mutex);
  e->owned -= bytes;
  cmeta_mutex_unlock(&e->budget_mutex);
}
void *fsx_alloc(FsEngine *e, size_t bytes) {
  if (!bytes)
    bytes = 1;
  if (bytes > SIZE_MAX - sizeof(FsAllocation))
    return NULL;
  size_t size = bytes + sizeof(FsAllocation);
  cmeta_mutex_lock(&e->budget_mutex);
  if (size > e->config.owned_memory_budget - e->owned) {
    cmeta_mutex_unlock(&e->budget_mutex);
    return NULL;
  }
  e->owned += size;
  cmeta_mutex_unlock(&e->budget_mutex);
  FsAllocation *a = malloc(size);
  if (!a) {
    cmeta_mutex_lock(&e->budget_mutex);
    e->owned -= size;
    cmeta_mutex_unlock(&e->budget_mutex);
    return NULL;
  }
  a->owner.engine = e;
  a->owner.size = size;
  return a + 1;
}
void fsx_free(void *p) {
  if (!p)
    return;
  FsAllocation *a = (FsAllocation *)p - 1;
  FsEngine *e = a->owner.engine;
  size_t size = a->owner.size;
  free(a);
  cmeta_mutex_lock(&e->budget_mutex);
  e->owned -= size;
  cmeta_mutex_unlock(&e->budget_mutex);
}
char *fsx_copy(FsEngine *e, const char *s, size_t n) {
  if (n == SIZE_MAX)
    return NULL;
  char *p = fsx_alloc(e, n + 1);
  if (p) {
    memcpy(p, s, n);
    p[n] = 0;
  }
  return p;
}
bool fsx_utf8(const char *s, size_t n) {
  if (!s || memchr(s, 0, n))
    return false;
  size_t at = 0;
  salts_unicode_scalar scalar;
  while (at < n)
    if (salts_unicode_utf8_next(vstr_from_buf(s, n), &at, &scalar) !=
        SALTS_UNICODE_OK)
      return false;
  return true;
}
FsStatus fsx_fold(FsEngine *e, const char *s, char **out) {
  *out = NULL;
  size_t n = 0;
  if (salts_unicode_case_fold(vstr_from_cstr(s), NULL, 0, &n) !=
      SALTS_UNICODE_OK)
    return FS_INVALID;
  char *p = fsx_alloc(e, n + 1);
  if (!p)
    return FS_NOMEM;
  if (salts_unicode_case_fold(vstr_from_cstr(s), p, n, &n) !=
      SALTS_UNICODE_OK) {
    fsx_free(p);
    return FS_INVALID;
  }
  p[n] = 0;
  *out = p;
  return FS_OK;
}
FsPage *fsx_page_create(FsEngine *e, uint32_t capacity) {
  if (!capacity || capacity > FSX_PAGE_ENTRIES ||
      sizeof(FsEntry) > (SIZE_MAX - sizeof(FsPage)) / capacity)
    return NULL;
  size_t bytes = sizeof(FsPage) + (size_t)capacity * sizeof(FsEntry);
  FsPage *page = fsx_alloc(e, bytes);
  if (page) {
    memset(page, 0, bytes);
    atomic_init(&page->refs, 1);
    page->capacity = capacity;
  }
  return page;
}
FsStatus fsx_page_retain(FsPage *page) {
  unsigned refs = atomic_load_explicit(&page->refs, memory_order_relaxed);
  for (;;) {
    if (refs == UINT_MAX)
      return FS_LIMIT;
    if (atomic_compare_exchange_weak_explicit(&page->refs, &refs, refs + 1,
                                              memory_order_relaxed,
                                              memory_order_relaxed))
      return FS_OK;
  }
}
FsStatus fsx_page_append(FsEngine *e, FsPage *page, const FsEntry *source) {
  if (page->count == page->capacity)
    return FS_LIMIT;
  FsEntry *entry = &page->entries[page->count++];
  *entry = *source;
  entry->name = entry->path = entry->name_fold = entry->path_fold = NULL;
  const char *strings[] = {source->name, source->path, source->name_fold,
                           source->path_fold};
  char **slots[] = {&entry->name, &entry->path, &entry->name_fold,
                    &entry->path_fold};
  for (unsigned i = 0; i < 4; i++) {
    *slots[i] = fsx_copy(e, strings[i], strlen(strings[i]));
    if (!*slots[i])
      return FS_NOMEM;
  }
  return FS_OK;
}
void fsx_page_release(FsPage *page) {
  if (!page ||
      atomic_fetch_sub_explicit(&page->refs, 1, memory_order_acq_rel) != 1)
    return;
  for (uint32_t i = 0; i < page->count; i++) {
    fsx_free(page->entries[i].name);
    fsx_free(page->entries[i].path);
    fsx_free(page->entries[i].name_fold);
    fsx_free(page->entries[i].path_fold);
  }
  fsx_free(page);
}
void fsx_snapshot_release(FsSnapshot *s) {
  if (!s || atomic_fetch_sub_explicit(&s->refs, 1, memory_order_acq_rel) != 1)
    return;
  if (s->pages)
    for (uint32_t i = 0; i < s->page_count; i++)
      fsx_page_release(s->pages[i]);
  fsx_free(s->pages);
  fsx_free(s);
}

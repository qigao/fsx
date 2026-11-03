#include "ntfs.h"

static uint16_t get16(const unsigned char *p) {
  return (uint16_t)(p[0] | p[1] << 8);
}
static uint32_t get32(const unsigned char *p) {
  return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
         (uint32_t)p[3] << 24;
}
FsStatus fsx_ntfs_records(const void *data, size_t size, bool journal,
                          uint64_t from, int64_t target, uint32_t limit,
                          FsNtfsRecordFn visit, void *context, uint64_t *next,
                          uint32_t *count) {
  *count = 0;
  *next = from;
  if (!data || size < 8 || !limit ||
      (journal && (from > INT64_MAX || target < 0)))
    return FS_CORRUPT;
  const unsigned char *buffer = data;
  uint64_t end = fsx_ntfs_get64(buffer);
  if (end < from || (journal && end > INT64_MAX))
    return FS_CORRUPT;
  int64_t previous = -1;
  for (size_t at = 8; at < size;) {
    size_t remaining = size - at;
    if (remaining < 8)
      return FS_CORRUPT;
    const unsigned char *p = buffer + at;
    uint32_t length = get32(p);
    if (length < 60 || length % 8 || length > remaining)
      return FS_CORRUPT;
    if (get16(p + 4) != 2)
      return FS_UNSUPPORTED;
    uint16_t name_bytes = get16(p + 56), offset = get16(p + 58);
    if (!name_bytes || name_bytes % 2 || offset < 60 || offset % 2 ||
        offset > length || name_bytes > length - offset)
      return FS_CORRUPT;
    FsNtfsRecord record = {fsx_ntfs_get64(p + 8),
                           fsx_ntfs_get64(p + 16),
                           (int64_t)fsx_ntfs_get64(p + 24),
                           get32(p + 40),
                           get32(p + 52),
                           length,
                           p + offset,
                           name_bytes};
    if (record.usn < 0)
      return FS_CORRUPT;
    if (journal) {
      if ((uint64_t)record.usn < from || record.usn <= previous ||
          (uint64_t)record.usn >= end ||
          (uint64_t)length > end - (uint64_t)record.usn)
        return FS_CORRUPT;
      if (record.usn >= target) {
        *next = (uint64_t)target;
        return FS_OK;
      }
      previous = record.usn;
    }
    FsStatus status = visit(context, &record);
    if (status != FS_OK)
      return status;
    ++*count;
    at += length;
    if (journal && *count == limit && at < size) {
      *next = (uint64_t)record.usn + length;
      if (*next > (uint64_t)target)
        *next = (uint64_t)target;
      return FS_OK;
    }
  }
  if (end == from && size > 8)
    return FS_CORRUPT;
  *next = journal && end > (uint64_t)target ? (uint64_t)target : end;
  return FS_OK;
}
FsStatus fsx_ntfs_text(FsEngine *e, const void *data, size_t size, char **out) {
  *out = NULL;
  if (!size || size % 2 || size > 65534)
    return FS_UNSUPPORTED;
  const unsigned char *raw = data;
  char *text = fsx_alloc(e, size / 2 * 3 + 1);
  if (!text)
    return FS_NOMEM;
  size_t used = 0;
  for (size_t at = 0; at < size; at += 2) {
    uint32_t cp = get16(raw + at);
    if (cp >= 0xd800 && cp <= 0xdbff) {
      if (at + 2 >= size) {
        fsx_free(text);
        return FS_UNSUPPORTED;
      }
      uint32_t low = get16(raw + at + 2);
      if (low < 0xdc00 || low > 0xdfff) {
        fsx_free(text);
        return FS_UNSUPPORTED;
      }
      cp = 0x10000 + ((cp - 0xd800) << 10) + low - 0xdc00;
      at += 2;
    } else if (cp >= 0xdc00 && cp <= 0xdfff) {
      fsx_free(text);
      return FS_UNSUPPORTED;
    }
    if (!cp || cp == '/' || cp == '\\' || cp == ':') {
      fsx_free(text);
      return FS_UNSUPPORTED;
    }
    if (cp < 0x80)
      text[used++] = (char)cp;
    else if (cp < 0x800) {
      text[used++] = (char)(0xc0 | cp >> 6);
      text[used++] = (char)(0x80 | (cp & 63));
    } else if (cp < 0x10000) {
      text[used++] = (char)(0xe0 | cp >> 12);
      text[used++] = (char)(0x80 | (cp >> 6 & 63));
      text[used++] = (char)(0x80 | (cp & 63));
    } else {
      text[used++] = (char)(0xf0 | cp >> 18);
      text[used++] = (char)(0x80 | (cp >> 12 & 63));
      text[used++] = (char)(0x80 | (cp >> 6 & 63));
      text[used++] = (char)(0x80 | (cp & 63));
    }
  }
  if (used > 65535) {
    fsx_free(text);
    return FS_LIMIT;
  }
  text[used] = 0;
  *out = text;
  return FS_OK;
}

/* FILE_LINKS_INFORMATION / FILE_LINK_ENTRY_INFORMATION, x64 Windows ABI.
 * Name length is UTF-16 character count, unlike the USN V2 byte count. */
FsStatus fsx_ntfs_link_records(const void *data, size_t bytes, uint32_t limit,
                               FsNtfsParentFn visit, void *context,
                               uint32_t *count) {
  *count = 0;
  if (!data || bytes < 8 || !limit)
    return FS_CORRUPT;
  const unsigned char *p = data;
  uint32_t needed = get32(p), entries = get32(p + 4);
  if (needed < 8 || needed > bytes || !entries)
    return FS_CORRUPT;
  if (entries > limit)
    return FS_LIMIT;
  size_t at = 8;
  for (uint32_t i = 0; i < entries; i++) {
    if (at > needed || needed - at < 20)
      return FS_CORRUPT;
    uint32_t next = get32(p + at), units = get32(p + at + 16);
    uint64_t parent = fsx_ntfs_get64(p + at + 8);
    if (!parent || !units || units > (needed - at - 20) / 2)
      return FS_CORRUPT;
    size_t length = 20 + (size_t)units * 2;
    if (i + 1 < entries) {
      if (next < length || next % 8 || next > needed - at ||
          needed - at - next < 20)
        return FS_CORRUPT;
    } else if (next)
      return FS_CORRUPT;
    FsStatus status = visit(context, parent);
    if (status != FS_OK)
      return status;
    (*count)++;
    at += next;
  }
  return FS_OK;
}

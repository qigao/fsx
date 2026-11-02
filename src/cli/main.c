#include <fsx/api.h>
#include <inttypes.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif
static void json_string(const char *s) {
  putchar('"');
  for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
    if (*p == '"' || *p == '\\') {
      putchar('\\');
      putchar(*p);
    } else if (*p < 32)
      printf("\\u%04x", *p);
    else
      putchar(*p);
  }
  putchar('"');
}
static bool positive_u32(const char *text, uint32_t *out) {
  uint32_t value = 0;
  if (!*text)
    return false;
  for (; *text; text++) {
    if (*text < '0' || *text > '9' ||
        value > (UINT32_MAX - (unsigned)(*text - '0')) / 10)
      return false;
    value = value * 10 + (unsigned)(*text - '0');
  }
  *out = value;
  return value != 0;
}
static volatile sig_atomic_t interrupted;
static void interrupt_watch(int sig) {
  (void)sig;
  interrupted = 1;
}
static FsStatus watch_directory(FsEngine *engine, const char *root,
                                uint32_t duration_ms, uint32_t updates,
                                int *exit_code) {
  FsWatchConfig config = fsx_watch_config();
  FsWatch *watch = NULL;
  FsStatus status = fsx_watch_open(engine, root, &config, &watch);
  if (status != FS_OK)
    return status;
  interrupted = 0;
  void (*previous)(int) = signal(SIGINT, interrupt_watch);
  if (previous == SIG_ERR) {
    fsx_watch_close(&watch);
    return FS_IO;
  }
  uint64_t now = fsx_engine_now_ns(engine);
  uint64_t span = (uint64_t)duration_ms * 1000000;
  uint64_t end = UINT64_MAX;
  if (duration_ms) {
    if (span > UINT64_MAX - now)
      status = FS_LIMIT;
    else
      end = now + span;
  }
  uint32_t completed = 0;
  while (status == FS_OK && !interrupted && (!updates || completed < updates)) {
    now = fsx_engine_now_ns(engine);
    if (now >= end)
      break;
    uint64_t deadline =
        UINT64_MAX - now < 250000000 ? UINT64_MAX : now + 250000000;
    if (end < deadline)
      deadline = end;
    FsWatchInfo changed;
    status = fsx_watch_wait(watch, deadline, &changed);
    if (status == FS_WAIT || status == FS_TIMEOUT) {
      status = FS_OK;
      continue;
    }
    if (status != FS_OK || interrupted)
      break;
    FsScanInfo scanned;
    status = fsx_watch_scan(watch, &scanned);
    if (status != FS_OK)
      break;
    completed++;
    printf("revision=%" PRIu64 " entries=%" PRIu64
           " complete=%s skipped=%" PRIu64 "\n",
           scanned.snapshot_epoch, scanned.entries,
           scanned.coverage_complete ? "true" : "false",
           scanned.skipped_entries);
    fflush(stdout);
    if (!scanned.coverage_complete)
      *exit_code = 3;
  }
  signal(SIGINT, previous);
  FsStatus closed = fsx_watch_close(&watch);
  return status == FS_OK ? closed : status;
}
static int run(int argc, char **argv) {
  if (argc < 3) {
    fputs("Usage: fsx index DATABASE ROOT\n       fsx search DATABASE [--limit "
          "N] [--offset N] [--sort name|path|size|mtime] [--desc] [--case] "
          "[--jit] [--scan-only] [--allow-partial-text] [--json] -- QUERY\n    "
          "   fsx info "
          "DATABASE\n       fsx watch DATABASE ROOT [--duration-ms N] "
          "[--updates N]\n",
          stderr);
    return 2;
  }
  FsEngineConfig config = fsx_engine_config(argv[2]);
  FsQueryOptions options = fsx_query_options();
  FsOpenError open_error = {0};
  config.open_error = &open_error;
  if (strcmp(argv[1], "index") && strcmp(argv[1], "watch"))
    config.flags &= ~FS_ENGINE_CREATE_DATABASE;
  uint32_t watch_duration = 0, watch_updates = 0;
  if (!strcmp(argv[1], "watch")) {
    if (argc < 4)
      return 2;
    for (int i = 4; i < argc; i += 2) {
      if (i + 1 >= argc)
        return 2;
      uint32_t *value;
      if (!strcmp(argv[i], "--duration-ms"))
        value = &watch_duration;
      else if (!strcmp(argv[i], "--updates"))
        value = &watch_updates;
      else
        return 2;
      if (!positive_u32(argv[i + 1], value))
        return 2;
    }
  }
  bool json = false;
  const char *text = "";
  for (int i = 3; strcmp(argv[1], "search") == 0 && i < argc; i++) {
    if (!strcmp(argv[i], "--")) {
      if (i + 2 != argc) {
        fputs("Pass the query as one quoted argument after --\n", stderr);
        return 2;
      }
      text = argv[i + 1];
      break;
    }
    if (!strcmp(argv[i], "--json"))
      json = true;
    else if (!strcmp(argv[i], "--desc"))
      options.flags |= FS_QUERY_DESCENDING;
    else if (!strcmp(argv[i], "--case"))
      options.flags |= FS_QUERY_CASE_SENSITIVE;
    else if (!strcmp(argv[i], "--jit"))
      config.flags |= FS_ENGINE_REGEX_JIT;
    else if (!strcmp(argv[i], "--scan-only"))
      config.flags |= FS_ENGINE_SCAN_ONLY;
    else if (!strcmp(argv[i], "--allow-partial-text"))
      options.flags |= FS_QUERY_ALLOW_PARTIAL_TEXT;
    else if ((!strcmp(argv[i], "--limit") || !strcmp(argv[i], "--offset")) &&
             i + 1 < argc) {
      const char *key = argv[i++], *value = argv[i];
      uint64_t number = 0;
      if (!*value)
        return 2;
      for (; *value; value++) {
        if (*value < '0' || *value > '9' ||
            number > (UINT32_MAX - (unsigned)(*value - '0')) / 10)
          return 2;
        number = number * 10 + (unsigned)(*value - '0');
      }
      if (!strcmp(key, "--limit"))
        options.limit = (uint32_t)number;
      else
        options.offset = (uint32_t)number;
    } else if (!strcmp(argv[i], "--sort") && i + 1 < argc) {
      const char *sort = argv[++i];
      if (!strcmp(sort, "name"))
        options.sort_key = FS_SORT_NAME;
      else if (!strcmp(sort, "path"))
        options.sort_key = FS_SORT_PATH;
      else if (!strcmp(sort, "size"))
        options.sort_key = FS_SORT_SIZE;
      else if (!strcmp(sort, "mtime"))
        options.sort_key = FS_SORT_MTIME;
      else
        return 2;
    } else {
      fprintf(stderr, "Unknown option: %s\n", argv[i]);
      return 2;
    }
  }
  FsEngine *engine = NULL;
  FsStatus status = fsx_engine_open(&config, &engine);
  if (status != FS_OK) {
    fprintf(stderr, "open: %s (native %d): %s\n", fsx_status_name(status),
            open_error.native_code, open_error.diagnostic);
    return 1;
  }
  int exit_code = 0;
  if (!strcmp(argv[1], "watch")) {
    status = watch_directory(engine, argv[3], watch_duration, watch_updates,
                             &exit_code);
  } else if (!strcmp(argv[1], "index") && argc == 4) {
    FsScanInfo info;
    status = fsx_engine_scan(engine, argv[3], &info);
    if (status == FS_OK) {
      printf("revision=%" PRIu64 " entries=%" PRIu64
             " complete=%s skipped=%" PRIu64 "\n",
             info.snapshot_epoch, info.entries,
             info.coverage_complete ? "true" : "false", info.skipped_entries);
      if (!info.coverage_complete)
        exit_code = 3;
    }
  } else if (!strcmp(argv[1], "info") && argc == 3) {
    FsProgress info;
    status = fsx_engine_progress(engine, &info);
    if (status == FS_OK)
      printf("revision=%" PRIu64 " entries=%" PRIu64
             " sources=%u incomplete=%u owned_bytes=%" PRIu64
             " pages=%u reused_pages=%u\n",
             info.snapshot_epoch, info.entries, info.source_count,
             info.incomplete_sources, info.owned_bytes, info.snapshot_pages,
             info.reused_snapshot_pages);
  } else if (!strcmp(argv[1], "search")) {
    FsQueryRequest request = {sizeof(request), FSX_API_VERSION, text,
                              strlen(text), options};
    FsQuery *query = NULL;
    FsResult *result = NULL;
    status = fsx_query_submit(engine, &request, &query);
    if (status == FS_OK)
      status = fsx_query_wait(query, FSX_DEADLINE_INFINITE);
    if (status == FS_OK)
      status = fsx_query_result_acquire(query, &result);
    if (result) {
      FsResultInfo info;
      fsx_result_info(result, &info);
      if (json)
        printf("{\"revision\":%" PRIu64 ",\"total\":%" PRIu64
               ",\"coverage_complete\":%s,\"candidate_strategy\":\"%s\","
               "\"candidates_examined\":%" PRIu64 ",\"entries\":[",
               info.snapshot_epoch, info.total_count,
               info.coverage_complete ? "true" : "false",
               FsCandidateStrategy_to_string(info.candidate_strategy),
               info.candidates_examined);
      for (uint32_t i = 0; i < info.window_count; i++) {
        FsHit hit;
        size_t written = 0;
        FsEntryView view;
        status = fsx_result_page(result, i, 1, &hit, 1, &written);
        if (status == FS_OK && written)
          status = fsx_result_entry(result, hit, &view);
        if (status != FS_OK)
          break;
        if (json) {
          if (i)
            putchar(',');
          fputs("{\"path\":", stdout);
          json_string(view.path);
          printf(",\"entry_key\":%" PRIu64 ",\"size\":", view.entry_key);
          if (view.flags & FS_ENTRY_SIZE_VALID)
            printf("%" PRIu64, view.size);
          else
            fputs("null", stdout);
          putchar('}');
        } else {
          fputs(view.path, stdout);
          putchar('\n');
        }
      }
      if (json)
        fputs("]}\n", stdout);
      else
        fprintf(stderr, "total=%" PRIu64 " revision=%" PRIu64 " complete=%s\n",
                info.total_count, info.snapshot_epoch,
                info.coverage_complete ? "true" : "false");
      if (!info.coverage_complete)
        exit_code = 3;
    } else if (query) {
      FsQueryInfo info;
      fsx_query_poll(query, &info);
      fprintf(stderr, "query byte %zu: %s\n", info.error_offset,
              info.diagnostic);
    }
    fsx_result_release(result);
    fsx_query_release(query);
  } else {
    status = FS_INVALID;
  }
  if (status != FS_OK) {
    fprintf(stderr, "fsx: %s\n", fsx_status_name(status));
    exit_code = 1;
  }
  FsStatus close_status = fsx_engine_close(&engine, FSX_DEADLINE_INFINITE);
  if (close_status != FS_OK) {
    fprintf(stderr, "close: %s\n", fsx_status_name(close_status));
    exit_code = 1;
  }
  return exit_code;
}
#ifdef _WIN32
int wmain(int argc, wchar_t **wide) {
  char **argv = calloc((size_t)argc + 1, sizeof(*argv));
  if (!argv)
    return 1;
  bool ok = true;
  for (int i = 0; i < argc; i++) {
    int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide[i], -1,
                                NULL, 0, NULL, NULL);
    if (n <= 0) {
      ok = false;
      break;
    }
    argv[i] = malloc((size_t)n);
    if (!argv[i] || !WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide[i],
                                         -1, argv[i], n, NULL, NULL)) {
      ok = false;
      break;
    }
  }
  int rc = ok ? run(argc, argv) : 1;
  for (int i = 0; i < argc; i++)
    free(argv[i]);
  free(argv);
  return rc;
}
#else
int main(int argc, char **argv) { return run(argc, argv); }
#endif

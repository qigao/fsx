# FileSearch (fsx)

A working C11/CMeta file-search library and CLI using SaltsCore, SQLite FTS5 and
PCRE2. The [design](docs/FileSearch-C11-设计.md) remains the roadmap; the current
implementation includes directory/RDC scanning, NTFS MFT/USN and FTS5 candidate planning; later
milestones remain on the roadmap.

## Implemented

- Native directory scans with file identity, hardlink aliases, metadata and
  explicit coverage. Directory links are recorded without traversing them;
  descent uses verified handles and relative child opens to resist replacement.
- SQLite transactions for sources, shared objects, entries and FTS5 trigram rows.
  Immutable query snapshots are prepared before commit and published afterward.
- Asynchronous directory/NTFS indexing on a dedicated single-writer worker,
  bounded owned NTFS batches, and page-limited SQLite memory work sets.
- Asynchronous queries on SaltsCore's bounded thread pool, retained results,
  cancellation, deadlines, STOPPING/close and allocation/admission limits.
- Linux/macOS/Windows asynchronous UTF-8 content jobs over a retained result window or a full
  metadata scope streamed from a pinned snapshot, using
  an explicit native ugrep executable, per-file batches and consumer backpressure.
- Explicit Linux/macOS bfs live discovery for unindexed directories, with root-relative
  file opens, bounded NUL path frames and no index writes.
- Unicode 17 full-fold substring search, scalar-aware glob matching, scoped
  `case:`/`path:`, AND/OR/NOT/groups, `file:`/`folder:`, extension sets, checked
  size filters and fixed-timezone modification-day filters.
- PCRE2 Unicode/byte regex, bounded execution and optional JIT. Query diagnostics
  identify source byte offsets. Results report actual semantic versions.
- Stable sorting and pagination, exact totals within indexed coverage, persistent
  revisions and stable database entry keys.
- FTS5 name-literal candidates with database identity/revision admission and
  scanner fallback. Results report the selected strategy and entries evaluated.
- Immutable 4096-entry pages shared across snapshots. Changed pages are copied
  before commit; retained results keep their original metadata and strings.
- Cooperative Windows RDC, Linux inotify and macOS FSEvents directory watches, armed before
  enumeration, with bounded notification buffers, root identity checks,
  overflow reconciliation and retryable shutdown. Linux registers each opened
  directory before listing it; the CLI runs the wait/rescan loop.

- Cooperative Windows NTFS sources: MFT directory topology, complete directory
  aliases, U0/U1 journal handoff, bounded USN batches, durable cursor recovery,
  subtree moves and atomic facts/FTS/checkpoint publication.

Query-visible unflushed deltas, full SQLite memory mirrors,
raw-unit queries for invalid Unicode names and Everything import are later work.
The current collector explicitly marks skipped/unrepresentable names as partial
coverage. RDC watches reconcile whole roots and have no durable change cursor;
NTFS sources use the strict profile described below.

See the [runtime and ownership contract](docs/runtime-contract.md) for precise
boundaries and limits. The C API is in [api.h](include/fsx/api.h); CMeta value and
status declarations are in [types.h](include/fsx/types.h).

The [cross-platform and content-search proposal](docs/cross-platform-content-search.md)
defines Linux inotify/macOS FSEvents and explicit bfs/ugrep content jobs. These
milestones include Linux inotify, Linux/macOS/Windows indexed content jobs, and
Linux/macOS bfs live discovery with an explicit separate request/CLI command.
macOS FSEvents uses a native dispatch-queue backend;
wait/scan consume a bounded event-flag bridge and reconcile the root.

## Build policy

Use versioned configure/build presets and formal CTest tests. There are no project
install rules, install presets, exports, consumer projects or install/consumer
verification paths. [AGENTS.md](AGENTS.md) records this user requirement.

The root build/presets were adapted from salts-core commit
`a2e3f2315f1de2194bd6f7fb257526c98074a2d8`. The supplied `CmakeUtils.cmake`,
`FindTools.cmake`, native Lemon source and re2c discovery are reused. The real
query lexer is generated from `src/query/lexer.re`; module-owned CMake files
register the library, CLI and formal tests. Keep `CMakeUserPresets.json` versioned.
Lemon source/template notices remain intact.

## Dependencies and environment

| Parent environment | Required content |
|---|---|
| `SALTSCORE_ROOT` | Compatible split Core SDK exporting Core, Platform, Concurrency, FS, Unicode and TinyTest |
| `CMETA_ROOT` | Compatible split CMeta SDK exporting `CMeta::CMeta` |
| `RE2C_ROOT` | Host re2c 4.6+ with `bin/` and `share/re2c/stdlib/` |
| `VCPKG_ROOT` | Canonical vcpkg checkout/tool selected by shared setup |
| `VCPKG_CACHE_REPOSITORY_ROOT` | Shared cache release `v0.1.002` (`d9f34af3f7bcad5ae2c8abf4ec95c7e93ad5d761`) |

Use absolute, forward-slash paths. The hidden `vcpkg-cache` and `sdk-roots` user
presets own these roots. `.env` is private and ignored; CMake does not load it
automatically. Preserve the caller's credentials and shared read-only feed policy.
Ordinary builds do not update shared checkouts or prewarm packages. CI pins the
shared actions to `v0.1.002` and selects `restore-packages: "false"`: the normal
product manifest build uses the local files cache and pinned source downloads.
Strict remote ABI restoration is a separate mode requiring every exact package.
All external package/tool transport uses curl, without Mono or NuGet executables.

The product manifest retains builtin baseline
`b1b19307e2d2ec1eefbdb7ea069de7d4bcd31f01`, PCRE2 10.46 and SQLite 3.50.4 with FTS5.
The added registry's zstd route is aligned to the cache commit without adding an
unused zstd dependency. `FSX_BUILD_SEARCH_TOOLS=ON` resolves optional ugrep/bfs
through the shared overlays; neither tool is linked into fsx. Their platform
restrictions remain in the manifest. TRE is not a dependency.

On Windows, preserve the shared LLVM-MinGW setup's `LLVM_MINGW_TOOLCHAIN_FILE`,
`LLVM_MINGW_DEPENDENCY_TOOLCHAIN_FILE` and
`LLVM_MINGW_HOST_DEPENDENCY_TOOLCHAIN_FILE`. Run from a VS developer shell and
retain the parent PATH. The shared native pkgconf helper supplies `PKG_CONFIG`
and `QIGAO_PKGCONF_VERSION`; presets preserve them through vcpkg's environment
filtering. SDKs must match the target architecture and compiler ABI. Runtime DLL
paths come from the selected profile; no post-build copying/deployment is used.

## Build and test

```powershell
cmake --preset windows-debug
cmake --build --preset windows-debug
ctest --preset windows-debug
```

`macos-debug` builds native ARM64 source-owned tests with the runner's Clang and
the published macOS Core/CMeta SDKs; `macos-release` builds the library/CLI.
The macOS 15 ARM64 CI profile targets scans, queries, indexing, real ugrep/bfs
content jobs and native FSEvents watching. `macos-x64-debug` and
`macos-x64-release` select the published Intel SDKs and x64-osx triplet; a separate
macos-15-intel CI job runs the same native suite. The selected SDK dylibs require macOS 15, matching
the explicit deployment baseline in the presets.
CI records native ARM64 and Intel results independently. Each profile runs scans,
queries, persistence, content processes and FSEvents against its selected SDKs;
results from one architecture do not substitute for the other.
`macos-x64-asan-ubsan` instruments the library, CLI and source-owned native tests
with Apple Clang AddressSanitizer/UndefinedBehaviorSanitizer. It runs the same
FSEvents lifecycle and actual bfs/ugrep cases as the ordinary Intel profile.
No sanitizer runtime options or child diagnostics are suppressed.
UBSan profiles compile with `-fno-sanitize-recover=undefined`, so a reported
violation terminates the process and fails its formal test.

`windows-release` builds the library and CLI without tests. Linux exposes
`linux-debug`, `linux-release`, `linux-asan-ubsan` and `linux-tsan`; Debug/sanitizer
profiles have matching CTest presets. Keep `FSX_BUILD_PARSER_TOOLS=ON`: the engine
requires the host-generated lexer. Unsupported sanitizer/platform combinations
fail configuration.

Formal fixtures and SQLite temporary files stay under the build tree. Fixture
cleanup requires successful ownership establishment, checks the fixture prefix
and does not follow directory links. Failed setup performs no recursive cleanup.

## CLI

From the configured runtime environment, use the build-tree executable:

```powershell
.\build\windows-debug\bin\fsx.exe index .\catalog.db C:/data
.\build\windows-debug\bin\fsx.exe search .\catalog.db -- 'report ext:txt;md'
.\build\windows-debug\bin\fsx.exe search .\catalog.db --json -- 'regex:"^report[0-9]+"'
.\build\windows-debug\bin\fsx.exe search .\catalog.db --sort size --desc --offset 10 --limit 20 -- 'file: size:>=1MiB'
.\build\windows-debug\bin\fsx.exe info .\catalog.db
.\build\windows-debug\bin\fsx.exe watch .\catalog.db C:/data
.\build\windows-debug\bin\fsx.exe watch .\catalog.db C:/data --duration-ms 60000 --updates 10
```

Pass the entire query as one argument after `--`. Query groups use `<...>`;
precedence is NOT (`!`), implicit AND, then OR (`|`). Quote query operands containing
spaces or operator characters, including regex alternation. Bare text is a name
substring; `*`/`?` select whole-name glob matching. `path:` glob stars do not cross
separators. `ext:txt;md`, `size:>=10MiB`, and `dm:2026-11-02` are supported; CLI dates
use UTC. The C API can supply an explicit timezone offset in minutes.

`--case` sets the default case-sensitive mode. `--jit` enables the optional PCRE2
JIT profile. `regexb:` forbids pattern-controlled UTF/UCP. Unsupported `fuzzy:`
functions return an error. `--allow-partial-text` permits regex over represented
names when a source contains unsupported names; result coverage remains partial.

Safe name-literal plans use FTS5 when the snapshot reaches the configured
entry-count floor (65,536 by default). Smaller snapshots report `scan_cost` and
scan retained memory without opening a SQLite candidate reader. The C API sets
`fts_min_entries`; 0 admits any safe FTS plan. `--fts` sets that floor to 0;
`--scan-only` forces scanning and takes precedence. The floor is a measured small
corpus heuristic, not a guaranteed crossover. Short terms, path/glob/regex-only
queries and unsafe Boolean plans scan the retained snapshot. An FTS reader finding
a database revision ahead of that snapshot also selects scanning.
JSON reports `candidate_strategy` and `candidates_examined`. Those counters measure
entries passed to the predicates, rather than elapsed time or SQLite's internal
work. All predicates still run on each candidate, with the same sorting and totals.

Exit 0 indicates success, 1 an operation/query error, 2 a command-line error, and
3 successful output with incomplete source coverage. Search/info do not create
missing or initialize empty databases. Results are observations of the indexed
revision, not authorization to open a file or a guarantee of current disk state.

`info` includes `pages` and `reused_pages` for the current snapshot. Page reuse
reduces retained memory for unchanged data. Generic scans and namespace changes
still read all database rows, and insertions/deletions can shift later dense page
boundaries. Admitted NTFS metadata-only batches patch affected pages directly.
Reuse counts describe that engine's last snapshot build; a newly opened `info`
process loads its initial snapshot and reports zero reused pages.

`watch` has Windows RDC, Linux inotify and macOS FSEvents backends. It arms the root before its first
scan and registers Linux descendants before enumeration, then rescans whenever
notifications invalidate the root. It prints and flushes
one revision summary per successful pass. Ctrl+C stops between waits/scans;
`--duration-ms` and `--updates` bound the run, with the initial pass counting as
one update. The database must be outside the watched tree. Unsupported platforms
return an error. A moved/replaced root requires reopening its watch.

Embedded hosts use `fsx_watch_open`, `fsx_watch_wait`, `fsx_watch_scan` and
`fsx_watch_close`, serializing calls on each watch and pumping notifications
themselves. Repeat scanning while dirty; retained query results stay valid.
Progress/results report consumed invalidations through `needs_rescan`; watch
info also reports overflows, native interruptions and reconciliation counts.
Keep the event-loop thread alive to avoid Windows canceling its pending I/O on
thread exit; serialized handoffs recover by marking a gap and rescanning.
Every reopened watch requires validation, and partial coverage remains explicit.
Linux requires `/proc/self/fd` for registration against verified directory fds.
`FsWatchConfig.max_directories` bounds registrations per reconciliation (default
65,536); exhaustion returns LIMIT and preserves the committed snapshot. Linux
watching has no durable cursor or automatic polling backend. The
[runtime contract](docs/runtime-contract.md#linux-inotify) documents notification
coverage and resource limits.
macOS delivers native FSEvents on a serial dispatch queue into a bounded flag
bridge; wait/scan consume it, and close waits for final stream-context release.
It has no history replay cursor. Every successful reconciliation advances the
snapshot epoch; delayed events can require additional passes after an observed IDLE.

The NTFS commands use a separate native backend:

```powershell
.\build\windows-debug\bin\fsx.exe ntfs-index .\catalog.db C:/data
.\build\windows-debug\bin\fsx.exe ntfs-index .\catalog.db C:/data --raw-mft
.\build\windows-debug\bin\fsx.exe ntfs-sync .\catalog.db C:/data
.\build\windows-debug\bin\fsx.exe ntfs-watch .\catalog.db C:/data --duration-ms 60000
```

`ntfs-index` explicitly rebuilds MFT topology, enumerates selected directory IDs
to complete hardlink aliases, and catches up USN before committing. `ntfs-sync`
resumes an existing checkpoint for one bounded journal batch; repeat until
`caught_up=true`. `ntfs-watch` starts with a rebuild and polls incremental sync,
pausing 250 ms after reaching each captured head. The initial pass counts toward
`--updates`; duration/interrupts are checked between synchronous operations.

The optional `--raw-mft` inventory reads MFT chunks directly from checked
volume extents and validates USA and directory attributes, with bounded kernel
assistance for metadata/extension records. ENUM remains the default. Raw errors
are reported without changing mode. Existing directory alias completion and USN
handoff still run. See the [raw reader protocol and validation](docs/raw-mft-reader.md).
The C API sets `FsNtfsConfig.mft_reader = FS_NTFS_MFT_RAW` after the factory call.
Raw uses two buffers and at most one overlapped read. The factory enables
FS_NTFS_RAW_PREFETCH | FS_NTFS_RAW_BATCH_DIRS: directory inventory is staged
in SQLite TEMP storage, then merged in FRN order within the same transaction.
--raw-mft-sync clears only PREFETCH. C API raw_flags=0 selects direct, serial
ingestion; the flags are independent and the config layout is unchanged by
BATCH_DIRS. See the
[prefetch protocol and measurements](docs/performance/raw-prefetch-2026-11-03.md).
Rebuild hosts against the updated FsNtfsConfig/FsNtfsInfo header.

The database must be outside the selected tree. This backend requires a local
NTFS volume with an active journal and permission to open/read the volume and
required directories. It does not enable privileges, create a journal, or switch
to RDC. Open failures report the native code and admission phase. Journal wrap,
reset or a generic scan replacing native state requires `ntfs-index` before
resuming. ReFS and V3/V4 records are outside this profile.

File rename/hardlink events reconcile saved alias parents and current parents
queried by full file ID. An unsupported or denied auxiliary hardlink lookup
reconciles the selected scope; malformed/partial replies fail without advancing
the cursor. Required indexed-object metadata reads still must succeed.
Metadata events refresh indexed objects and affected parents even when the
changed alias is outside the selected tree. They avoid directory enumeration;
with an exact base revision and unchanged complete namespace, only affected
immutable pages are copied. Other cases use full projection/loading. Descendant
paths follow directory moves; unchanged alias edges retain their entry keys.
Full-volume MFT initialization still builds and persists the volume directory
graph, and remains expensive for a small selected subtree.

Embedded hosts use `fsx_ntfs_open/rebuild/sync/close`, serialize session calls and
join callers before close. The session pins an external engine handle. Default
limits are a 64 KiB I/O buffer, 16 million MFT records and 4096 USN records per
sync. Raw mode applies the MFT bound to captured slots, including free slots,
before allocating. Engine limits bound indexed aliases and depth.

## Validation

The native content API and CLI use the actual ugrep stdin/JSON protocol. Enable
their formal tests with `FSX_BUILD_CONTENT_TESTS=ON` and an absolute
`FSX_CONTENT_TEST_UGREP` and (Linux/macOS) `FSX_CONTENT_TEST_BFS` executable paths.
Linux/macOS CI build the pinned optional search-tools manifest feature. Windows CI
qualifies the exact static LLVM-MinGW candidate from cache PR #282, commit
`b247d9c4cf738fbf5640cd4ad82832e74929e756`, and runs FSX against that tool.
That package is unreleased; production dependency/Action pins are unchanged.
Windows supports indexed windows/scopes; BFS_LIVE remains explicitly UNSUPPORTED.
There is no automatic tool discovery or fallback.

```sh
fsx content index.db /absolute/path/to/ugrep --literal 'sqlite3_open' -- 'file: ext:c'
fsx content-live /absolute/path/to/bfs /absolute/path/to/ugrep --parallel-files 2 --literal 'TODO' -- /work/src
```

The CLI emits match records and a final `live_read` summary; repeated matches on
one line remain separate records. It searches the complete metadata scope on a
snapshot pinned at submission, independent of the configured query window.
`fsx_content_scope_request(scope, ugrep, pattern)` uses the same query language
without sorting or pagination; `fsx_content_request(result, ugrep, pattern)`
continues to select exactly the supplied result window. Scope candidate totals
are incremental until `candidates_known` becomes true after enumeration.
Missing/skipped/changed files make coverage incomplete (CLI exit 3); tool errors,
resource limits or timeout are failures (exit 1). The database is not modified.
`--parallel-files 1|2` immediately after UGREP selects bounded file concurrency;
the default is 1. With 2, batches can arrive in either file order, and at most two
active or retained batches share the per-file output budgets. The final summary
reports `parallel_files`. This option carries no measured speedup claim.
See [content ownership and limits](docs/runtime-contract.md#content-jobs).

`content-live` needs no database or prior indexing. It uses an in-memory engine,
discovers hidden and ordinary files with `bfs -j1 -P -f ROOT -xdev -type f -print0`,
and opens candidates relative to a verified root fd without following links or
crossing device IDs. `fsx_content_live_request(root, bfs, ugrep, pattern)` is the
corresponding C factory; paths must be absolute with no dot/empty components.
Results report `source: "bfs_live"`, observed device/inode and metadata epoch 0;
no metadata entry key is invented. Live traversal is an observation, without a
filesystem snapshot guarantee. Discovery errors and budgets fail explicitly;
confirmed earlier file batches remain valid. Public request/info structures
changed, so callers must recompile using the factories.

Windows x64 LLVM-MinGW Debug/Release builds the library, generated lexer, native tools,
CLI and formal tests. The current suite has 104 Windows / 102 Linux engine cases, 25 dedicated raw
reader cases, two controlled-replay/measurement cases and 10 CLI cases, run
through four ordinary CTest entries. Enabled POSIX content profiles add two
CTest entries with 43 native API cases and 12 CLI cases using real ugrep/bfs.
A source-owned native process peer injects malformed path frames, traversal
errors and stalled discovery for failure/cancellation cases; it does not link FSX.
The POSIX suite also checks unrelated non-CLOEXEC fd exclusion and survival after
a matcher closes stdin early. macOS uses POSIX_SPAWN_CLOEXEC_DEFAULT and per-fd
F_SETNOSIGPIPE; Linux keeps close-from spawn actions and worker-local signal handling.
The APFS fixture expects illegal UTF-8 names to be rejected at creation; Linux
tests their discovered-path skip behavior. No substitute names are generated.
Windows content profiles run 20 native API cases and eight CLI cases (four POSIX
bfs CLI cases are explicitly skipped), including UTF-16 argv quoting, overlapped
pipe traffic, restricted handle inheritance and actual descendant cancellation.
Windows 10+ is required for the Job Object process-creation attribute. New indexes
record full 64-bit volume serials; recreate databases with older 32-bit Windows
source identities. They are reported stale rather than silently reinterpreted.
Coverage includes hardlinks, Unicode full
folding and glob boundaries, regex/JIT/limits, timezone days, snapshot retention,
rollback, persistence, FTS maintenance and concurrent senders/writer publication.
FTS/scanner comparisons cover Unicode scalar lengths and folding, phrase escaping,
Boolean recall, all sorts and pagination. Tests also cover revision fallback,
identity mismatch, legacy migration, invalid posting keys and WAL release before
result publication. A selective fixture evaluates 1 candidate versus 257 entries
in the forced scanner; this establishes reduced predicate work, not a latency claim.
Snapshot cases verify page sharing, metadata changes, 4096/4097 boundaries, empty
indexes, retained-view validity and managed-budget failure with durable rollback
and recovery. The unchanged 4101-entry fixture shares both pages with less than
1 KiB of additional managed allocations while its earlier result remains live.
Watch cases cover startup changes, create/rename/delete/metadata refresh, changes
during a scan, real native-buffer overflow, independent dirty sources, limits and
failed-scan recovery, root replacement, issuing-thread exit and STOPPING wakeup.
The CLI test waits for an initial published revision, creates a file, then checks
that a later revision persists it for an independent search invocation.
Regressions also cover a 65,001-byte nested modifier request, the accepted depth
boundary, failures retained across independent root scans, conservative validation
after an unidentified failure, and a deterministic directory-to-junction swap
during observation. Stable junctions remain searchable without traversing targets.
The asynchronous-index runtime passed all three CTest entries on Linux Debug,
Linux ASan/UBSan and Windows LLVM-MinGW Debug using the published Core 0.1.2 and
CMeta 0.1.3 SDKs. That earlier Linux revision compiled 91 engine cases;
Windows-only cases were excluded. Current platform case counts are listed above.
The independent sanitizer runner keeps default leak detection enabled. See
[CI validation](https://github.com/qigao/fsx/actions/runs/41598912885).

The added [controlled benchmark](docs/ntfs-live-validation.md#controlled-replay-and-diagnostic-counters)
compares identical modeled snapshots/USN batches in balanced mode order and
measures stage timing, actual work-pager VFS writes, queue waits and concurrent
query latency. The live job preserves these results separately from native
volume acceptance; the original four-mode timings retain their historical scope.

Repeated runs in the local sandbox observed fixture files reappearing after
successful deletion, confirmed by direct filesystem metadata reads before
rescanning. The unchanged 4096/4097 assertions pass on the independent runners;
no boundary assertion or sanitizer check was disabled for CI. Normal product
builds use the pinned manifest/source and local files cache. Strict remote ABI
restoration remains a separate mode requiring exact published packages.

NTFS protocol tests use the real SQLite/FTS/snapshot coordinator with a
deterministic native-operation provider. They cover U0/U1 changes and gaps,
split renames, hardlinks, subtree moves/name swaps, outside-alias metadata,
full FRN sequence/unsigned IDs, checkpoint resume, generic-profile invalidation,
malformed records, SQL/OOM rollback and retained results. The native capability
test under a normal Medium token reports access denied (Win32 5). The opt-in
authorized Release test now passes real full-volume MFT enumeration and selected
subtree/USN parity: approximately 4.55 million MFT records and 10,000 fixture files.
The [measurement report](docs/ntfs-live-validation.md) includes results and fixes.
Full initialization is slower for this small subtree. See the
[optimization report](docs/performance/ntfs-optimization-2026-11-03.md) for the
measured query and incremental changes. Those measurements predate the new
asynchronous pipeline. The [four-mode live report](docs/performance/async-index-2026-11-03.md)
now verifies serial/asynchronous and spillable/memory parity on a real NTFS
volume; its sample does not establish a material latency improvement.

Opt-in real-volume acceptance and measurement uses the Release
`windows-native-release` configure/build/test presets. It requires authorized
native volume access and is separate from ordinary CI tests; denied admission
fails the live test rather than skipping it. See the
[real NTFS procedure and measurements](docs/ntfs-live-validation.md).


## Asynchronous indexing

Embedded callers can submit `fsx_engine_scan_async` or
`fsx_ntfs_rebuild_async/fsx_ntfs_sync_async`, poll/wait through
`fsx_index_poll/fsx_index_wait`, request cooperative cancellation, and release
the independent job handle. Only one writer is admitted; a separate query pool
continues serving the committed snapshot. An NTFS session cannot close while
its job is pending. A timed wait does not cancel the job. Terminal publication
follows source unpinning and native/SQL cleanup. Engine close joins released
background jobs and remains BUSY/TIMEOUT while external job handles are held.

NTFS `pipeline_batches` defaults to four bounded, owned `buffer_bytes` slots;
zero is the serial control. The native reader and SQL writer overlap, with
backpressure and joined cancellation. `work_memory_pages` defaults to 4096
4-KiB pages for small identity work tables; zero selects spillable storage.
Large MFT inventory, scope paths and projection staging remain file-backed.
Main facts/FTS/USN commit together before a new query snapshot is published.
These are execution/storage choices, not a measured speedup. See the
[ownership, limits and cancellation contract](docs/runtime-contract.md#asynchronous-indexing-and-bounded-ntfs-batches).

The formal `fsx_content_benchmark_test` adds a seventh CTest entry to content
profiles. It checks identical native results while measuring serial/parallel
warm-cache throughput, first confirmed batches, cooperative cancellation and
same-engine metadata query latency. The `linux-content-release` preset provides
an optimized build; ordinary Debug/sanitizer timings are kept separate. CI
preserves raw CSV, strict summaries with tool/source hashes, and formal logs.
See [the first content performance baseline](docs/performance/content-parallel-2026-11-05.md)
for the exact corpus, observations and remaining acceptance work.

The v2 benchmark adds a POSIX direct per-file ugrep control, with four-position
rotation against both scope modes and separate spawn/lifetime/collection sums.
It runs 160 POSIX operations; Windows keeps 56 scope operations. Direct controls
use immutable fixture paths, temporary regular output files and blocking reaping;
they do not have FSX's file-version, streaming or cancellation semantics.
See [the direct control data](docs/performance/content-direct-control-2026-11-05.md)
for results and the limits of this comparison.

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
- Asynchronous queries on SaltsCore's bounded thread pool, retained results,
  cancellation, deadlines, STOPPING/close and allocation/admission limits.
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
- Cooperative Windows directory watches, armed before scanning, with bounded
  notification buffers, root identity checks, overflow/interruption recovery and
  retryable shutdown. The CLI runs the wait/rescan loop.

- Cooperative Windows NTFS sources: MFT directory topology, complete directory
  aliases, U0/U1 journal handoff, bounded USN batches, durable cursor recovery,
  subtree moves and atomic facts/FTS/checkpoint publication.

Dedicated asynchronous source queues,
raw-unit queries for invalid Unicode names and Everything import are later work.
The current collector explicitly marks skipped/unrepresentable names as partial
coverage. RDC watches reconcile whole roots and have no durable change cursor;
NTFS sources use the strict profile described below.

See the [runtime and ownership contract](docs/runtime-contract.md) for precise
boundaries and limits. The C API is in [api.h](include/fsx/api.h); CMeta value and
status declarations are in [types.h](include/fsx/types.h).

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
| `VCPKG_CACHE_REPOSITORY_ROOT` | Shared cache checkout containing `2913c530b8f12ae411e3c180e898fb6c7783462f` |

Use absolute, forward-slash paths. The hidden `vcpkg-cache` and `sdk-roots` user
presets own these roots. `.env` is private and ignored; CMake does not load it
automatically. Preserve the caller's credentials and shared read-only feed policy.
Ordinary builds do not update shared checkouts or prewarm packages.

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

`watch` arms recursive Windows directory notifications before its first scan,
then rescans whenever notifications invalidate the root. It prints and flushes
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

Windows x64 LLVM-MinGW Debug/Release builds the library, generated lexer, native tools,
CLI and formal tests. The current suite has 88 engine cases, 17 dedicated raw
reader cases and 10 CLI cases, run through three ordinary CTest entries.
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
Linux and sanitizer runs have not been performed in this environment.

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
measured query and incremental changes. Linux/sanitizer profiles were not run.

Opt-in real-volume acceptance and measurement uses the Release
`windows-native-release` configure/build/test presets. It requires authorized
native volume access and is separate from ordinary CI tests; denied admission
fails the live test rather than skipping it. See the
[real NTFS procedure and measurements](docs/ntfs-live-validation.md).

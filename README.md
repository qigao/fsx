# FileSearch (fsx)

A working C11/CMeta file-search library and CLI using SaltsCore, SQLite FTS5 and
PCRE2. The [design](docs/FileSearch-C11-设计.md) remains the roadmap; the current
implementation includes snapshot scanning and FTS5 candidate planning; later
milestones remain on the roadmap.

## Implemented

- Native directory scans with file identity, hardlink aliases, metadata and
  explicit coverage. Directory links are recorded without traversing them.
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

Page-level COW, source queues, live RDC/USN/MFT,
raw-unit queries for invalid Unicode names and Everything import are later work.
The current collector explicitly marks skipped/unrepresentable names as partial
coverage. It does not claim a complete raw-name index or live synchronization.

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

Eligible name literals use FTS5 automatically. Short terms, path/glob/regex-only
queries and unsafe Boolean plans scan the retained snapshot. A database revision
ahead of that snapshot also selects scanning. `--scan-only` forces the baseline;
JSON reports `candidate_strategy` and `candidates_examined`. Those counters measure
entries passed to the predicates, rather than elapsed time or SQLite's internal
work. All predicates still run on each candidate, with the same sorting and totals.

Exit 0 indicates success, 1 an operation/query error, 2 a command-line error, and
3 successful output with incomplete source coverage. Search/info do not create
missing or initialize empty databases. Results are observations of the indexed
revision, not authorization to open a file or a guarantee of current disk state.

## Validation

Windows x64 LLVM-MinGW Debug builds the library, generated lexer, native tools,
CLI and formal tests. The current suite has 32 engine cases and 4 CLI end-to-end
cases, run through two CTest entries. Coverage includes hardlinks, Unicode full
folding and glob boundaries, regex/JIT/limits, timezone days, snapshot retention,
rollback, persistence, FTS maintenance and concurrent senders/writer publication.
FTS/scanner comparisons cover Unicode scalar lengths and folding, phrase escaping,
Boolean recall, all sorts and pagination. Tests also cover revision fallback,
identity mismatch, legacy migration, invalid posting keys and WAL release before
result publication. A selective fixture evaluates 1 candidate versus 257 entries
in the forced scanner; this establishes reduced predicate work, not a latency claim.
Linux and sanitizer runs have not been performed in this environment.

# Current fsx runtime contract

This describes the initial implementation. The design document contains the
later journal, source-session and large-index architecture.

## Owners and state transitions

| Data path | Ownership and lifetime |
|---|---|
| scan → SQLite | One synchronous writer per engine. A second scan returns BUSY. Names/metadata are bound into prepared statements; no shell or user SQL is evaluated. |
| SQLite → snapshot | Load/validate a complete immutable array inside the write transaction. Allocation or validation failure rolls back; the old snapshot stays current. Commit precedes publication, which allocates nothing. |
| snapshot → query | Submission copies the request and retains the current snapshot under the publication mutex. Evaluation uses that snapshot; FTS candidates require a read transaction at the same revision. |
| query → result | A result holds its own snapshot reference. Result acquisition adds an independent external handle. Releasing a query does not invalidate an acquired result. |
| result → view | `fsx_result_entry` returns borrowed UTF-8 pointers valid until that result is released. Hit values are scoped to their producing result/database; cross-revision hits are rejected. |
| queue → worker | SaltsCore copies a bounded task descriptor. Successful admission owns one internal query reference through run/cancel and finalize. Rejection invokes no callback and transfers nothing. |

Queries transition QUEUED → RUNNING → READY/FAILED/CANCELLED. Cancellation requests
termination and leaves READY results intact. Wait deadline 0 polls; UINT64_MAX
waits indefinitely. Finite deadlines share `fsx_engine_now_ns`'s monotonic domain.
Regex callouts and candidate boundaries observe cancellation/deadlines; this is
not OS-level preemption of compilation or filesystem calls.

Close enters STOPPING, rejects new submissions/acquisitions and requests task
cancellation. It returns BUSY when polling with live references, or TIMEOUT at a
finite deadline. Release external queries/results and retry. The host serializes
final engine destruction against new API entry and joins its scan callers. Close
waits for writer/task finalization before freeing SQLite, snapshots and allocators.

## Snapshot and storage semantics

SQLite is the only persistent fact source. Schema/application/Unicode/text-version
admission rejects unrelated or incompatible databases. Sources have stable IDs,
canonical roots and recorded volume identity. Objects separate native identity
from entries, so hardlink aliases remain separate searchable paths. Windows
stores the complete 64-bit native file index; the second identity word is reserved.
No ReFS/128-bit journal identity support is claimed.

Each successful scan updates one source, removes its unseen entries and advances
the global revision. Stable paths retain their non-reused database entry keys.
FTS5 external-content insert/update/delete triggers run in the same transaction.
Queries use eligible positive name literals to narrow candidates through FTS5;
other plans scan their retained snapshot. The reader handshake and fallback rules
are specified below.

Snapshots currently use full immutable arrays and owned strings, rather than the
design's eventual 4096-row COW pages. Old and new snapshots both count against the
managed budget during rebuild. An allocation failure does not discard retained
snapshots or publish an incomplete replacement.

Directory enumeration is a series of filesystem observations. It follows neither
nested directory links nor Windows reparse points. Windows root spelling is
resolved from a native handle. Source coverage is partial if entries cannot be
read or represented as valid Unicode. Such names are counted and omitted;
raw-unit matching is not implemented. Strict regex requests reject unsupported
text coverage; explicit partial-text requests retain the coverage counters.
Totals are exact within the indexed coverage, not the whole changing filesystem.

A failed scan marks the in-process engine as needing rescan. Results capture that
control state at submission. There is no persistent change-stream checkpoint,
live watcher, durable gap/retry ledger or claim that an old snapshot is caught up.
Reopening recovers the last committed scan and its committed coverage.

## Limits and error behavior

- API records require exact size/version admission. Unknown flags, malformed
  UTF-8, embedded query NUL and invalid windows are rejected before evaluation.
- Defaults: 1,000,000 indexed entries, 32 external query/result handles, 2 workers,
  a queue bounded by the handle setting, 256 traversal levels and a maximum
  `offset+limit` of 100,000. Sorting uses bounded top-k storage and a stable ID tie
  break. Missing metadata stays last in either direction.
- Query input is at most 64 KiB, AST at most 1024 nodes/depth 32, regex source at
  most 4 KiB, capture count at most 256 and subjects at most 64 KiB. Glob execution
  has a finite step budget. Size arithmetic is checked; directory sizes are unknown.
- PCRE2 uses the fsx allocator, match/depth/heap limits and explicit cancellation
  callouts. Interpreter execution is the default. Optional JIT code and stack
  reservations are charged, with actual execution reported; JIT allocation peak
  is not a strict process-memory guarantee. No alternate regex library is used.
- `owned_memory_budget` defaults to 256 MiB and bounds fsx-managed heap allocations,
  including retained snapshots and PCRE2 allocator requests. It is **not** a global
  process RSS limit. Engine/provider control objects, thread stacks and SQLite's
  internal allocations are outside it. SQLite uses a 4 MiB writer page-cache target,
  row/SQL length limits and file-backed temporary storage, not a certified global
  hard heap bound. The design's total-memory admission remains future work.
- Parse, UTF, regex resource, cancellation and timeout errors are distinct from
  a successful zero-match result. Regex compile offsets map to original query
  bytes. Failures do not expose partially constructed results.

## Build/test boundary

All verification uses the project configure/build presets and formal CTest cases.
There are no installation, consumer, smoke, or temporary verification projects.
Test fixtures and SQLite spill files live under the build tree. Cleanup requires
an owned fixture root and refuses paths outside it or traversal through links.

## FTS candidate protocol

The worker exclusively owns one optional read-only SQLite connection, transaction
and cursor. It admits the database identity and revision in that transaction
before consuming any row IDs. The identity must match the engine's database;
a revision mismatch selects the retained snapshot scanner. Each candidate's
stable database key maps to the same snapshot, and all original predicates,
sorting, pagination and exact counting still execute. A result never owns a
SQLite transaction or pins its WAL.

Only positive name literals of at least three Unicode scalars are eligible.
AND may narrow through either eligible side; OR requires safe candidates for
both sides. NOT, path terms, glob/regex and short literals do not independently
narrow the candidate universe. Literal text is FTS-phrase escaped and parameter
bound. The expression is capped at 512 KiB, and candidates stream without a SQL
page limit or a materialized candidate array. Cancellation/deadlines are checked
at row boundaries and by the reader's SQLite progress handler.

A private reader has a 1 MiB cache target and no shared-cache/dirty-read mode.
There is at most one reader per executing worker. Database, budget and index
errors propagate; they are not treated as zero matches. Memory databases,
explicit scan-only profiles and legacy indexes without an identity use the
snapshot scanner. A legacy index opened with `FS_ENGINE_CREATE_DATABASE` receives
an additive identity table transactionally, preserving its revision and entries;
read-existing profiles do not migrate it. Existing schema-v1 readers tolerate
this optional table. Replacing or externally editing a live database is outside
the writer ownership contract; detected identity changes fail as stale.

`FsResultInfo.candidate_strategy` identifies FTS or the scanner reason;
`candidates_examined` counts entries passed to the predicates, including rejected
entries. `FS_ENGINE_SCAN_ONLY` (CLI `--scan-only`) provides an explicit baseline.
Counters do not include SQLite's internal posting-list work or imply a latency
improvement. Resource errors on evaluated candidates remain errors; skipped
candidates do not consume regex/glob execution budgets.

This follows SQLite's [trigram and phrase rules](https://sqlite.org/fts5.html)
and [read-transaction isolation](https://sqlite.org/isolation.html). The current
indexed universe contains only supported Unicode names; omitted native names
remain partial coverage. Future raw-name storage must add a safe scan union
before using these candidates over a larger universe.

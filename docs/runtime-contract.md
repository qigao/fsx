# Current fsx runtime contract

This describes the initial implementation. The design document contains the
later journal, source-session and large-index architecture.

## Owners and state transitions

| Data path | Ownership and lifetime |
|---|---|
| scan → SQLite | One admitted writer per engine, called synchronously or on a dedicated index worker. A competing writer returns BUSY. Names/metadata are bound into prepared statements; no shell or user SQL is evaluated. |
| SQLite → snapshot | Build/validate a complete immutable page directory inside the write transaction, retaining unchanged pages. Allocation or validation failure rolls back; the old snapshot stays current. Commit precedes publication, which allocates nothing. |
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

Failed scan obligations are retained per canonical root in bounded, engine-owned
control records (at most 256). They survive unrelated successful scans and are
cleared only by that root's successful reconciliation. A failure before a root
can be identified conservatively invalidates all tracked roots; each must then
be validated. With no tracked roots, a pending validation bit remains until a
successful first scan. Admission/allocation failure never silently drops an
existing obligation. These records and their root strings use the managed budget
and are released after the writer and handles have quiesced, not persisted as a
journal. Watch closure invalidates its own root through the same control state,
or all tracked roots if it has not yet been registered by a scan.

Directory descent stays bound to verified native directory handles. On Windows,
directory batches are read from handles, children open relative to the parent
with final-component reparse processing disabled, and a verified listing handle
remains open through observation callbacks and recursion. On POSIX, descriptor
relative stat/open operations and O_NOFOLLOW provide the corresponding boundary.
Native listing buffers have bounded capacity and validate record offsets/names.
Paths stored in entries are logical observation names, not names reopened for
descent. Concurrent namespace changes can cause partial coverage; they cannot
redirect descent through a newly substituted junction/symlink.
Windows listing buffers are 4 KiB per active directory and use the managed budget.
The traversal-depth limit bounds both buffers and open parent/child handles.

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

Snapshots use immutable pages of up to 4096 rows. Unchanged pages and their strings
are shared across revisions; changed pages are copied before commit. Shared pages
are charged once against the managed budget. An allocation failure does not
discard retained snapshots or publish an incomplete replacement.

Directory enumeration is a series of filesystem observations. It follows neither
nested directory links nor Windows reparse points. Windows root spelling is
resolved from a native handle. Source coverage is partial if entries cannot be
read or represented as valid Unicode. Such names are counted and omitted;
raw-unit matching is not implemented. Strict regex requests reject unsupported
text coverage; explicit partial-text requests retain the coverage counters.
Totals are exact within the indexed coverage, not the whole changing filesystem.

A failed scan or consumed watch notification marks the in-process engine as
needing rescan. Results capture that control state at submission. Cooperative
Windows RDC and Linux inotify watches are described below. These backends have no persistent
change-stream checkpoint or durable gap/retry ledger. Reopening recovers the last
committed scan and coverage; a reopened watch always requires a fresh scan.
The separate NTFS backend persists its USN checkpoint as described below.

## Snapshot page ownership

SQLite remains authoritative. A snapshot owns a bounded directory of immutable
pages, each containing up to 4096 entries in stable database-key order. Pages own
their strings and have atomic reference counts; snapshots retain pages, while
queries/results retain snapshots. Borrowed entry views expire with their result.
The single scan writer borrows the engine's current snapshot while loading the
replacement; publication and final destruction cannot race that writer.

The loader compares each database row with the corresponding old page. A wholly
unchanged page is retained. On its first changed row, a private page is allocated
and its unchanged prefix is copied; subsequent entries are copied into that page.
No published page is modified. Directories, new pages and copied strings count
against the managed memory budget, and each shared allocation is charged once.
Directory sizing and page sizing use checked arithmetic. Reference saturation
returns `FS_LIMIT`; allocation failure returns `FS_NOMEM`, rolls back SQLite and
releases every partially built page without changing the published revision.

Only the scan writer creates page references; reader threads may concurrently
release snapshots. Publication occurs after commit under the existing mutex,
and requires no allocation. Close waits for the writer and external handles.
Progress reports current page count and pages reused from the previous snapshot.
The directory remains dense: insertions/deletions can shift later page boundaries
and force those pages to be copied. This is page sharing, not a persistent tree
or incremental database reader; loading still visits all committed rows.

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
- Recursive `file:`/`folder:` modifiers have a separate 32-call admission guard,
  checked before descending or continuing after an earlier parse failure. The AST
  height limit also applies; 31 `file:` wrappers around a literal fit that limit.
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

## Linux content jobs

The current content runtime requires Linux/glibc 2.34 or newer (the selected
Ubuntu 24.04 CI profile). Other platforms return UNSUPPORTED
at submission, even when a native ugrep package is available. FSX remains C11;
ugrep executes as an explicitly supplied absolute executable, with no shell,
PATH search, filter commands, archives or matcher fallback. The caller owns tool
provenance/version selection. The existing pinned Linux 7.8.5 tool uses the
protocol qualified in vcpkg-cache PR #282; this does not select an unreleased
Windows package or update the infrastructure pin.

The expanded request/info structures require callers to recompile and use the
request factories. `FsContentRequest` selects exactly the supplied retained `FsResult` window,
including its original source coverage and metadata epoch. Submission copies
the tool and pattern and retains the result; the caller can release its query
and result immediately afterward. An offset/limit intentionally selects that
window.

`fsx_content_scope_request(scope, tool, pattern)` explicitly selects the entire
metadata query scope. Exactly one of `candidates` or `scope` must be provided;
an empty scope matches all entries. Submission copies the scope and captures
the current immutable snapshot and source health under the engine mutex.
The content worker compiles the existing query language once and scans that
snapshot in entry order, invoking the per-file pipeline for each match. It
allocates no all-candidate ID/path array, applies no sorting/window limit and
opens no SQLite reader or FTS cursor across consumer backpressure. This is a
fixed snapshot scan, not an automatic backend fallback. The query plan and
PCRE2 limits use the engine heap budget. Scope regex callouts and each entry
visit observe the content cancellation flag and execution deadline.

`scope_flags` accepts CASE_SENSITIVE and ALLOW_PARTIAL_TEXT; the explicit timezone
offset has the same -840..840 minute bounds as ordinary queries. Other scope
flags and scope options on a result-window request are rejected. Syntax/regex
compilation errors are asynchronous task failures, before any content process
starts, with diagnostics retained in `FsContentInfo`. Scope counts increase as
matching candidates enter the file pipeline; `candidate_count_known` becomes
true only after successful exhaustion. Failure/cancellation leaves a partial
count with known=false; result-window counts are known at submission. The CLI
uses this full scope entry and emits the flag as `candidates_known`.

New files absent from the pinned metadata snapshot are not included, even if a
new indexing revision commits during the content task. Readers do not follow
the engine's current snapshot after submission.

A lazily created one-worker content pool is independent of the query and writer
pools. At most one content task executes per engine. Each file uses one ugrep
process with `-J1`, literal `-F` or explicit PCRE2 `-P`. The child receives a fixed
C locale and no inherited interactive configuration. Linux directory components
are opened relative to fds with O_NOFOLLOW; only regular files with the selected
device/inode identity are read. Fds use CLOEXEC, and child stdio pipes are wired
explicitly in a private process group. Spawn closes all inherited fds above
stderr using glibc's close-from file action. Only this worker blocks SIGPIPE; it drains
new pending pipe signals before restoring its previous mask.

Input, stdout and stderr advance together through nonblocking pipes/poll. Input
is checked incrementally for UTF-8 (including scalars crossing read boundaries)
and NUL. A leading UTF-8 BOM is stripped; offsets refer to the resulting stream.
UTF-16/32, NUL and invalid UTF-8 are skipped with incomplete coverage. Symlinks,
directories and non-regular files are skipped. Denied/missing/replaced objects
are counted explicitly. Other native I/O errors fail the task.

The file's tool output is held until normal tool completion and post-read checks
of fd identity, size, mtime/change-time and path association. Observed changes
discard that file's records. Stat checks cannot prove that bytes belong to one
atomic content version: results explicitly mean LIVE_READ observations in the
reported read time window, not content at `metadata_epoch`. No content facts or
FTS rows are written to SQLite.

The fixed JSON record schema is decoded into typed record views. Repeated
matches on the same line produce separate records with their own byte offsets;
record counts are not unique matching-line counts. Malformed/truncated JSON
fails the file; exact escape decoding includes Unicode scalar/surrogate checks.
The default raw-output bound is 8 MiB per file (configurable 1 KiB–8 MiB), with
up to 10,000 record slots and a 64 KiB encoded frame limit. Record views point
into the decoded output buffer. Input buffers are 64 KiB; file input is capped
at 32 MiB (configurable downward). Stderr is drained with a 64 KiB cap and a
192-byte diagnostic. These managed allocations use the engine heap budget;
child RSS is separate and has no portable hard cap in this milestone.

At most one complete file batch is outstanding per task. Publication wakes
`content_wait`; acquiring it transfers batch ownership, and the worker waits
until release before opening the next file. Wait therefore signals a batch or
termination, not just whole-task completion. Consumers must acquire/release
batches while waiting. Reaching a file/output/record limit discards the current
unconfirmed file and terminates with LIMIT; earlier acquired batches remain
valid. Arrival order follows the selected candidate window or scope snapshot
entry order, with native tool
record order inside a file. There is no global content sorting/pagination.

Acquired batches retain the query and its selected snapshot and count as engine
handles. Query release cancels unfinished work and discards an unacquired batch;
acquired batches remain valid until explicitly released. Cancellation/STOPPING
wakes consumer backpressure and kills the child process group, then reaps the
owned child before terminal publication. Close remains retryable while tasks or
external query/result/batch handles exist. Execution deadlines use monotonic
time; a wait deadline does not itself cancel. Regular-file kernel reads and
native cleanup are not promised hard real-time interruption.

`FsContentInfo.complete` describes the selected window or full scope, subject to its captured
coverage: terminal OK, no skipped/changed candidates and no captured source
invalidation. It does not assert that the whole current filesystem was searched.
Published record/file counters, process start/running counts, native tool exit
status and diagnostics remain available through poll. No PDF/Office extraction,
bfs discovery or Windows/macOS process backend is
claimed by this initial API.

## Cooperative directory watches

`FsWatch` is an external engine handle. The host serializes all calls on a given
watch, and closes it only after its wait/scan caller returns. Different watches
can run concurrently; the existing single-writer admission still returns BUSY.
There are no watch threads or filename queues. On Windows each watch owns a canonical root,
native directory handle, cancellation/completion events, one OVERLAPPED and a
DWORD-aligned buffer (1–64 KiB, default 64 KiB), charged to the engine budget.
Watch count shares the engine's external-handle limit; duplicate native roots
are rejected. Linux uses the inotify protocol below; other platforms currently
return UNSUPPORTED.

Open arms recursive ReadDirectoryChangesW before returning and starts DIRTY.
Wait pumps bounded batches, coalescing every completion into a root-wide dirty
generation. Zero-byte/overflow completions also increment the gap counter and
require enumeration. Scan drains/rearms before calling the transactional scanner,
then drains again. It clears DIRTY only if no further generation was observed
and the reader remains armed. Changes during scanning therefore require another
scan. IDLE means no observed pending changes, not a journal checkpoint or complete
filesystem coverage. Partial scan coverage stays explicit and independent.
Windows cancels event-based I/O when its issuing thread exits. A consumed
ERROR_OPERATION_ABORTED while the engine remains running increments
`interruptions`, rearms on the new serialized caller and retains DIRTY until
reconciliation. This permits thread handoff without silently losing the gap.
STOPPING cancellation remains terminal; other native monitor failures are FAILED.

The host must pump watches; queries do not secretly perform I/O. Engine progress
and submitted results aggregate consumed watch invalidations with scan failures.
One source's successful scan cannot clear another active watch's dirty state.
Root identity is checked by native handle before/after scan and during pumping;
movement, replacement or monitor failure enters FAILED and requires reopening.
Database files beneath a watched root are rejected to avoid self-invalidation.
Watches have no durable cursor; every reopened watch starts with a fresh scan.

Wait uses the engine monotonic deadline domain: 0 polls, UINT64_MAX waits;
finite expiry returns TIMEOUT without discarding the watch. Engine STOPPING
signals every watch cancellation event and wakes waiters. Watches keep close
BUSY/TIMEOUT until released. Watch close cancels pending native I/O and awaits
its completion before freeing the buffer/OVERLAPPED, then releases its engine
handle. Native calls are not promised hard real-time interruption. Closing a
watch marks the engine as needing validation; a later explicit scan can validate
the remaining cached observation. Scan failures retain DIRTY; retained results
remain valid. A monitor/root check may fail after a database commit; in that case
the scan output identifies the committed pass and the session stays invalidated.
Filesystem root identity checks do not turn enumeration into an OS transaction.

### Linux inotify

Linux exposes the same cooperative API and transactional scanner. Open registers
the root and starts DIRTY. A watch owns one verified root fd, one nonblocking
CLOEXEC inotify instance, an eventfd for STOPPING and the configured 1–64 KiB
read buffer. There is no filename/watch-descriptor map: all native events are
coalesced into root-wide reconciliation obligations.

Each scan builds a fresh inotify instance, registers the root before replacing
the old instance, then registers each verified child directory fd before
enumerating it. The scanner drains events at directory entry and every 256
directory records. A scan-local latch preserves events already consumed at
those checkpoints, so an empty queue after commit cannot incorrectly clear DIRTY.
After commit, the host repeats wait/scan until a complete pass has no consumed
changes and the queue has been drained. An incomplete scan remains DIRTY.

Directory registration uses `/proc/self/fd/<fd>` to bind inotify to the already
opened directory; Linux therefore requires procfs at `/proc`. Missing procfs is
an explicit failure, not a reason to reopen an unverified pathname. The new
`FsWatchConfig.max_directories` limits registration visits per pass, including
the root (default 65,536, accepted range 1–1,048,576). Bind aliases can count
more than once; the number is an upper bound on native watches, not a unique
object count. The field is unused on Windows. Callers must recompile and use
the configuration factory with the exact current struct size.

`IN_Q_OVERFLOW` increments `overflows`, retains DIRTY and requires a new complete
watch set and scan. Root changes/unmounts fail the session; a registration limit,
allocation or scan failure is retryable while DIRTY and cannot publish a partial
transaction. A later explicit scan can retry with the same configuration, or the
host can close/reopen with a different limit. Old snapshots remain valid. Closing
the inotify instance drops all watches, including directories moved out of scope.
No system limits are changed and no polling backend is selected automatically.

Ordinary content reads do not invalidate the watch: IN_OPEN/IN_ACCESS are not
subscribed. Symlinks are recorded without following their targets. STOPPING
signals eventfd; wait uses poll in the existing monotonic deadline domain.
Directory events do not cover writes through external hardlinks, all mmap
modifications, remote filesystem changes or mount overlays. IDLE only describes
observed obligations on this backend; there is no durable journal or atomic
filesystem/content snapshot guarantee.

See Microsoft's [directory notification contract](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-readdirectorychangesw)
and [overlapped cancellation lifetime](https://learn.microsoft.com/en-us/windows/win32/api/ioapiset/nf-ioapiset-cancelioex).
The [thread exit contract](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-exitthread)
also applies to requests that use events rather than completion ports.

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

## NTFS MFT/USN source protocol

The NTFS source is an explicit, local Windows backend; it does not change token
privileges, create/delete a journal, or fall back to RDC. It admits NTFS, a volume
GUID plus full volume serial, a full 64-bit root FRN (including sequence), and V2
USN records. Other record majors, inaccessible required directories, unsupported
native names and missing journals fail explicitly. ReFS/128-bit FRNs are outside
this profile. A private provider boundary supplies the actual native operations;
formal tests exercise the same coordinator with deterministic records/faults.

A session is one external engine handle. Hosts serialize calls on a session.
Rebuild/sync reserve the existing single writer; queries retain old snapshots.
The session owns one bounded I/O buffer (4–1024 KiB; default 64 KiB); the provider
owns bounded native resources. MFT enumeration is bounded by a configured record
limit, scope/aliases by engine entry/depth limits, and journal work by a configured
records-per-batch limit. Small identity work sets use a page-limited attached memory database by default.
Large inventory/scope/projection staging remains file-backed. Neither setting
is an asserted process-wide RSS cap. Hosts join session
callers before close releases the source; STOPPING is checked at batch and
row boundaries. The optional producer uses one joined native-reader thread and a bounded owned
batch ring; asynchronous jobs also use a dedicated single-writer worker. There
are no blocking journal waits.

Rebuild captures JournalID and U0 before enumerating MFT records. The persistent
directory graph covers the volume so subtree moves can be classified. Directory
entry batches from selected directory IDs complete actual name edges, including
all hardlink aliases, without trusting one MFT name per file. Reparse entries
remain searchable but are not descended. Rebuild then captures a fixed U1 and
reconciles journal records in [U0,U1), never saving a queried head as an applied
cursor without consuming that interval. Initial inventory, entries, FTS, source
coverage, root/volume identity and cursor commit together; the complete immutable
mirror is allocated/validated before commit and published afterwards. Initial
rebuild work runs on the synchronous caller or dedicated index worker and is
cancellable at native batch/row boundaries;
the bounded sync record count does not bound full rebuild time.

Sync resumes the durable cursor. Metadata events refresh the shared object by
full FRN, including writes via aliases outside the selected tree. Create/delete
events reconcile affected parents. Directory events refresh topology by full
FRN and admit newly entering subtrees. File rename/hardlink events reconcile
saved alias parents plus current parents queried by full file ID. Split OLD/NEW
batches need no guessed rename pairing. An unavailable or denied auxiliary
hardlink query falls back to all selected directories; incomplete replies fail
the batch. Materialized paths are rewritten
for moved subtrees while retained directory-entry keys remain stable.

Only complete validated V2 records advance a batch cursor. Unknown/truncated
records, identity changes, cursor regression, journal wrap/discontinuity, source
replacement, OOM and database errors retain the old committed cursor and mirror.
A JournalID/gap or invalidated backend checkpoint requires explicit rebuild.
Derived filename state and its cursor are one SQLite transaction. The final
identity/floor is rechecked before commit. caught_up means reconciliation through
the captured head; it does not claim a transactional filesystem snapshot.

Optional schema-v1 NTFS tables are created only by a creation-enabled profile.
Legacy databases remain readable; NTFS checkpoint mode/version is admitted
separately. Native open requests GENERIC_READ on the volume and reports its
admission phase/native error through the optional borrowed open_error output.
The database must resolve outside the selected tree. A directory-profile scan of an NTFS source invalidates its checkpoint
in that same transaction, preventing later journal replay over unrelated facts.
Active sessions participate in needs_rescan aggregation; another source's success
cannot acknowledge their lag/gap. Reopened sessions always query the journal,
and stale persisted cursors never silently resume.

Native references: [MFT enumeration](https://learn.microsoft.com/en-us/windows/win32/api/winioctl/ni-winioctl-fsctl_enum_usn_data),
[USN reads](https://learn.microsoft.com/en-us/windows/win32/api/winioctl/ni-winioctl-fsctl_read_usn_journal),
[V2 record layout](https://learn.microsoft.com/en-us/windows/win32/api/winioctl/ns-winioctl-usn_record_v2).

The CLI exposes ntfs-index (explicit rebuild), ntfs-sync (one resumed batch)
and ntfs-watch (initial rebuild, then polling batches). A caught-up poll pauses
250 ms to avoid a tight loop over the volume's unrelated journal traffic,
including SQLite writes outside the selected root. Materialized filename rows
and FTS are changed only when their facts differ; saving an outside-tree cursor
can still publish a new revision. Namespace changes still project the selected
scope and load complete mirror rows. Metadata-only batches with an exact complete
base can use the page patch protocol below; scope staging remains synchronous.

Checkpoint/profile admission occurs under BEGIN IMMEDIATE, including caught-up
polls, so another database writer cannot replace a source between admission and
batch commit. A caught-up poll loads the committed mirror if another engine has
already advanced its revision; acknowledging the newer cursor with an older
mirror is forbidden.

Real-volume validation established two additional native rules. NTFS directory
enumeration can return cached duplicate metadata. After namespace reconciliation,
a bounded temporary identity set collects observed objects, changed objects and
affected parents. Each indexed identity is refreshed through OpenFileById and
GetFileInformationByHandle before projection/commit. Missing identities remove
aliases, and changed directory/reparse state prunes scope before publication.
A metadata read failure rolls back the complete batch and cursor. Invalid size
or timestamp fields remain governed by their entry-validity flags.

For an already admitted FileIdType descriptor and pinned NTFS volume,
OpenFileById can return ERROR_INVALID_PARAMETER (87) for a deleted historical
FRN. Only this identity-lookup boundary classifies it as missing. Invalid FSCTL
parameters remain errors, and final root/volume/journal validation is required
before any resulting removals commit.

Scope expansion is driven from selected parents through the
(source_id,parent) index, and is recomputed only when discovered topology
changes. Projection validates entry/object source ownership by primary-key
lookups; it does not scan all alias bindings for each entry. These changes
preserve the same atomicity, source boundaries and retained-page contracts.
The opt-in Release acceptance/measurement procedure is documented in
[ntfs-live-validation.md](ntfs-live-validation.md).

## Measured query and USN planning revision

The 10,104-entry live benchmark measured about 4.83 ms for an eligible FTS query
versus 0.31 ms for a retained-memory scan. Default query planning therefore uses
a configurable entry-count admission floor before opening a candidate reader:
fts_min_entries defaults to 65,536, 0 admits any safe FTS plan. This adds a field
to FsEngineConfig; hosts must rebuild with this header and initialize config via
fsx_engine_config(). The existing exact struct_size admission rejects old layouts.
Below the floor, SCAN_COST scans the retained immutable snapshot without touching SQLite, like
the explicit scan profile; it does not validate a newer on-disk identity. Forced
FTS tests set the floor to 0 and retain the existing UUID/revision handshake,
stale/corrupt posting rejection and WAL-release requirements. This threshold is
a tunable heuristic grounded in the measured small corpus, not a universal
crossover guarantee. No new mutable shared cache or reader lifetime is added.

USN filename changes are staged by full FRN in a bounded SQLite temporary set.
After directory topology refresh, the admitted writer dirties every saved
alias parent and every current hardlink parent returned by the native provider.
The native operation opens the exact full FRN without following reparse points,
queries FileHardLinkInformation into at most the configured I/O buffer, validates
the complete response, and synchronously borrows parent IDs through a callback.
Callbacks do not retain response views. Distinct changed FRNs are bounded by
the batch/transaction record limits. Distinct dirty parents have a separate
max_mft_records cap, checked after each staging insert (including saved parents
and complete-scope fallback); overflow rolls back rather than dropping parents.
This avoids multiplying unbounded parent staging by each file's link count.
No extra background threads or unbounded heap containers are introduced. The one writer
owns all mutations, and queries retain old immutable snapshots.

An unavailable or access-denied auxiliary hardlink lookup conservatively
reconciles the selected scope. Required selected-object metadata still must pass
its final identity reads. A partial/malformed response, another native error, OOM
or exceeded buffer/record bound does not acknowledge the change: the transaction
and cursor roll back. A missing full identity dirties saved parents, then missing aliases are reconciled. Root,
volume and journal coverage are rechecked before commit. Split rename records,
outside aliases moving into the scope, source mode invalidation, retained results
and metadata refresh keep the same correctness obligations.

Pure file data/basic-metadata reasons refresh indexed objects without enumerating
a parent. Namespace reasons dirty parents; directory facts still use checked
MFT refresh. A one-directory MFT lookup requests at most 4 KiB rather than a full
inventory buffer. Bounded native reads, cooperative cancellation and STOPPING
checks remain at record/object boundaries.

Metadata-only native batches may patch immutable snapshot pages when the admitted
database revision exactly matches the current mirror, the source checkpoint is
valid, no selected directory was enumerated, no alias/topology removal/change
occurred and the base has complete represented coverage. Other cases retain full
projection/load. The single writer owns the new snapshot directory, retains all
base pages, and lazily clones an affected page through existing budgeted page
APIs. Only numeric metadata changes in cloned entries; names, paths, folds,
source IDs, full file IDs, alias bindings and sorted entry keys must match the
old validated entry and the transaction's alias facts.

The patch input is an ordered bounded row cursor owned/finalized by the native
writer. SQLite strings are borrowed only until the next step and are compared
before advancing; page cloning copies existing owned strings. Space is bounded
by a page directory plus at most the affected existing pages, all charged to the
engine budget. With P patches, N entries, G pages and C cloned pages, work is
O(G + P log N + C*4096) plus copied strings. Old query/result snapshots remain
immutable. OOM, cancellation, invalid ordering, identity/alias mismatch or commit
failure releases the prepared snapshot and rolls back the cursor and facts.
Publication still follows COMMIT and requires no allocation. External revision
advancement, namespace changes and incomplete bases use the full loader.

## Raw MFT inventory protocol

FsNtfsConfig appends mft_reader after open_error so its struct_size changes;
old layouts are rejected before reading the new member. Hosts rebuild and use
fsx_ntfs_config(). The explicit RAW reader replaces only full-rebuild inventory.
ENUM remains the default; a raw failure never silently retries a different
backend. Both modes produce the same SQLite source/alias facts and use the same U0/U1 reconciliation,
required metadata reads and commit-before-publication protocol. A resumed source
may use either inventory mode; no second persistence format is introduced.

One synchronous writer owns the raw reader, its volume file pointer, stream maps,
bitmap, I/O chunks and scratch records. There are no new threads, queues or
shared mutable readers. Windows supplies normalized MFT metadata/extension
records through FSCTL_GET_NTFS_FILE_RECORD; checked mapping-pair decoding resolves
the unnamed MFT DATA and BITMAP streams, including ATTRIBUTE_LIST references.
Bulk FILE records are read directly from mapped volume extents, not through
FSCTL_ENUM_USN_DATA. The OS-assisted extension resolver is explicit and counted.

Only NTFS 3.1, 512..4096-byte power-of-two sectors, bounded power-of-two clusters
and 512..65536-byte FILE records are admitted. USA protection has a fixed
512-byte stride, independent of physical sector size. Every admitted raw record
must validate FILE signature, USA trailers, used/allocated sizes and record
number. Used directory/metadata attributes also validate bounds, full FRN/sequence
and base/extension ownership. File content/metadata attributes are not ingested
from raw file records; selected aliases still come from live directory handles.
Deleted and extension records are not independent namespace nodes. Directory FILE_NAME and
STANDARD_INFORMATION attributes are resolved across checked attribute lists;
DOS-only names do not replace a long name. Ambiguous directory name edges fail.

MFT slot count is capped by max_mft_records before allocation. Each map admits
at most 4096 non-sparse, uncompressed, unencrypted extents. Attribute lists have
at most 1 MiB and 4096 entries; one record has at most 65536 bytes. The bitmap is
bounded by ceil(max_mft_records/8). Entire chunks with no allocated bitmap bits
are skipped before volume I/O; partially allocated chunks are read and checked
record by record. Captured slots include both read and bitmap-skipped positions.
Chunks use the configured buffer size rounded up to record/sector size, at most
1 MiB. Stream maps, bitmap, aligned buffers, scratch and saved map verification are charged to the engine budget. Overflow,
OOM, missing/stale extension, invalid mapping or partial native read fails the
complete transaction. There is no dropping of records on capacity exhaustion.

Borrowed raw/attribute/name views last only through the synchronous callback;
SQLite copies all retained facts. Stream mapping is reloaded after inventory and
must describe the same physical prefix through the captured MFT length. Growth
is left to USN catch-up. Geometry/serial are checked again. Raw reads do not
constitute a transactional or cache-coherent filesystem snapshot: each selected
directory is verified through the live kernel MFT lookup before initial raw
enumeration, and each newly entering scope is similarly verified on every mode.
Live parent enumeration discovers descendants missing from cached/raw topology.
This prevents old on-disk parent edges from admitting moved-out directories.
The selected namespace is then refreshed/reconciled with U0/U1 as before.

STOPPING is checked between native reads, records, attributes and list entries.
Hosts serialize source operations and join before close. All reader allocations
are released before the source operation returns, including failure/callback
cancellation. Old queries keep their original immutable pages. Raw read bytes,
calls, slots, live base records and kernel-resolved records are reported as
aggregate source-operation counters. Physical-device I/O is not inferred from
these counters. Full rebuild remains synchronous.

## Raw inventory stage profiling

Optional FSX_NTFS_RAW_PROFILE=1 records elapsed nanoseconds for synchronous
volume read calls, kernel record resolution and directory callbacks. Counters
belong to the single raw-reader writer and are returned in its private stats;
they add no allocations, queues, retained views or public ABI fields. Clocks are
not read on these boundaries when profiling is disabled. The remaining elapsed
time includes parsing, bitmap traversal, allocation, copying and scheduling; it
is not an isolated parser CPU measurement. Timings are diagnostic and excluded
from normal performance distributions. Failure/cancellation keeps the same
cleanup and transaction rollback. The formal live-test diagnostic setting
FSX_NTFS_TEST_PROFILE=1 runs one rebuild, two updates and one sample per query
without changing the default three/twenty/twenty acceptance workload.

## Raw two-buffer prefetch protocol

Measured serial inventory spent 13.175 s in volume reads and 18.926 s in
directory callbacks out of 33.250 s. The next optimization overlaps those stages
without changing SQLite statements, row order, directory semantics or the
single writer. RAW config flags admit FS_NTFS_RAW_PREFETCH and
FS_NTFS_RAW_BATCH_DIRS (both enabled by the factory). Clearing PREFETCH selects
serial reads independently of graph ingestion. raw_flags=0 selects direct,
serial ingestion. The appended 64-bit flags member
increases FsNtfsConfig size; old layouts and unknown bits are rejected.

The engine-owned reader allocates exactly two aligned chunk buffers when
prefetch is enabled, plus its existing main-thread bounce buffer. One chunk is
CURRENT and borrowed by parsing/callbacks; the other is SPARE or PENDING. At most
one volume read is submitted. A whole next allocated chunk may be submitted only
when it lies in one mapped extent and meets sector alignment. Fragmented chunks
are assembled synchronously using the checked stream reader. Bitmap-free chunks
still issue no read. The source operation remains synchronous to its host.

The OS kernel produces PENDING bytes; the single writer consumes CURRENT bytes.
The private begin/finish I/O contract is: successful begin pins the destination
until exactly one finish call has drained completion; failed begin retains no
borrow. Finish, including cancel=true, must return only after the kernel has
stopped using the destination and OVERLAPPED state. A successful finish promotes
PENDING to CURRENT and swaps the old current buffer to SPARE. Callbacks cannot
retain chunk views. Stream maps and bitmap stay immutable through all in-flight
reads and are revalidated only after completion has drained.

Windows uses a separate FILE_FLAG_OVERLAPPED volume handle and one manual-reset
event. Metadata/list reads keep the original synchronous handle and its file
pointer, so concurrent I/O cannot redirect them. No application worker thread,
lock-free queue, result publication or additional SQLite writer is added. Native
begin errors and completion/short-read errors fail the transaction; there is no
silent serial retry after an admitted asynchronous operation fails.

On parser/callback failure or STOPPING, cancel the exact pending operation and
drain it with GetOverlappedResult before freeing either chunk, the event or the
volume handle. A cancellation request is not completion. The original failure
is retained if draining also reports an error. Source close still follows joined
source callers; an operation never returns with a kernel buffer borrow. A full
pipeline applies backpressure by waiting for its one pending completion, with
STOPPING checked during waits. No extra buffer or pending request is allocated.

Extra managed memory is one chunk (at most 1 MiB plus alignment). Raw requested
read counters include asynchronous submissions and successful completed bytes.
Optional read_ns measures main-thread read submission/completion wait time, not
the overlapped request's lifetime, so callback time is not double-counted.

## Raw directory staging and writer SQL cancellation

A RAW rebuild with FS_NTFS_RAW_BATCH_DIRS appends directory records to the
writer's TEMP nt_inventory table, capped independently at max_mft_records.
The callback copies borrowed name bytes into SQLite, then releases its managed
UTF-8 allocation. SQLite TEMP storage is file-backed; its page cache and sorter
memory belong to SQLite rather than the managed FsEngine allocation budget.

After raw I/O drains, one ordered INSERT SELECT merges into fsx_ntfs_dirs in
(source_id,frn,rowid) order. The rowid preserves last-emission-wins for duplicate
FRNs. The TEMP rows are deleted before reconciliation continues; all journal,
alias, graph and snapshot work remains in the original transaction. No
persistent schema migration or index removal is involved. Failures roll back
the committed graph, source cursor and revision together. Live directory
verification and alias completion remain required.

The writer connection owns one SQLite progress handler, checking STOPPING
every 2,048 VM instructions. SQLITE_INTERRUPT maps to FS_CANCELLED. A private
observer can be configured with a quiescent writer for diagnostics; it must not
change the SQLite connection and must not replace the stop handler. Query
reader connections retain their separate query cancellation handlers.
This interval is measured in VM work, not a wall-clock bound on sorting or I/O.

The formal live test can clear only BATCH_DIRS with
FSX_NTFS_TEST_DIRECT_GRAPH=1 for a same-build direct-write control. The
FSX_NTFS_TEST_RAW_SYNC setting now clears only PREFETCH, so I/O and directory
ingestion can be controlled independently. Full workload counts are unchanged.


## Asynchronous indexing and bounded NTFS batches

`fsx_engine_scan_async`, `fsx_ntfs_rebuild_async` and `fsx_ntfs_sync_async`
admit one writer job per engine. Submission copies a directory root, reserves
writer admission before returning, and uses a separate one-worker SaltsCore
pool so a long index job cannot occupy the query workers. Competing writers
return BUSY; query submission and retained results remain available. NTFS jobs
pin their source until finalization; source close and synchronous calls return
BUSY while that pin is held. A terminal job no longer borrows its source.

`fsx_index_poll/wait` return the completed scan/native information. Wait deadline
0 polls; UINT64_MAX waits. A finite wait timeout leaves the job running.
Cancel is cooperative; it is checked by traversal, native parsing, overlapped
completion, queue waits, SQL progress and the final pre-commit boundary. A
commit that wins cancellation reports success. Job release does not implicitly
cancel. Engine STOPPING cancels queued/running work and joins both pools before
freeing storage. Terminal status is published only after native I/O and SQL
cleanup and source unpinning. External job handles participate in the engine
handle/budget limits and keep close retryable.

`FsNtfsConfig.pipeline_batches` defaults to 4 and accepts 0..16. Zero uses the
serial reference path. Each slot has `buffer_bytes` owned bytes; the producer
copies the record and UTF-16 name before returning to native parsing. The SQL
consumer holds a slot until all callbacks finish. A full ring waits rather
than allocating or dropping records. Only the producer touches native operations
during inventory/journal collection; the coordinator joins it before directory
verification, alias completion, metadata reads or further native requests.
The existing raw reader can still have one pending overlapped volume read;
it drains before the producer returns or exits. Queue storage is charged to
owned_memory_budget. SQLITE/native/callback failures retain the first recorded
error, stop the peer, join it and roll back; no queued cursor is acknowledged.
FsNtfsInfo reports published-batch count and peak occupied slots for successful
operations. These counters include a slot borrowed by the consumer. Raw-reader
callback timings now measure producer copying/backpressure; SQL consumption is
on the coordinator, so those timings are not comparable to earlier direct-SQL
callback timings. Complete operation wall time still includes both.

Small sets (changes, dirty parents, refresh IDs, relinks, processed and verified
IDs) live in `fsx_work`. `FsEngineConfig.work_memory_pages` defaults to 4096
4096-byte pages (16 MiB of database pages); `max_page_count` exhaustion returns
FS_LIMIT and rolls back. Zero selects an automatically deleted spillable attached
database. Native/memory admission failures never silently change modes. This page
limit includes work-table indexes but is not a limit on all SQLite heap, sorter,
allocator overhead or process RSS. SQLite allocations remain outside the managed
fsx budget. Main facts, FTS, coverage, revision and durable USN cursor all remain
in one WAL file transaction. The attached work database is disposable, including
after a crash; it never owns recovery facts. Explicit schema qualification prevents
main-schema tables from shadowing the work sets.

`sqlite_cache_kib` controls the main connection's suggested cache size, default
4096 KiB. It is a cache target, not a hard heap limit. No speedup claim follows
from selecting memory storage or using another thread.

The alias entry-key map is captured before a rebuild clears its source, or just
before an incremental transaction first replaces a directory's names. Pure
metadata updates do not copy the complete alias namespace into `nt_old_keys`.
Namespace reconciliation still captures the original edge identities once per
transaction before enumeration, so surviving aliases and descendants keep their
entry keys. Capture failures roll back the transaction and durable cursor; a
retry starts with a fresh work set.

A new immutable query snapshot is prepared before COMMIT and published afterward.
Queries never read uncommitted work tables. Visible and durable revisions remain
the same; this implementation has no query-visible unflushed delta or full
SQLite memory mirror. Such a later mode requires explicit version pinning,
tombstones, merged totals/sorting and crash-replay rules.

All hosts must rebuild for the extended FsEngineConfig, FsNtfsConfig and
FsNtfsInfo layouts. Exact struct-size/version admission rejects older layouts.
The CLI's index and NTFS commands exercise the asynchronous API and wait for
completion; watch/RDC reconciliation retains its existing host-driven API.

# Current fsx runtime contract

This describes the initial implementation. The design document contains the
later journal, source-session and large-index architecture.

## Owners and state transitions

| Data path | Ownership and lifetime |
|---|---|
| scan → SQLite | One synchronous writer per engine. A second scan returns BUSY. Names/metadata are bound into prepared statements; no shell or user SQL is evaluated. |
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
Windows RDC watches are described below. That backend has no persistent
change-stream checkpoint or durable gap/retry ledger. Reopening recovers the last
committed scan and coverage; a reopened RDC watch always requires a fresh scan.
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

## Cooperative Windows directory watches

`FsWatch` is an external engine handle. The host serializes all calls on a given
watch, and closes it only after its wait/scan caller returns. Different watches
can run concurrently; the existing single-writer admission still returns BUSY.
There are no watch threads or filename queues. Each watch owns a canonical root,
native directory handle, cancellation/completion events, one OVERLAPPED and a
DWORD-aligned buffer (1–64 KiB, default 64 KiB), charged to the engine budget.
Watch count shares the engine's external-handle limit; duplicate native roots
are rejected. On non-Windows platforms opening a watch returns UNSUPPORTED.

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
records-per-batch limit. SQLite staging uses file-backed temporary storage and the
existing cache/row limits, not an asserted process-wide RSS cap. Hosts join session
callers before close releases the source; STOPPING is checked at batch and
row boundaries. There are no background threads or blocking journal waits.

Rebuild captures JournalID and U0 before enumerating MFT records. The persistent
directory graph covers the volume so subtree moves can be classified. Directory
entry batches from selected directory IDs complete actual name edges, including
all hardlink aliases, without trusting one MFT name per file. Reparse entries
remain searchable but are not descended. Rebuild then captures a fixed U1 and
reconciles journal records in [U0,U1), never saving a queried head as an applied
cursor without consuming that interval. Initial inventory, entries, FTS, source
coverage, root/volume identity and cursor commit together; the complete immutable
mirror is allocated/validated before commit and published afterwards. Initial
rebuild work is synchronous and cancellable at native batch/row boundaries;
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

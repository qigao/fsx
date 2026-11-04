# Real NTFS acceptance and measurement

This is an opt-in, source-owned TinyTest/CTest test in
[ntfs_live_test.c](../tests/ntfs_live_test.c), built with the versioned
`windows-native-release` profile. The ordinary Debug test profile does not
request volume access or run this workload.

## Scope

The test fully enumerates the MFT of the local NTFS volume containing its build
directory. It indexes an exclusively created subtree of 100 directories and
10,000 small files, plus a Unicode name. Its database is outside the indexed
tree. It does not assert that every protected directory on C: is indexable:
full-volume MFT enumeration and complete namespace coverage of the selected
subtree are separate claims.

The reference is the production directory scanner over exactly that subtree.
Every path/name, full file ID, flags and valid size/timestamp field is compared after the
initial builds and after real create/rename/hardlink/metadata/move/delete events.
A close/reopen phase checks durable USN continuation. Query totals are compared
with a scanner-profile engine. Retained results and descendant entry keys are
checked across updates. Journal reset/gap/fault injection remains in the
deterministic engine tests; this test does not alter the system journal.

All filesystem mutations remain under an exclusively owned build-tree fixture.
Cleanup checks its absolute fixture prefix and skips directory reparse points.
An interrupted attempt can leave its fixture for diagnosis.

## Run

Configure and build from the ordinary developer environment:

```powershell
cmake --preset windows-native-release
cmake --build --preset windows-native-release
```

Run from an authorized administrator developer shell with the same SDK roots and
runtime environment:

```powershell
ctest --preset windows-native-regression
ctest --preset windows-native-release --verbose
```

The native backend requests GENERIC_READ on the volume. A denied admission fails
this opt-in acceptance; it is never converted into a passing RDC fallback or a
skip. CTest gives the single live test 30 minutes and serializes it.

## Raw reader variant

The later explicit raw inventory mode uses the same complete acceptance body.
Set FSX_NTFS_TEST_RAW_MFT=1 for its opt-in run; see
[raw-mft-reader.md](raw-mft-reader.md) for implementation, limits and results.
The original measurements below remain an ENUM baseline.

## Asynchronous pipeline comparison

The live test defaults to the asynchronous index API. Set
`FSX_NTFS_TEST_INDEX_ASYNC=0` to call the synchronous API instead. Both execute
the same complete namespace/identity/metadata comparisons, real mutations,
retained-result checks and durable close/reopen continuation. Every asynchronous
operation waits for finalization before the provider/session is inspected or
closed; failed test cleanup cancels and joins any retained job first.

Two additional controls select `FSX_NTFS_TEST_PIPELINE_BATCHES` (0..16, default 4)
and `FSX_NTFS_TEST_WORK_MEMORY_PAGES` (0..262144, default 4096). Zero selects the
serial callback path or spillable work database respectively. CSV headers record
all three controls, with published-batch/peak occupancy comments per operation.
The test asserts that occupancy never exceeds configured capacity.

The optional `ci/ntfs-live` PR label, or the workflow's `ntfs_live` manual input,
runs the existing `windows-native-release` preset on Windows 2025 with
LLVM-MinGW. It first executes the ordinary source-owned Release tests, then the
controlled replay described below, then the full live suite in these four modes
on the same runner and build:

| Mode | Index API | Native queue slots | Work database |
|---|---|---:|---|
| `serial-file` | Synchronous | 0 | Spillable |
| `job-file` | Asynchronous | 0 | Spillable |
| `pipeline-file` | Asynchronous | 4 | Spillable |
| `pipeline-memory` | Asynchronous | 4 | 4096 pages / 16 MiB of database pages |

All modes use raw MFT inventory, the existing raw I/O prefetch and directory
staging settings, three complete rebuild samples and twenty metadata updates.
These are sequential comparisons, not randomized experiments or cold-cache
measurements. Mode order, changing volume contents and OS caches limit causal
speedup claims. Query-visible uncommitted deltas are outside this acceptance.

The job stages an exclusively owned Git worktree on the existing system volume
so its build-tree fixture uses that volume's journal. It never creates or resets
a journal, elevates a token, or substitutes a mock/RDC backend. Denied native
admission fails the opt-in job. CSV, JUnit and CTest logs are preserved per mode
before the worktree is removed. Ordinary unlabelled Debug/sanitizer jobs retain
their existing scope and do not request volume access.

### Controlled replay and diagnostic counters

`fsx_ntfs_benchmark_test` is a formal CTest entry. The ordinary suite checks a
small modeled provider plus VFS counter calibration. The live Release job also
runs it with `FSX_NTFS_BENCHMARK=1`: 100 directories, 10,000 files, four rounds,
twenty single-file metadata batches and one 10,000-file metadata batch per mode
per round. This provider supplies the same snapshot, metadata and exact V2 USN
bytes to the production parser/coordinator in every mode. It models input; it
does not replay captured device reads or measure raw NTFS throughput.

The four rounds rotate the first mode, placing every configuration in every
position once. Each operation checks its input fingerprint against the first
mode and validates every resulting path, name, identity and file size against
the model. A mode gets a fresh persistent database. All modes keep the default
work-pager cache settings. No cache flush, artificial provider delay or latency
threshold is used. Benchmark timings include the query load described below;
they must not be pooled with older runs without that load.

Both replay and live tests now emit `# stages` counters in nanoseconds:

| Counter | Wall-time boundary |
|---|---|
| `initialize_ns` | Transaction admission, checkpoint/source setup, work-table preparation |
| `inventory_ns` | Inventory collection and its SQL callbacks/queue drain |
| `journal_ns` | Journal identity check and event collection/SQL callbacks |
| `reconcile_ns` | Inventory merge, directory/link reconciliation and metadata refresh |
| `projection_ns` | Projection, final journal validation and durable cursor preparation |
| `snapshot_ns` | Snapshot load or metadata patch before commit |
| `commit_ns` | Statement finalization and SQLite COMMIT, including any WAL checkpoint |
| `publish_ns` | Committed snapshot publication and source statistics |

These stage intervals are disjoint wall times, not SQL CPU times. Provider and
callback counters overlap them and must not be added to them. `total_ns` includes
small unassigned bookkeeping/diagnostic overhead; failed operations need not
account for every stage. Producer-full and consumer-empty condition-wait counts
and nanoseconds record actual waits (including timed wakes/retries and lock
reacquisition), separately from queue occupancy. Consumer callback wall time
overlaps collection; it is zero for the direct serial path.

The test-only forwarding VFS counts successful `xWrite` calls/bytes on the
`fsx_work` database file identified through SQLite's file-control interface.
It excludes main, WAL, other TEMP files and temporary journals. An unopened
spillable pager can report zero writes; memory work storage has no VFS writes.
`work_pages` reports allocated pager pages, including reusable free pages, not
RSS or peak memory. The memory factory selects 4096-byte pages. Calibration forces
a spill with a two-page cache and verifies that main-db writes do not change
the work counter. VFS bytes describe writes handed to the OS, not physical
device traffic. Native-mode process I/O remains a separate, broader counter.

A joined query thread submits `file` searches with a ten-result window and a
one-millisecond pause between completions during each replay operation and a
separate additional live rebuild, including synchronous controls. Ordinary live
build/update samples run without this query load. The thread uses its own handles and records submit-through-result
latency, epoch and match count in a separate `*-queries.csv`. Index start/end
timestamps allow analysis to retain only queries wholly inside the operation;
queries crossing a boundary must be reported separately. A short operation can
have no wholly contained sample. Sampling stops at 65,536 queries; per-operation
sample/capacity comments expose saturation. This is one concurrent client with
closed-loop pacing, not a throughput or saturated-service benchmark. The sampler
buffer and query load affect process memory/CPU; uninstrumented historical
working-set peaks are not directly comparable.
CSV serialization occurs after the measured index interval. The ordinary live
samples retain their unloaded-query workload; the extra `concurrent_rebuild`
sample is labeled separately.

Replay CSVs/JUnit/CTest logs are uploaded under `reports/replay`, alongside the
separate live-mode artifacts. To reproduce with the existing Release presets:

```powershell
$env:FSX_NTFS_BENCHMARK = '1'
ctest --preset windows-native-regression -R '^fsx_ntfs_benchmark_test$' --verbose
```

Summarize one replay CSV and its query CSV with
`python tools/ntfs_benchmark_report.py REPLAY.csv QUERIES.csv`. The script checks
matched input fingerprints/balanced positions and reports sample counts, exact
VFS-write totals, stage medians and contained/crossing query samples separately.
It does not establish statistical significance or combine hosted runs.

## Asynchronous pipeline results

The [2026-11-03 four-mode report](performance/async-index-2026-11-03.md) preserves
real-volume correctness, all four original CSVs and the source/CI/artifact
identities. All modes passed; the sampled memory-work-table and pipeline changes
did not establish a material end-to-end latency improvement. The prior local
hardware measurements below remain separate baselines.

## Measurements

The test creates CSV artifacts in `build/windows-native-release/tests/` and
flushes phase summaries to the CTest log. Three fresh database samples record
directory scans and complete MFT/USN builds; twenty samples record one metadata
change followed by USN reconciliation and the reference rescan. Query timings
include submit, scheduling, wait and retained-result acquisition.

The operation wall clock and process CPU time are recorded separately.
Working set before/after each operation and the cumulative process peak are not
an allocation budget or a per-phase peak. Process I/O byte counters are not
physical-device traffic. Provider list time includes callbacks and SQLite
directory-item writes. MFT/read provider time covers their native calls; SQL
processing outside those calls remains in total elapsed time.

Each initial pair runs the directory scan before MFT; updates run native sync
before the reference rescan. This fixed order and shared OS caches limit causal
speedup claims. The test does not clear or explicitly warm OS filesystem caches. Fresh databases
do not imply cold OS caches. Source volume activity and the indexer's own
out-of-tree database traffic can add unrelated journal records. Record counts,
MFT batches and directory enumerations are reported with each operation.
Three build samples are descriptive; the update/query samples support sample
P50/P95, not a general tail-latency guarantee.

For diagnosis only, `FSX_NTFS_SQL_PROFILE=1` enables SQLite trace/profile and
100,000-VM-instruction progress callbacks. An optional
`FSX_NTFS_PROFILE_TIMEOUT_MS` sets a source-session deadline and interrupts SQL work and makes that
attempt fail. Profiling attempts are reported separately from final measurements;
normal measurement runs leave both settings unset.

## 2026-11-03 original baseline

The results in this section are the original baseline. The subsequent
[optimization report](performance/ntfs-optimization-2026-11-03.md) records targeted
parent reconciliation, small-index planning and metadata page patching, including
the successful 284.22 s / 52,824-assertion rerun. The original CSV is unchanged.


**Passed**: the complete elevated Release CTest acceptance finished in 302.48 s,
with 52,318 assertions and no failures/skips. The production coordinator used
real Windows operations throughout; the provider wrapper only recorded timing.
The ordinary Release engine suite (76 cases) and CLI suite (8 cases) also passed.
The complete CSV is [preserved here](performance/ntfs-live-2026-11-03.csv).

Environment: Windows 11 build 26200, AMD Ryzen 9 7940HX (16 cores/32 logical
processors), approximately 16 GB RAM, WD PC SN560 1 TB NVMe SSD, system volume
NTFS. Compiler: LLVM-MinGW Clang 23.1.4, Release `-O3 -DNDEBUG`; the configured
split Core/CMeta SDKs and SQLite 3.50.4/PCRE2 10.46 were used. Native test settings:
1 MiB I/O buffer, at most 65,536 USN records per batch and 16 million MFT records.
These override the public defaults of 64 KiB/4096 records. No intentional OS
cache flush/warmup or system-load isolation was performed.

The three complete enumerations returned **4,552,165 / 4,552,176 / 4,552,178**
MFT records. Volume counts can change between runs. All 10,101 selected initial
entries matched the reference. Real mutations produced 10,004, then 10,103 and
10,104 entries; every subsequent phase matched all valid fields. The 20 metadata
updates alternate writes through inside/outside aliases of one hardlinked file.
The engine was fully closed and reopened before the resume phase.

All times below are milliseconds. Build samples are few; only medians and ranges
are descriptive. P95 for 20 samples uses the nearest-rank estimator.

| Initial build | Samples | Median | Range |
|---|---:|---:|---:|
| Full-volume MFT + selected subtree + USN handoff | 3 | 59975.760 | 54125.209–60184.065 |
| Directory traversal of the selected subtree | 3 | 2828.791 | 2814.585–3371.833 |

MFT initialization does substantially more work here: the entire volume is
enumerated and its directory graph persisted to index this small subtree.
It is approximately 21.2 times slower in this comparison. This is not a
same-work claim that MFT enumeration itself is 21.2 times slower.

| Operation | Samples | Native median / P95 | Reference median / P95 |
|---|---:|---:|---:|
| One hardlinked object's metadata + reconciliation | 20 | 1986.020 / 3480.137 | 2532.990 / 3404.707 |
| Name literal query, 100 matches | 20 | 4.828 / 6.650 | 0.309 / 0.674 |
| Unicode folded name query, 1 match | 20 | 1.597 / 2.530 | 0.323 / 0.498 |
| Size-only query (both scan) | 20 | 0.215 / 0.426 | 0.219 / 0.312 |

Metadata median improved by approximately 21.6%, but P95 did not improve.
Twelve update batches enumerated one directory; eight conservatively enumerated
all 102 directories because a batch contained rename/hardlink reasons. Batches
also include unrelated system-volume traffic. The current policy therefore has
no uniform incremental latency advantage.

For this 10,104-entry snapshot, FTS query setup costs more than a retained-memory
scan, despite evaluating fewer candidate entries. The measurements do not
support selecting FTS solely because a safe trigram exists. Size-only queries
use scans on both engines and have comparable latency.

The mixed rename/hardlink/move phase took 6083.967 ms versus 2658.311 ms for the
reference rescan; return/delete took 3537.153 versus 2274.330 ms. One resumed
offline-create update took 896.341 ms versus 2171.002 ms for the reference rescan.
These are single samples, not latency distributions.

Maximum observed cumulative process working set was **40.51 MiB**, covering the
test process and its concurrent engine/result state, not just one index.
CPU granularity was too coarse for reliable per-query CPU medians. Raw process
I/O, owned-byte and provider-call measurements remain in the CSV.

## Problems found and fixed

- Scope expansion repeatedly traversed a roughly 725,000-directory volume graph.
  An interrupted attempt exceeded 458 s without completing its first build.
  A bounded SQL-profile attempt measured about 28.2 s in only six scope statements.
  Parent-driven indexed lookups and topology-change tracking removed that repeated
  volume traversal. The interrupted attempts are not included in final statistics.
- Directory enumeration metadata can be stale. Full-identity refresh after
  namespace reconciliation fixed a real directory modification-time mismatch;
  objects, changed identities and affected parent directories refresh together.
- Projection ownership validation searched alias bindings per entry. It now uses
  entry/object primary-key checks; a cross-source corruption regression guards
  the ownership boundary.
- OpenFileById returned Win32 87 for a deleted historical FRN on this volume.
  Missing-identity handling is restricted to that operation; invalid FSCTL
  parameters still fail and rollback.
- The test originally retained result/query handles across an engine-close
  phase. It now releases them first and bounds test close waits. Failed or
  interrupted test attempts are excluded from the final acceptance/timing data.

SQLite diagnosis used its official [trace/profile hook](https://www.sqlite.org/c3ref/trace_v2.html),
[progress handler](https://www.sqlite.org/c3ref/progress_handler.html) and
[statement counters](https://www.sqlite.org/c3ref/c_stmtstatus_counter.html).

Real selected-subtree correctness is validated. All protected paths on C: were
not indexed, and Linux/sanitizer profiles were not run. Those baseline findings motivated the implemented
[query/incremental optimizations](performance/ntfs-optimization-2026-11-03.md).
Whole-volume initialization and conservative work caused by unrelated directory
events remain performance work.

## Raw directory batch follow-up

The [directory staging report](performance/raw-directory-batch-2026-11-03.md)
preserves a same-build raw-prefetch batch/direct comparison and two SQL
diagnostics. Each full mode passed three cases / 52,866 assertions. Initial
median elapsed time was 26.253 / 27.051 s (overlapping ranges); median CPU
was 19.531 / 23.891 s. Process write I/O fell about 18%, with no claim about
physical device writes. There is no intentional warmup or isolated cache state.

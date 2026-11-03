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
ctest --preset windows-native-release --verbose
```

The native backend requests GENERIC_READ on the volume. A denied admission fails
this opt-in acceptance; it is never converted into a passing RDC fallback or a
skip. CTest gives the single live test 30 minutes and serializes it.

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

## 2026-11-03 results

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
not indexed, and Linux/sanitizer profiles were not run. Further performance work
should narrow rename/hardlink reconciliation, reduce complete projection/mirror
work and account for FTS setup cost on small snapshots.

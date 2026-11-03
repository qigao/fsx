# Raw directory ingestion, 2026-11-03

The raw reader was already overlapping I/O with a single SQLite writer.
Profiling then identified directory persistence as the next large CPU cost.
This revision stages RAW directory callbacks in SQLite TEMP storage and merges
them in FRN order, while retaining the existing graph schema and indexes.

## Evidence and implementation

A full-volume diagnostic saw about 730,000 directories. In the original direct
path, directory upserts accounted for 14.364 s out of 15.660 s in callbacks.
This supported improving SQL locality before adding parser threads or UTF
conversion pools.

| Diagnostic stage, ms | Direct before revision | Staged batch |
|---|---:|---:|
| Directory callbacks, including SQLite | 15659.725 | 3092.521 |
| SQL inside callbacks | 14364.000 | 1933.000 |
| Ordered merge after inventory | — | 9162.000 |
| TEMP row deletion after merge | — | 63.000 |
| Main-thread raw read calls/waits | 2940.433 | 6144.490 |
| Complete rebuild | 26699.204 | 27293.533 |

The diagnostic itself did not improve overall elapsed time: shorter callbacks
allow less I/O overlap, and sorting/merging adds work after inventory. Its value
is attribution. SQL trace timings are diagnostic, with different callback
frequencies in the two attempts, and are excluded from full-workload results.
Sources: [direct diagnostic CSV](ntfs-live-2026-11-03-graph-sql-before.csv) and
[batch diagnostic CSV](ntfs-live-2026-11-03-graph-sql-batch.csv).
Only trailing blanks in SQL comment lines were normalized in the copied CSVs;
measurement rows are unchanged.

The implementation preserves one writer and the original transaction.
nt_inventory is append-only, without indexes; emitted directories are capped
at max_mft_records independently of provider summary counters. The ordered
merge uses source_id, binary FRN and rowid so duplicate identities retain
last-emission-wins behavior. SQL copies names before each callback returns.
There is no new application worker, queue or growable C allocation.
SQLite uses file-backed TEMP storage; page cache and sorter memory remain
outside the managed engine budget. TEMP disk capacity is a real requirement.

Merge, live kernel verification, complete aliases, U0/U1 coverage and snapshot
preparation all precede COMMIT/publication. Failure rolls back the graph,
facts and checkpoint together. Other sources and persistent indexes remain
available. Incremental sync and ENUM ingestion retain direct upserts.

The writer now owns a SQLite progress handler checking STOPPING every 2,048 VM
instructions. A private diagnostic observer chains with it; it cannot replace
cancellation or modify the connection. SQLITE_INTERRUPT maps to FS_CANCELLED.
The interval is not a wall-clock deadline for sorting or filesystem I/O.
See the [ownership protocol](../raw-mft-reader.md#directory-batch-protocol),
[SQLite progress callback contract](https://sqlite.org/c3ref/progress_handler.html)
and [INSERT SELECT UPSERT parsing rule](https://sqlite.org/lang_upsert.html).

## Same-build full-volume comparison

Both modes used the same Release library and live acceptance executable.
Both kept raw prefetch enabled; only BATCH_DIRS differed. Batch ran first,
then direct, without concurrent regression tests. Each completed three fresh
rebuilds, twenty metadata updates and twenty samples per query.

| Metric | Batch | Direct control |
|---|---:|---:|
| Initial rebuild samples, s | 26.253 / 29.229 / 25.792 | 29.847 / 27.051 / 26.675 |
| Initial rebuild median, s | 26.253 | 27.051 |
| Initial rebuild median CPU, s | 19.531 | 23.891 |
| Initial rebuild median process write I/O, MiB | 1256.006 | 1538.615 |
| Metadata update median / P95, ms (20 samples) | 185.478 / 221.168 | 202.331 / 755.198 |
| Automatic name query median, ms | 0.293 | 0.331 |
| Forced FTS name query median, ms | 3.952 | 3.873 |
| Cumulative process peak working set, MiB | 51.496 | 51.297 |
| Whole live test, s | 151.80 | 164.80 |

The initial-build median was **3.0% lower elapsed time, 18.2% lower CPU and
18.4% lower process write I/O** in this comparison. Elapsed ranges overlap:
25.792–29.229 s batch and 26.675–29.847 s direct. Three samples on a shared
volume do not establish a stable wall-clock speedup. The CPU and I/O results
support the locality change, without implying the same reduction in device
writes. Process I/O includes SQLite, journaling, TEMP and native volume traffic;
it is not physical device telemetry. Peak working set covers the entire test
process and retained results, not an isolated graph allocation.

Incremental sync and queries use the same code in both modes. Their table
differences, including the direct update outliers, are observations on the shared
host and are not claimed benefits of batch ingestion.

Each rebuild captured 7,799,808 MFT slots and requested exactly
7,368,273,408 raw bytes in 7,218 reads, with 5,325 kernel record lookups and
6,986 prefetch submissions. Live allocated-record counts and USN counts changed
slightly as the shared volume remained active. The raw read work was unchanged.

Each mode passed **3 live cases and 52,866 assertions**, including real raw
STOPPING/drain, complete controlled-subtree comparison after namespace changes,
and durable resume. The controlled subtree contains 100 directories, 10,000
files and a Unicode entry; the test does not claim namespace access to every
protected location on the volume. Full-volume inventory and selected-subtree
namespace completeness are separate properties.

Raw [batch CSV](ntfs-live-2026-11-03-graph-batch.csv) and
[direct control CSV](ntfs-live-2026-11-03-graph-direct.csv) are preserved.
Environment: Windows 11 build 26200, Ryzen 9 7940HX / 32 logical processors,
approximately 16 GB RAM, SN560 NVMe, Clang 23.1.4 Release, a 1 MiB source buffer
and 65,536-record journal batches. No intentional warmup, cache flush or
journal reset occurred. Order effects and OS cache/load were not isolated.
Linux and sanitizer profiles were not run.

## Configuration and reproduction

The factory now enables FS_NTFS_RAW_PREFETCH | FS_NTFS_RAW_BATCH_DIRS.
The flags are independent; BATCH_DIRS adds no member or config-size change.
Clear only BATCH_DIRS to select the direct-write control, or clear only PREFETCH
to select serial reads. raw_flags=0 retains direct serial ingestion.
ENUM remains the default reader. --raw-mft-sync now clears only PREFETCH.

Use the configured, authorized Release developer environment. For the full
batch workload, set FSX_NTFS_TEST_RAW_MFT=1 and leave
FSX_NTFS_TEST_DIRECT_GRAPH and FSX_NTFS_TEST_RAW_SYNC unset/0, then run
ctest --preset windows-native-release --verbose. For the direct control, set
FSX_NTFS_TEST_DIRECT_GRAPH=1. Unset FSX_NTFS_SQL_PROFILE, and leave
FSX_NTFS_TEST_PROFILE and FSX_NTFS_RAW_PROFILE unset/0 for ordinary timing.
No intentional warmup or OS cache flush is performed.

The raw inventory_callback_ms and callback_ns fields cover staging callbacks;
the later merge is outside those fields. Complete mft_initial elapsed time
includes both inventory and merge. Comparing inventory_callback_ms alone
would overstate this revision's benefit.

## Formal checks

Four additional engine cases cover direct/batch parity across all flag
combinations, duplicate FRN ordering, merge-trigger failure and retry with an
unrelated source, the independent staging cap, and STOPPING during the bulk
merge while the prior result and committed checkpoint remain valid.

The earlier prefetch report incorrectly listed 25 raw cases; the actual
source-owned file had 19. Six dedicated cases now exercise serial/prefetch
parity with bounded overlap, immediate completion, missing asynchronous
capabilities and unknown flags, submission failure, failed completion with a
late write, and preservation of the original callback error when cancellation
drain also fails. These use the production raw parser and buffer ownership path.

The final Release regression passed 92 engine cases / 61,570 assertions,
25 raw-reader cases / 742 assertions, and 10 CLI cases, across the three formal
CTest entries. Build and regression completed successfully after adding the
six raw cases. The library and live executable used for the paired timing runs
were unchanged by that final test-only build.

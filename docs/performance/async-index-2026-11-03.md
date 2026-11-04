# Real NTFS asynchronous indexing comparison — 2026-11-03

All four modes passed the full real-volume acceptance. This run does **not**
establish a material end-to-end speedup from SQLite memory work tables or the
producer queue. Initial-build medians were 12.2–12.4 seconds; metadata-update
medians were 125–143 milliseconds. Moving work to the index worker changes
caller scheduling; these measurements include waiting for the complete commit.

## Provenance and workload

- Source HEAD: `133739146b0ae6038171f7225aa20ff29a2bf46a`.
- Tested PR merge: `6e8c6eb25f3d0ee57ed4e3a2185458f9aa81bfb4`.
- [CI #41603022260](https://github.com/qigao/fsx/actions/runs/41603022260),
  [live job #125806256012](https://github.com/qigao/fsx/actions/runs/41603022260/job/125806256012).
  Linux Debug, Linux ASan/UBSan, Windows Debug and the live Release job all passed.
- Windows 2025 hosted runner, 4 logical processors, LLVM-MinGW Clang 23.1.4,
  Release (`-O3 -DNDEBUG`), published Core 0.1.2/CMeta 0.1.3 SDKs and the pinned
  SQLite 3.50.4/PCRE2 10.46 manifest. Physical storage and CPU model were not
  captured; this is a hosted VM measurement.
- The same build and runner execute the modes in the table order. Each uses a
  fresh owned fixture: 100 directories, 10,000 files and one Unicode filename.
  The database is outside the indexed subtree on the existing system volume.
- Each mode performs three complete raw MFT rebuilds and twenty metadata updates,
  plus mixed rename/hardlink/move/delete, retained results, close/reopen USN
  continuation and query/reference parity. Raw prefetch and directory staging
  remain enabled in all modes, including the serial callback control.
- Every rebuild captured 1,371,648 MFT slots and read 1,404,739,072 provider bytes
  in 1,388 calls. Active-record counts changed from 1,355,360 to 1,355,400 across
  the run. These counters do not describe physical device traffic.
- Artifact `13216225406` contains all per-mode CSV/JUnit/CTest logs. The downloaded
  ZIP SHA-256 was verified as
  `c31cac8a0f64445ea2604a913525b40fcd21c774704ff9eff9bc1ff9b17ef968`.
  The four accepted full-workload CSV files below are preserved byte-for-byte.

## Correctness and measured latency

Every mode passed three live cases without failures or skips. The Release
source-owned engine/raw/CLI tests also passed (3/3 CTest entries). Complete
path/name/native identity/valid metadata comparisons covered 10,101 initial
entries and the later 10,004 / 10,103 / 10,104-entry namespaces. Old results kept
their revision and descendant entry keys survived directory moves. Async modes
waited for job finalization before inspecting or closing the source.

Times are milliseconds. Initial ranges contain all three rebuild samples.
Update P95 uses nearest rank over twenty samples.

| Mode | Async API | Queue slots | Work pages | Build P50 / range | Update P50 / P95 | Observed queue peak | Assertions |
|---|---:|---:|---:|---:|---:|---:|---:|
| `serial-file` | 0 | 0 | 0 | 12313.993 / 12260.220–20536.918 | 125.240 / 128.842 | 0 | 52,904 |
| `job-file` | 1 | 0 | 0 | 12196.592 / 12141.129–12296.775 | 142.844 / 682.824 | 0 | 52,982 |
| `pipeline-file` | 1 | 4 | 0 | 12360.108 / 12345.497–12397.155 | 126.393 / 130.887 | 4 | 52,982 |
| `pipeline-memory` | 1 | 4 | 4096 | 12288.971 / 12239.366–12376.000 | 125.159 / 617.216 | 4 | 52,982 |

The first serial rebuild took 20.537 seconds; its later two samples took about
12.3 seconds. OS caches were neither flushed nor explicitly warmed. Removing
that first sample, changing mode order or treating the later modes as cold would
misrepresent this run. Queue occupancy reached its configured four-slot bound.
Occupancy alone does not show whether the producer blocked; this run did not
measure queue waits or establish a throughput advantage.

With the four-slot queue unchanged, memory work pages reduced update median by
about 1% relative to spillable work storage, while P95 increased from 130.887 to
617.216 ms. The memory-mode median was within 0.1% of the serial control. This
sample supports functional parity, not a reliable memory-storage speedup or a
reliable ordering of tail latency. A second hosted-runner repeat reversed the
P95 ordering; the two hosts' timings must not be pooled.

## CPU, writes and outliers

The values below are per-operation medians. Writes are process I/O counters,
not physical disk writes; they include the admitted writer's other SQLite work.

| Mode | Build CPU ms | Update CPU ms | Update process write bytes | Query auto P50 ms | Forced FTS P50 ms |
|---|---:|---:|---:|---:|---:|
| `serial-file` | 9062.500 | 125.000 | 1,825,056 | 0.279 | 5.181 |
| `job-file` | 8968.750 | 140.625 | 1,853,800 | 0.368 | 5.286 |
| `pipeline-file` | 8906.250 | 125.000 | 1,745,060 | 0.251 | 5.207 |
| `pipeline-memory` | 9015.625 | 125.000 | 1,775,848 | 0.288 | 5.227 |

The job-only mode had five metadata batches above 300 ms; the queued memory
mode had two; the other modes had none. Those slow batches contained 27–293
journal records and performed no directory-list calls, with larger CPU/write
counts. The CSV does not identify which SQL stage caused them. Different
journal work is a confound; attributing the tails to memory storage or thread
scheduling requires further stage profiling and controlled replay.

The spillable control attaches SQLite's empty-name temporary database. SQLite
can retain its pages in the pager cache without writing them out. These CSVs
do not measure work-schema pages or writes, so they do not establish that the
control exercised storage I/O. See SQLite's [temporary database semantics](https://sqlite.org/inmemorydb.html).

Cumulative process peak working set stayed below 48.28 MiB. It covers the test
process, reference engine and retained snapshots, and is not a per-phase peak
or an engine memory guarantee. The work-page setting bounds database pages,
not all SQLite allocations or process RSS. Raw inventory wall time was about
3.4–3.5 seconds after the first sample, while complete builds took about
12.3 seconds. This suggests substantial work outside native collection remains;
it does not by itself locate that work.

Auto queries over this small snapshot retained the scan strategy and were
faster than forced FTS. The memory work database is not a query mirror, and no
query-visible uncommitted delta was exercised or claimed.

## Raw data and limits

- [serial-file](async-index-2026-11-03-serial-file.csv)
- [job-file](async-index-2026-11-03-job-file.csv)
- [pipeline-file](async-index-2026-11-03-pipeline-file.csv)
- [pipeline-memory](async-index-2026-11-03-pipeline-memory.csv)

Each mode has three build samples and twenty update samples from one hosted
runner, in fixed order with changing volume activity. There is no randomized
order, cache-state control, matched journal replay, physical I/O trace or
statistical tail guarantee. Reproduce through the opt-in live job or the
[documented presets and controls](../ntfs-live-validation.md#asynchronous-pipeline-comparison).
All earlier reports retain their own source/environment scope.

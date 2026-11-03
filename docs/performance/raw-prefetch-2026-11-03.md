# Bounded raw MFT prefetch, 2026-11-03

This follows the [raw reader baseline](../raw-mft-reader.md). The optimization
overlaps reading the next MFT chunk with parsing and persisting the current
chunk. It preserves the single SQLite writer, live directory verification,
alias completion, USN handoff and commit/publication order.

## Evidence before choosing the change

A source-owned diagnostic run measured the serial raw inventory as follows:

| Stage | Elapsed ms |
|---|---:|
| Synchronous volume read calls | 13174.516 |
| Kernel metadata/extension record calls | 532.012 |
| Directory callbacks, including UTF conversion and SQLite | 18925.819 |
| Total raw inventory | 33249.798 |

The remainder (617.451 ms) includes parsing, bitmap traversal, allocation,
copying and scheduling. It is not an isolated parser CPU measurement. Those
measurements support overlapping I/O and callbacks before adding parser threads
or changing the persistent graph schema.

## Implementation and limits

FsNtfsConfig.raw_flags defaults to FS_NTFS_RAW_PREFETCH. Setting it to 0 uses
the serial reference. The 64-bit field increases config size, so hosts must
rebuild and initialize through fsx_ntfs_config(); old layouts and unknown flags
are rejected. ENUM remains the default inventory reader and is unaffected by
the prefetch flag.

CLI --raw-mft enables raw inventory with prefetch; --raw-mft-sync selects serial
raw reads. The raw backend never changes mode after a native submission or
completion error.

Exactly two engine-budgeted aligned chunk buffers are used with prefetch.
One is current and the other is spare/pending. There is at most one outstanding
read, no application worker thread, and no unbounded queue. Extra managed
storage is one configured chunk, at most 1 MiB plus alignment.

A whole next allocated chunk is submitted only when it lies in one extent and
meets sector alignment. Fragment-spanning chunks use the existing synchronous
assembler. Bitmap-free chunks still issue no I/O. The parser consumes records
in the same order and callbacks remain on the writer thread.

Windows uses a separate overlapped volume handle with explicit offsets and
one manual-reset completion event. Ordinary metadata and attribute-list reads
retain the original synchronous handle, so they cannot race a shared seek
pointer. A callback borrows only the current completed chunk.

Successful submission pins the spare buffer and OVERLAPPED state through exactly
one completion drain. Immediate completion is reaped too. On STOPPING, parser
failure or callback failure, CancelIoEx requests cancellation and
GetOverlappedResult drains the operation before any buffer or native resource
is released. The original operation error survives a secondary drain error.
There is no retry through the serial backend after a failed admitted request.

See the [ownership/state protocol](../runtime-contract.md#raw-two-buffer-prefetch-protocol)
and Microsoft's [ReadFile](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-readfile),
[CancelIoEx](https://learn.microsoft.com/en-us/windows/win32/api/ioapiset/nf-ioapiset-cancelioex)
and [GetOverlappedResult](https://learn.microsoft.com/en-us/windows/win32/api/ioapiset/nf-ioapiset-getoverlappedresult)
contracts.

## Verification

The ordinary Release suites passed: 88 engine cases, 25 raw-reader cases and
10 CLI cases. Added cases cover bounded scheduling, the serial reference,
immediate completion, missing capabilities, begin/completion failure, late
completion during drain, spare-buffer budget exhaustion with serial recovery, preservation of the original callback error, STOPPING
and old config-layout/unknown-flag admission.

The live suite additionally starts real raw prefetch, requests engine STOPPING
from a directory callback, verifies that the operation returns CANCELLED with
its managed allocations released, then closes its source and engine. This
checks the native cancellation/drain path rather than treating a passing read
as cancellation evidence.

## Diagnostic stage comparison

| Stage, ms | Serial diagnostic | Prefetch diagnostic |
|---|---:|---:|
| Main-thread read call/submission/completion-wait time | 13174.516 | 3095.851 |
| Kernel record calls | 532.012 | 671.631 |
| Directory callbacks | 18925.819 | 17861.078 |
| Total raw inventory | 33249.798 | 22247.786 |
| Complete initial rebuild | 42437.456 | 31123.299 |

The read timer measures time spent in the calling thread, excluding the part of
a pending request that overlaps callbacks. It is not physical device latency.
The diagnostic uses one rebuild, two metadata updates and one sample per query.
Its passing results establish correctness and attribution for this attempt;
they are excluded from the final full-workload timing distribution.

Raw [serial diagnostic CSV](ntfs-live-2026-11-03-stages-serial.csv) and
[prefetch diagnostic CSV](ntfs-live-2026-11-03-stages-prefetch.csv) are preserved.
Raw directory counts were 729,828 / 729,839; volume activity changes between runs.
No intentional OS cache flush/warmup or system-load isolation was performed.

## Full workload comparison

Both full runs passed **3 cases / 52,866 assertions**, with no failures or skips.
Prefetch completed in **187.95 s**, followed by the serial control in **196.90 s**,
from the same final Release build. Each used three fresh rebuilds, twenty
metadata updates and twenty query samples, with stage profiling disabled.

| Initial rebuild / inventory | Prefetch | Serial control |
|---|---:|---:|
| Full rebuild median (ms) | 36626.334 | 39729.695 |
| Full rebuild range (ms) | 32059.908–38386.072 | 32407.841–48914.127 |
| Full rebuild median CPU (ms) | 28890.625 | 29640.625 |
| Inventory including SQL callbacks, median (ms) | 24847.924 | 31177.813 |
| Selected entries after each initial build | 10101 | 10101 |

The full rebuild median is **7.8% lower in these samples**. The inventory median
is about 20.3% lower. Sample ranges overlap, and even the paired directory-scan
medians differ (3630.648 ms versus 3159.779 ms). These are descriptive results,
not an isolated or universal speedup. The shared system volume, fixed
prefetch-then-serial order and natural OS/device caches limit causal claims.
There was no intentional cache warmup/flush, randomization or load isolation.

All selected names, paths, full file IDs, flags and valid metadata fields matched
the production directory scanner after initial builds and real USN
rename/hardlink/move/delete/metadata/resume phases. The additional native case
verified STOPPING after a real prefetch submission, cancellation/drain, managed
allocation recovery and source/engine closure.

Each initial build captured **7,799,808 slots**, requested **7,368,273,408 raw
bytes in 7,218 reads**, and resolved **5,325 kernel records** in both modes.
Prefetch submitted **6,986** of those reads asynchronously; the serial control
submitted **zero**. Thus the change overlaps the same logical read workload
rather than increasing speculative I/O beyond the next selected chunk.

Peak cumulative process working set was 50.18 MiB versus 51.46 MiB. These process
peaks do not establish a reader memory reduction: the implementation adds one
budgeted chunk (up to 1 MiB), and the tests verify that this extra capacity can
fail with NOMEM while explicitly selected serial operation still succeeds.

Raw [prefetch CSV](ntfs-live-2026-11-03-prefetch.csv) and
[same-build serial control CSV](ntfs-live-2026-11-03-prefetch-control-sync.csv)
are separate from the diagnostic CSVs and all prior baselines. Environment:
the same Windows 11 build 26200 / Ryzen 9 7940HX / SN560 NVMe / approximately
16 GB RAM / Clang 23.1.4 Release host, with a 1 MiB source buffer and 65,536
USN records per batch. No other regression suite competed with these runs.

The pipeline is used only for rebuild inventory. It does not accelerate ordinary
incremental sync or query execution. Algorithmic read/parse/SQL work is unchanged;
only one next read overlaps the writer's current chunk processing. Large volume
reads and directory persistence remain substantial costs.

## Reproduction

Use the existing authorized Release developer environment. The default live
acceptance workload remains three fresh builds, twenty metadata updates and
twenty query samples. For prefetch, set FSX_NTFS_TEST_RAW_MFT=1, leave
FSX_NTFS_TEST_RAW_SYNC unset/0, and run ctest --preset windows-native-release.
For the serial control, additionally set FSX_NTFS_TEST_RAW_SYNC=1.

Diagnostic runs set FSX_NTFS_RAW_PROFILE=1 and FSX_NTFS_TEST_PROFILE=1.
Unset both for normal timing. Profiling counters add no clock calls to their
stage boundaries when disabled. The runtime counters are private; this change
does not add public result ABI fields or another database format.

Only the selected owned subtree is asserted complete, despite full-volume MFT
inventory. Filesystem observations are not a volume transaction. Raw ingestion
still reads gigabytes, persists the volume directory graph and completes aliases
through live directory handles. Linux and sanitizer profiles were not run.

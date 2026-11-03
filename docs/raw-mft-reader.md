# Raw MFT reader

The explicit raw inventory mode is implemented in
[raw.c](../src/source/ntfs/raw.c) and the Windows adapter in
[native.c](../src/source/ntfs/native.c). It replaces the full-volume ENUM inventory
step during rebuild. SQLite remains the only persistent fact source.

## Use

From the ordinary authorized NTFS environment:

```powershell
.\build\windows-native-release\bin\fsx.exe ntfs-index .\catalog.db C:/data --raw-mft
.\build\windows-native-release\bin\fsx.exe ntfs-sync .\catalog.db C:/data
.\build\windows-native-release\bin\fsx.exe ntfs-watch .\catalog.db C:/data --raw-mft --updates 5
```

The C API initializes FsNtfsConfig with fsx_ntfs_config(), then sets mft_reader
to FS_NTFS_MFT_RAW. FS_NTFS_MFT_ENUM remains the default. Unsupported raw geometry,
a malformed record, torn read, allocation failure or failed native operation
returns an error and rolls back; raw mode never silently becomes ENUM.
Raw now uses bounded two-buffer prefetch by default. Use --raw-mft-sync or set
raw_flags=0 for serial reads. The appended raw_flags field changes config size;
rebuild hosts against this header. See the
[prefetch report](performance/raw-prefetch-2026-11-03.md).

FsNtfsConfig and FsNtfsInfo changed; rebuild hosts against the new public header.
The config field is appended after open_error so exact struct_size admission
rejects old layouts. No schema migration or second database format is needed.
A source built with raw inventory can resume with ENUM and vice versa.

## Read and consistency path

1. Query NTFS geometry/version/serial and the current MFT length. Capture U0 in
   the existing coordinator before inventory begins.
2. Fetch kernel MFT record 0. Validate and decode the unnamed DATA and BITMAP
   mapping pairs; resolve ATTRIBUTE_LIST extension references with exact full
   FRNs through FSCTL_GET_NTFS_FILE_RECORD. A lower record returned by that IOCTL
   is not accepted as the requested record.
3. Read the bounded allocation bitmap, then read allocated MFT chunks directly
   from volume extents. Entire free chunks skip I/O. Fragment boundaries can
   split a FILE record; the stream reader assembles it before parsing.
4. Validate USA at its fixed 512-byte stride, FILE header lengths/slot numbers,
   full sequence identity and relevant attribute boundaries. Inactive records
   and extension records are not independent directory nodes.
5. Extract directory topology from STANDARD_INFORMATION and FILE_NAME, resolving
   directory extension attributes. Prefer a represented long name over DOS-only
   aliases; ambiguous directory name edges fail explicitly. Raw ordinary-file
   attributes are not used as final file metadata.
6. Verify selected directory parent/name facts with the live kernel before raw
   bootstrap enumeration. Every newly entering scope gets this verification in
   either mode, including after reopening a raw-built database in ENUM mode.
   Live directory handles complete all selected file aliases and newly discovered
   descendant directories. Required metadata is refreshed by full file ID.
7. Reload the MFT stream map and require the same physical prefix through the
   captured length; recheck volume geometry/serial. Consume the existing U0/U1
   handoff, prepare the immutable snapshot, commit facts/FTS/cursor and publish.

Raw on-disk metadata can lag the kernel cache. A bitmap or raw directory graph
is therefore not proof of the current selected namespace. Directory verification
prevents an old moved-out edge from leaking into the indexed tree, and live
enumeration discovers entries absent from raw topology. The same retained
snapshot, root identity, reparse, coverage and journal-gap rules still apply.
This remains a sequence of filesystem observations, not a volume transaction.

Raw uses at most one overlapped chunk read alongside the current writer's
parsing/SQL callbacks, plus explicit kernel assistance for metadata/extension
records. The source operation stays synchronous to its host. It does not use IOCP,
parallel raw parsing, a standalone offline image importer or a general NTFS
filesystem. Selected hardlink alias completion still uses directory enumeration.

## Bounds and ownership

One source operation and one writer own the read pointer, maps, bitmap, aligned
chunks and scratch. Callback views expire on return; SQLite copies retained
facts. All heap storage uses the engine budget and is released on every exit.
There are no new worker threads or shared mutable caches.

| Resource | Limit / failure |
|---|---|
| Volume | NTFS 3.1, logical sectors 512..4096 bytes, power-of-two clusters up to 2 MiB |
| FILE record | 512..65536 bytes, power of two; USA fixed 512-byte stride |
| Captured MFT slots | max_mft_records; rejected before reader allocation |
| Bitmap | ceil(captured slots/8), within the same record cap |
| DATA/BITMAP/list map | 4096 extents each; no sparse/compressed/encrypted metadata stream |
| Attribute list | 1 MiB and 4096 entries; exact extension ownership and instance checks |
| I/O chunks | Two with prefetch, one serial; each at most 1 MiB, plus the existing bounce buffer |
| Changed stream mapping / stale identity | STALE; complete transaction and cursor roll back |
| Other parse/limit/OOM/read/callback failure | Explicit status; no partial publication |

STOPPING is checked between reads, records, attributes and list entries. Close
still requires serialized source calls and joined callers. The detailed protocol
is in [runtime-contract.md](runtime-contract.md#raw-mft-inventory-protocol).

## Formal validation

The Release build passes 88 engine cases, 25 dedicated raw-reader cases and
10 CLI cases, across three ordinary CTest entries. Raw cases cover fragmented
and negatively encoded runlists, a FILE record crossing discontiguous extents,
512-byte USA on 4096-byte sectors, resident/nonresident bitmap and attribute-list
streams, long names, full unsigned FRNs, extension ownership/sequence,
incomplete mapping, torn USA, malformed bounds, empty-chunk skipping, record and
memory capacity, relocated mapping, failed reads/callbacks and STOPPING cleanup.

Coordinator cases prove that stale raw moved-out edges are removed, stale cached
descendants are checked when their parent later enters under ENUM, failed raw
callbacks roll back existing facts/cursors, unavailable raw providers are not
downgraded and old config layouts are rejected. CLI cases exercise --raw-mft
while preserving database-outside-root admission.

## Original raw versus ENUM measurements

These measurements predate prefetch; see the
[same-build prefetch/serial report](performance/raw-prefetch-2026-11-03.md)
for the current raw implementation.

Both modes passed the same complete acceptance body from the **same final
Release build**. Raw completed in **178.87 s / 52,836 assertions**; the following
ENUM control completed in **192.77 s / 52,824 assertions**. The 12 extra raw
assertions check its counters in each of three rebuilds. Neither run failed,
skipped a case or substituted another inventory reader.

| Initial build, three samples | RAW | ENUM control |
|---|---:|---:|
| Median (ms) | 36753.724 | 43073.430 |
| Range (ms) | 32264.392–42067.805 | 40594.707–45368.539 |
| Median CPU (ms) | 26640.625 | 31203.125 |
| Median process read bytes | 8806693104 | 1436621456 |
| Selected entries after each initial build | 10101 | 10101 |

The raw initial-build median is **14.7% lower in these samples**. The comparison
includes directory SQL persistence, live directory verification/alias completion,
metadata refresh, USN catch-up and snapshot preparation, not just raw transfer.
It does not establish a general speedup or a cold-cache result. Tests run on a
shared system volume in fixed RAW-then-ENUM order; there is no intentional cache
flush/warmup, randomized order or system-load isolation. Three build samples
are descriptive, not a tail-latency distribution. The previous 60.38 s ENUM
median belongs to an earlier run, and is not used as this feature's control.

Each raw rebuild captured **7,799,808 slots**, observed about **4.578 million
live base records**, requested **7,368,273,408 raw bytes** in **7,218 volume
reads**, and made **5,325 kernel metadata/extension record queries**. These
counters exclude selected directory MFT verification queries. The unoptimized
first raw run read 7,987,981,824 bytes; skipping whole empty chunks removed
619,708,416 requested bytes (about 591 MiB) from the same captured slot range.
That first run is diagnostic context, not part of the final sample statistics.
Raw mode requests substantially more volume data than the kernel's compact ENUM
output. Process read counters include those requests and SQLite traffic; they
are not physical SSD bytes or an isolated storage-throughput measurement.

All phases matched the production directory scanner's names, paths, full file
IDs, flags and valid size/timestamp fields. This covers a controlled subtree,
not every protected namespace on C:. The raw reader observes the full allocated
MFT slot range while selected namespace coverage remains separately validated.

Post-build metadata updates use the same coordinator in both modes: 20-sample
median/P95 was 181.144/214.629 ms for RAW and 182.672/220.591 ms for ENUM. This
does not suggest a raw-specific incremental update advantage. Peak cumulative
process working set was **51.41 MiB RAW / 50.95 MiB ENUM**, covering the whole
test process and retained engines/results, not an isolated raw-reader peak.

Raw [CSV](performance/ntfs-live-2026-11-03-raw.csv) and same-build
[ENUM control CSV](performance/ntfs-live-2026-11-03-raw-control-enum.csv) are preserved
separately. Environment and native settings match the
[original host description](ntfs-live-validation.md#2026-11-03-original-baseline):
Windows 11 build 26200, Ryzen 9 7940HX, approximately 16 GB RAM, SN560 NVMe,
Clang 23.1.4 Release, 1 MiB buffer and 65,536 USN records per batch.
Linux/sanitizer and NTFS 3.0/ReFS validation were not performed.

The implementation still persists the complete volume directory graph and
completes aliases by live directory enumeration. It has not reached the
initialization cost of an in-memory-only search index. The inventory callback
timing combines raw I/O, parsing, kernel lookups and SQL callbacks; these samples
do not isolate their individual costs. Further optimization needs that profile
before choosing parallel raw reads or a different graph representation.

To reproduce with the existing opt-in Release test, from an authorized developer
shell with the configured SDK runtime environment:

```powershell
$env:FSX_NTFS_TEST_RAW_MFT = '1'
ctest --preset windows-native-release --verbose
```

Unset FSX_NTFS_TEST_RAW_MFT for ENUM. The same formal test does three fresh
rebuilds of the complete volume directory inventory, compares the controlled
100-directory / 10,000-file subtree after each phase and exercises real USN,
hardlinks, moves, deletions, metadata updates and durable resume. It never writes
raw volume data, changes privileges or resets the system journal.

CSV rows retain the original timing columns. The immediately following # raw
comment records slots, raw read bytes/calls, kernel-resolved records and inventory
time including SQLite callbacks. These raw byte counters are requested volume
reads, not physical-device traffic; inventory time includes directory SQL work.

## References

The implementation follows Microsoft's
[NTFS volume geometry](https://learn.microsoft.com/en-us/windows/win32/api/winioctl/ns-winioctl-ntfs_volume_data_buffer),
[FILE record header](https://learn.microsoft.com/en-us/windows/win32/devnotes/file-record-segment-header),
[USA protocol](https://learn.microsoft.com/en-us/windows/win32/devnotes/multi-sector-header),
[attribute/runlist layout](https://learn.microsoft.com/en-us/windows/win32/devnotes/attribute-record-header),
[attribute list](https://learn.microsoft.com/en-us/windows/win32/devnotes/attribute-list-entry)
and [kernel record lookup](https://learn.microsoft.com/en-us/windows/win32/api/winioctl/ni-winioctl-fsctl_get_ntfs_file_record).
UFFS informed the raw ingestion direction; no third-party implementation was
copied or added as a dependency.

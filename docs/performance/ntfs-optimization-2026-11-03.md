# NTFS query and incremental optimization, 2026-11-03

This follows the [original acceptance baseline](../ntfs-live-validation.md).
SQLite remains the only durable fact source. All new snapshots are prepared
before commit and published after commit. Existing retained results remain valid.

## Source study and chosen changes

The inspected public projects suggest separating ingestion cost from query cost
and avoiding work on unchanged records. These are architectural comparisons,
not reproduced third-party benchmark results. No third-party implementation was
copied or added to the fsx dependency graph.

| Project / source inspected | Relevant mechanism | Application to fsx |
|---|---|---|
| [UltraFastFileSearch reader](https://github.com/skyllc-ai/UltraFastFileSearch/blob/main/crates/uffs-mft/src/reader.rs) and [compact index](https://github.com/skyllc-ai/UltraFastFileSearch/blob/main/crates/uffs-mft/src/index.rs) | Raw MFT ingestion and contiguous record/name/link structures with identity lookup | Keep query work in the retained memory index; avoid rebuilding unchanged metadata pages. Its raw reader is a separate future ingestion design. |
| [OmniSearch scanner](https://github.com/Eul45/omni-search/blob/main/src-tauri/cpp/scanner.cpp) | FSCTL MFT enumeration into an identity map, then query snapshots | Avoid unnecessary full reconciliation. Its single-name node map is insufficient for fsx's multiple hardlink-alias contract. |
| [FSearch database](https://github.com/cboxdoerfer/fsearch/blob/master/src/fsearch_database.c) | C database ownership and asynchronous scan/search separation | Retain explicit ownership and immutable query state. It is a Unix reference, not an NTFS backend. |

Three changes were implemented:

1. **Small-index query planning.** Below 65,536 entries, a safe name-literal
   query scans retained memory and reports scan_cost without opening SQLite.
   FsEngineConfig.fts_min_entries is configurable; 0 admits any safe FTS plan,
   and CLI --fts selects that setting. --scan-only has priority. Forced FTS still
   validates UUID/revision and postings and releases WAL before publishing
   results. Cost scans use their admitted immutable snapshot, like explicit
   scans; they do not check a newer on-disk UUID.
2. **Targeted rename/hardlink reconciliation.** Saved alias parents are combined
   with all current parents returned for the full FRN by FileHardLinkInformation.
   Names are still obtained by checked parent-directory enumeration. Split
   OLD/NEW records and outside-to-inside aliases do not require guessed pairing.
   Unsupported/denied auxiliary queries use the conservative complete-scope
   fallback. Partial/malformed replies, buffer bounds and other failures roll
   back the transaction and cursor. Required metadata reads remain strict.
3. **Metadata-only page patching.** Data/basic-information changes refresh shared
   objects and affected parents without directory enumeration. With an exact
   admitted base revision and unchanged complete namespace, an ordered SQLite
   cursor validates affected aliases against old entries. Only changed pages
   are copied. Names/paths/folds/IDs are immutable; namespace changes, missing
   identities and external revision advancement use full projection/loading.

The hardlink parser follows Microsoft's
[FILE_LINKS_INFORMATION](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ntifs/ns-ntifs-_file_links_information)
and [FILE_LINK_ENTRY_INFORMATION](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ntifs/ns-ntifs-_file_link_entry_information)
layouts. Name length is a UTF-16 character count. The formal native probe checks
that both returned parent IDs match full IDs, including sequence bits.

## Resource and failure protocol

The single writer owns SQLite staging and a prepared snapshot. Native hardlink
responses use at most the configured I/O buffer, charged to the managed budget;
borrowed responses never escape callbacks. Temporary identity/parent sets use
bounded transaction staging. Distinct dirty parents are capped independently
at max_mft_records, including saved-parent and fallback inserts; overflow rolls
back the batch. No new worker, mutable shared cache or lock-free structure was
added.

For P patch rows, N entries, G pages and C changed pages, patching costs
O(G + P log N + C*4096) plus copied strings. It retains G old pages and allocates
one page directory plus C copied pages through existing budgeted APIs. Retained
results can hold older copies, governed by the same owned-memory budget.
Cancellation, failed partial copy, invalid identity/alias/order or failed commit
releases prepared state and preserves the old facts/cursor/mirror. See the
[detailed protocol](../runtime-contract.md#measured-query-and-usn-planning-revision).

## Validation and measurement

The Release engine suite passes 85 cases and the CLI suite passes 9 cases.
New cases cover the query floor boundary, targeted parents, outside aliases,
unsupported/partial hardlink responses, real parent identities, cross-page
metadata reuse, partial-copy OOM rollback/retry, retained old metadata, missing
identity removal, distinct-parent capacity and external-writer namespace
advancement. Existing forced-FTS
identity/posting/WAL and source-boundary tests continue to run.

The complete elevated real-volume CTest passed in **284.22 s**, with **2 cases,
52,824 assertions, no failures or skips**. The second case enumerates the whole
real volume, then compares the controlled 100-directory / 10,000-file subtree
against the production directory scanner through real rename/hardlink/move/
delete/metadata/resume operations. All valid fields matched after every phase.

Raw [optimized CSV](ntfs-live-2026-11-03-optimized.csv) is separate from the
unchanged [baseline CSV](ntfs-live-2026-11-03.csv). Both use the same host,
workload and native settings described in the baseline report. Volume record
counts and outside-tree activity differ. No intentional OS cache flush/warmup or
load isolation was performed; directory-before-MFT and USN-before-rescan order
is fixed. These are descriptive before/after samples, not isolated causal
speedup guarantees. The extra forced-FTS queries also change total suite work.

Times are milliseconds. P95 uses nearest rank for 20 samples.

| Operation | Original median / P95 | Optimized median / P95 |
|---|---:|---:|
| Single hardlinked object metadata sync | 1986.020 / 3480.137 | 254.763 / 1935.287 |
| Corresponding directory rescan | 2532.990 / 3404.707 | 2607.142 / 3491.966 |
| Name query, automatic plan | 4.828 / 6.650 (FTS) | 0.608 / 0.792 (memory) |
| Name query, forced FTS | 4.828 / 6.650 | 5.416 / 6.767 |
| Name query, forced scan | 0.309 / 0.674 | 0.619 / 0.872 |
| Unicode name, automatic plan | 1.597 / 2.530 (FTS) | 0.395 / 0.731 (memory) |
| Unicode name, forced FTS | 1.597 / 2.530 | 1.827 / 2.120 |
| Size-only query, both scan | 0.215 / 0.426 | 0.205 / 0.296 |

The metadata median is about 87.2% lower than the original sample and 90.2%
below the paired optimized-run rescan median. P95 is about 44.4% below the old
sample, and remains much larger than the median. All 20 optimized metadata
batches performed **zero directory enumerations**. Unrelated system-volume
directory events can still change the persisted volume graph and conservatively
force full projection/loading, even when selected aliases do not change.

Within the optimized query run, automatic name-query median is about 8.9 times
lower than forced FTS and close to forced scanning. The heuristic avoids FTS
setup on this 10,104-entry snapshot; forced FTS itself was not accelerated.
These measurements do not establish the best admission floor for larger indexes.

| Other phase | Original | Optimized |
|---|---:|---:|
| MFT initial build, 3-sample median | 59975.760 | 60378.640 |
| Selected-directory initial build, 3-sample median | 2828.791 | 3147.787 |
| Mixed rename/hardlink/move, one sample | 6083.967 | 2498.424 |
| Return/delete, one sample | 3537.153 | 1123.282 |
| Reopen/resume, one sample | 896.341 | 835.518 |

Mixed reconciliation enumerated 3 selected directories, versus 101 originally;
return/delete enumerated 3 versus 102. The initial MFT samples still enumerated
about 4.57 million records and persisted the volume directory graph. They show
no initialization improvement. Peak cumulative process working set was
**40.33 MiB** versus 40.51 MiB originally, not an index-only or per-phase peak.

The ordinary regressions and the native acceptance were run separately, so
regression tests did not compete with the measured native workload. The final
distinct-parent capacity guard was added after this measurement and passed the
85-case engine / 9-case CLI rerun, including partial-parent-set rollback; the
full-volume timing was not repeated for that guard. Linux and sanitizer profiles
were not run.

## Initialization follow-up

The explicit [raw MFT reader](../raw-mft-reader.md) was implemented after this
measurement. The following paragraph describes this report's ENUM baseline;
see the linked reader report for subsequent raw measurements.

Full initialization in the measured ENUM mode uses FSCTL_ENUM_USN_DATA, persists the volume directory
graph, completes selected directory aliases, refreshes metadata by ID and catches
up USN. This measurement predates the optional raw inventory implementation. Small-subtree directory
indexing remains an appropriate way to avoid whole-volume ingestion.

A raw MFT reader needs its own bounded USA-fixup, nonresident/runlist,
attribute-list/extension-record, bitmap, alias and concurrent-change handling
before it can preserve this backend's guarantees. Published UFFS or OmniSearch
speed claims are not fsx measurements. Large-corpus query crossover,
all-protected-path coverage, Linux and sanitizer validation remain outstanding.

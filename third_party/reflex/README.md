# RE/flex runtime from ugrep

This private source dependency is copied from
[Genivia/ugrep](https://github.com/Genivia/ugrep), version **7.8.5**, commit
`f2cc252c011460f6c75b1fb591813b4b6b59c225`.
`SOURCE.json` records every imported file and its upstream SHA256. The
BSD-3-Clause notices are retained in their source headers and in `LICENSE.txt`.
Two local changes in `lib/posix.cpp` and `lib/unicode.cpp` defer allocating lookup
tables until first use. This puts allocation failures inside the matcher's
exception boundary instead of process startup. C++11 local-static initialization
preserves safe concurrent compilation. Their local SHA256 values are recorded
alongside the upstream originals.

FSX uses the RE/flex pattern/matcher runtime for literal content matching and
its expression conversion for the existing PCRE2 content mode. The ugrep CLI,
filesystem traversal, process scheduling, compression, and output code are not
part of this target. FSX links its existing manifest PCRE2 dependency directly.

The module source list follows the core of ugrep's `lib/Makefile.am` with code
generation and indentation disabled. Optional AVX2/AVX512 objects are omitted;
only the selected target's baseline SSE2 or AArch64 NEON is enabled. This target
does not require an upstream configure, install, exported package, or consumer
project. Its C++11 implementation is kept behind FSX's private C matcher ABI.

## Ownership and bounds

The compiled pattern belongs to one content job and is immutable during file
scans. Each file scan owns its mutable matcher state. FSX limits the source
pattern to 4096 bytes and validated file input to 32 MiB. Literal scanning uses
64 KiB windows plus at most `pattern_bytes - 1` bytes of lookahead, preserving
matches crossing window boundaries and non-overlapping match selection.

The adapter and its literal window are allocated through the FSX host allocator.
All PCRE2 contexts, code and matching state use the same allocator. RE/flex's
internal C++ containers, pattern compilation, and initial matcher buffer use
their upstream allocators: **these allocations are not charged to FSX's owned
memory budget**. No global C++ allocator is replaced. Allocation failures are
caught at the C boundary and reported as `FS_NOMEM`.

FSX owns file reads, validation, content buffers, full-line result copies,
candidate queues, cancellation state and post-read identity checks. No borrowed
RE/flex result pointers escape a synchronous callback. Literal scans check
cancellation before each bounded window and match. PCRE2 scans use callouts,
match/depth/heap limits and no start optimizations; patterns are applied to the
complete validated subject so lookbehind and cross-line matches remain valid.

## Matching semantics

Results preserve the current ugrep `%n`, `%b`, `%J` content protocol: line number
and byte offset identify each match, while text contains the complete line(s)
covering it, without an unmatched terminating LF. Repeated matches on one line
are separate records. Empty patterns select every real input line; empty files
produce no records. The ugrep special cases `^`, `$`, `^$`, `.*`, and `.+` retain
their line-selection behavior. Other PCRE2 empty matches follow ugrep's default
suppression unless the source pattern starts with `^` or ends with `$`.

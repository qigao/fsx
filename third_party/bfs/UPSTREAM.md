# bfs traversal subset

Source: <https://github.com/tavianator/bfs>

- Version: 4.1.7
- Commit: `6122b7fdab2a5adca17a6f94fb97c1206d8d4c68`
- License: 0BSD; see `LICENSE` and the original notices in each source file.
- `upstream.json` records the original SHA-256 of every imported upstream file.

This is a private source dependency of FSX, not a separately published libbfs
API. FSX calls the original `bftw()` implementation through `bridge.h`. The
bridge selects breadth-first, no-follow, single-device traversal and zero
additional I/O threads. It returns regular files and stops on the first
traversal error. The walker owns at most 32 cached descriptors in the current
FSX adapter; the root and content-file descriptors are owned by FSX separately.

## Deliberate source boundary

The imported sources are the traversal, directory/stat facades, allocation,
strings, trie, I/O queue, thread and mount helpers needed to link `bftw()`.
The CLI parser, expression evaluator, optimizer, actions, signal hooks and
process launcher are not imported. Three upstream translation units are trimmed:

- `bfstd.c`: retains only filesystem/error wrappers used by this dependency
  closure. Interactive prompts, regex, terminal, locale-printing and unrelated
  utilities are omitted.
- `diag.c`: retains only low-level diagnostics and invariant failure handling.
- `dstring.c`: omits printf and shell-escaping helpers.

The traversal algorithms themselves are unchanged. `namespace.h` gives their
external functions private FSX-prefixed names. `config.h` is an explicit
Linux/macOS configuration: optional predicate libraries and io_uring are off;
Linux uses getdents64, and stat metadata uses fstatat on both platforms. Mount
boundaries therefore use device IDs, matching FSX's existing live-discovery
contract. No generated configure probes, install rules or consumers are added.

The vendor target uses C17 with the compiler extensions required by bfs; its
forced prelude and configuration are confined to that target. The bridge header
is plain C11 and does not include upstream headers.

## Ownership, budgets and cancellation

All explicit vendor heap allocations (`malloc`, `calloc`, `realloc`, aligned
allocation, `strdup`, `strndup`) are redirected inside the private target to the
host allocator. FSX supplies `fsx_alloc()`/`fsx_free()`, so walker queues, paths,
arenas, allocation headers and alignment overhead count against the engine's
owned-memory budget. Temporary reallocations account for both allocations until
the old allocation is released. Libc's internal directory-stream allocations
on macOS remain libc-owned, bounded by the walker descriptor limit.

A thread-local allocation scope is installed only for the synchronous walk and
is saved/restored for nested calls. Each allocation records its deallocator;
freeing does not consult the current scope. Separate walks on different threads
do not share mutable host state. The bridge never enables bfs I/O threads.

The callback borrows its path only for the call. FSX copies paths when placing
them on the asynchronous candidate queue. Traversal metadata never replaces
FSX's verified root-relative open, identity and read/post-read checks.

Cancellation/deadlines are cooperative at callbacks. A currently blocked
filesystem syscall has no hard interruption guarantee. Callback stop, candidate
or path-byte limit, traversal errno and normal exhaustion are distinguished by
the FSX adapter; the bftw return value alone does not establish completeness.
Discovery byte accounting is the sum of admitted path lengths plus one NUL per
candidate, even though the native bridge does not serialize a NUL stream.

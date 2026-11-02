# FileSearch (fsx)

C11/CMeta indexed file search based on [the design](docs/FileSearch-C11-设计.md).
This initial project contains the CMake build and dependency scaffold. The engine,
CLI, database schema and native tests have not been implemented. Consequently,
the current build has no product compilation targets; install publishes these
documents. Source-owned targets and CTest cases will be added with real code.

## CMake origin

Adapted from `qigao/salts-core` commit
`a2e3f2315f1de2194bd6f7fb257526c98074a2d8`:

- `CMakeLists.txt`: C-only project, install directories and module ownership.
- `CMakeOptions.cmake`: project options and sanitizer selection.
- `CMakePresets.json` / `CMakeUserPresets.json`: hidden Ninja bases and public
  configure/build/install entry points, with isolated build/install directories.
- `cmake/Sanitizers.cmake`: copied with `SALTSCORE_` renamed to `FSX_`.

No Core implementation, Core-specific feature switches, release publisher or
cache warmer is copied. The shared Qigao toolchain remains in vcpkg-cache.
`CMakeUserPresets.json` is a versioned project file, not an ignored local file.

## Dependencies

Provide explicit, compatible installed SDK roots through the parent environment:

| Environment | Required content |
|---|---|
| `SALTSCORE_ROOT` | Split SaltsCore SDK exporting Core, Platform, Concurrency, FS and Unicode |
| `CMETA_ROOT` | Compatible split CMeta SDK exporting `CMeta::CMeta` |
| `VCPKG_ROOT` | Canonical vcpkg checkout/tool selected by shared setup |
| `VCPKG_CACHE_REPOSITORY_ROOT` | Shared cache checkout including commit `2913c530b8f12ae411e3c180e898fb6c7783462f` |

Use forward slashes in these environment paths. Cached package paths are cleared
before resolving the two SDKs; other installed SDKs are not searched as fallbacks.
The old monolithic Salts SDK is not a Core/CMeta dependency source. SDKs must match
the target architecture, compiler ABI and selected build profile.

`vcpkg.json` owns the product's PCRE2 and SQLite FTS5 dependencies at baseline
`b1b19307e2d2ec1eefbdb7ea069de7d4bcd31f01`. PCRE2 keeps its default desktop JIT
feature. Future adapters privately link `PCRE2::8BIT` and
`unofficial::sqlite3::sqlite3`. Configuring requests these dependencies; runtime
FTS5/trigram, Unicode and JIT admission will belong to the implementation.
TRE is not a dependency.

Optional ugrep/bfs use the shared overlay ports. Configure with
`-DFSX_BUILD_SEARCH_TOOLS=ON` to install the manifest's `search-tools` feature.
These tools are separate executables, never linked into the C11 engine.
ugrep currently supports Linux/macOS and MSVC Windows; bfs supports Linux/macOS.
Neither is installed by this feature for Windows LLVM-MinGW.

## Windows x64

Use the shared `setup-llvm-mingw` action with `target: x64`,
`build-type: toolchain-only`, and `obfuscator: false`. Preserve its
`LLVM_MINGW_TOOLCHAIN_FILE`, `LLVM_MINGW_DEPENDENCY_TOOLCHAIN_FILE` and
`LLVM_MINGW_HOST_DEPENDENCY_TOOLCHAIN_FILE` environment outputs. Do not substitute
MSVC-built SDKs for the LLVM-MinGW SDKs. Local builds use the same installed
compiler setup and inherit `PATH`; run the commands from a VS developer shell.

Both shared vcpkg setup actions at the pinned commit provide native pkgconf.
If setting up a local shell without those actions, call the same helper and
import its returned environment before configuring:

```powershell
# Set the four dependency roots above and the LLVM-MinGW setup outputs first.
$native = python -c "import json,os,sys; from pathlib import Path; sys.path.insert(0,sys.argv[1]); from windows_pkgconf import prepare_pkgconf; print(json.dumps(prepare_pkgconf(Path(os.environ['TEMP']))))" "$env:VCPKG_CACHE_REPOSITORY_ROOT/scripts" | ConvertFrom-Json
if ($LASTEXITCODE -ne 0) { throw 'Native pkgconf setup failed' }
$native.psobject.Properties | ForEach-Object {
    [Environment]::SetEnvironmentVariable($_.Name, [string]$_.Value, 'Process')
}
cmake --preset windows-release
cmake --build --preset windows-release
cmake --build --preset install-windows-release
```

`windows-debug` exposes the corresponding Debug configure/build/install presets.
Windows presets retain native `PKG_CONFIG` through `VCPKG_KEEP_ENV_VARS`; setup
fails before dependency installation if the native executable is missing.
Runtime `PATH` includes the active SDK/vcpkg DLL directories and inherited PATH;
CMake does not copy DLLs after a build.

## Linux x64

Use native Clang, Ninja and pkg-config, set the same dependency roots, and run:

```sh
cmake --preset linux-release
cmake --build --preset linux-release
cmake --build --preset install-linux-release
```

`linux-debug`, `linux-asan-ubsan`, and `linux-tsan` are also available, each with
matching build and `install-<profile>` presets. Sanitizers follow salts-core's
Linux/Clang policy and incompatible sanitizer combinations fail configuration.
There are no fabricated tests, consumer probes or benchmark targets in this
scaffold. `BUILD_TESTING` reserves the native CTest/TinyTest graph for future
source-owned tests; no test presets claim an existing suite.

## Planned source ownership

The design's implementation boundaries are `include/fsx`, `src/model`,
`src/index`, `src/query`, `src/changes`, `src/storage`, `src/platform`,
`src/source/win32`, `src/cli`, and `tests`. Introduce each CMake subdirectory when
its implementation exists, with explicit source lists. SQLite remains the sole
persistent fact source; source collection, writer publication and query snapshots
remain fsx responsibilities.

## Initial validation

On 2026-11-02, the Windows x64 LLVM-MinGW Release configure, build and install
presets completed with the installed split SDKs, PCRE2 10.46 and SQLite 3.50.4
with FTS5. Dependency installation did not acquire MSYS2. The build currently
has no product sources to compile, and no runtime or search tests were run.
Linux and sanitizer profiles have not been executed.

vcpkg emitted import-library warnings for its MinGW dependency builds; the
`.dll.a` files exist and the package imports resolved. Product link/runtime
qualification remains part of implementing the actual engine targets.

# FileSearch (fsx)

C11/CMeta indexed file search based on [the design](docs/FileSearch-C11-设计.md).
The project currently provides dependency configuration, reusable CMake helpers,
re2c discovery and a real native Lemon build. The engine, CLI, database schema
and formal search tests have not been implemented.

## Build policy

Use the versioned configure/build presets. Run formal native tests through CTest
when their implementations are added. This project has no install rules, install
presets, package exports, consumer projects or install/consumer validation paths.
Do not add synthetic consumers, smoke scripts, configuration self-tests or dummy
targets. Production dependency resolution remains part of normal configuration.

## Reused project files

The initial root CMake and presets were adapted from `qigao/salts-core` commit
`a2e3f2315f1de2194bd6f7fb257526c98074a2d8`. The supplied common files are now wired
into the project:

| File | Responsibility |
|---|---|
| `cmake/CmakeUtils.cmake` | Generic target, grammar, test and benchmark helpers |
| `cmake/FindTools.cmake` | Select host re2c from `RE2C_ROOT`, its Unicode data and the Lemon template |
| `tools/lemon/` | Build the supplied Lemon source as `lemon` / `Fsx::Lemon` |
| `cmake/Sanitizers.cmake` | Core's Linux/Clang sanitizer policy, using `FSX_` options |
| `.env.example` | Private local environment settings, with no credentials |
| `vcpkg-configuration.json` | Shared registry reference aligned with the selected cache contract |

Generic helpers remain independent of fsx's dependencies and paths. Project
options belong to `CMakeOptions.cmake`; source-owned subdirectories will declare
real targets with explicit sources. `cmake_add_executable`, `cmake_add_test` and
`cmake_add_benchmark` take the target name first. `cmake_add_grammar` receives
explicit `RE2C_EXECUTABLE`, `LEMON_TARGET` and `LEMON_TEMPLATE` arguments.
The supplied Lemon sources and template retain their original notices.

`FSX_BUILD_PARSER_TOOLS=ON` is the default. `FindTools.cmake` requires native re2c
4.6 or newer and its Unicode data. Cross builds require
`FSX_HOST_LEMON_EXECUTABLE` from a completed host build; a target executable is
never selected as a host code generator. Configure with
`-DFSX_BUILD_PARSER_TOOLS=OFF` when building a graph without parser tools.

## Dependencies and cache

The hidden `vcpkg-cache` and `sdk-roots` user presets own environment roots and
cache paths. Shared presets contain compiler/platform settings. Keep
`CMakeUserPresets.json` in version control.

| Parent environment | Required content |
|---|---|
| `SALTSCORE_ROOT` | Compatible split Core SDK with Core, Platform, Concurrency, FS and Unicode |
| `CMETA_ROOT` | Split CMeta SDK exporting `CMeta::CMeta` |
| `RE2C_ROOT` | Native host re2c package with `bin/` and `share/re2c/stdlib/` |
| `VCPKG_ROOT` | Canonical vcpkg checkout/tool selected by the shared setup |
| `VCPKG_CACHE_REPOSITORY_ROOT` | Shared cache checkout containing `2913c530b8f12ae411e3c180e898fb6c7783462f` |

Use absolute, forward-slash paths. Explicit Core/CMeta roots are resolved without
searching other SDKs as fallbacks. Select roots matching the compiler ABI,
architecture and build profile. The old monolithic Salts SDK is not selected.

The product manifest retains builtin baseline
`b1b19307e2d2ec1eefbdb7ea069de7d4bcd31f01`, PCRE2 10.46 with its desktop JIT
feature and SQLite 3.50.4 with FTS5. The added registry configuration retains its
`zstd` routing at cache commit `2913c530b8f12ae411e3c180e898fb6c7783462f`; no unused
zstd dependency is added. Optional search tools use that checkout's overlays.
Runtime FTS5/trigram, Unicode and JIT admission belongs to the engine modules.
TRE is not a dependency.

`FSX_BUILD_SEARCH_TOOLS=ON` selects the `search-tools` manifest feature. ugrep and
bfs remain standalone tools. ugrep currently supports Linux/macOS and MSVC
Windows; bfs supports Linux/macOS. Neither is selected for Windows LLVM-MinGW.

The `.env` file is private and ignored. CMake does not load it automatically;
a caller must explicitly load local settings before invoking the presets.
Preserve existing tokens and the caller's shared read-only cache feed policy.
The example's local writable binary cache is separate from the shared checkout
and every profile's own vcpkg dependency tree. Ordinary builds do not update a
shared checkout or prewarm packages.

## Windows x64

Use the shared LLVM-MinGW setup with `target: x64`, `build-type: toolchain-only`
and `obfuscator: false`. Preserve its `LLVM_MINGW_TOOLCHAIN_FILE`,
`LLVM_MINGW_DEPENDENCY_TOOLCHAIN_FILE` and
`LLVM_MINGW_HOST_DEPENDENCY_TOOLCHAIN_FILE` outputs. Run configure/build from a
VS developer shell and retain the parent PATH. Existing SDKs must match this ABI.

Both shared vcpkg setup actions provide checksum-pinned native pkgconf. For a
local shell without those actions, use the same helper before configuring:

```powershell
# Set the dependency roots, RE2C_ROOT and compiler setup outputs first.
$native = python -c "import json,os,sys; from pathlib import Path; sys.path.insert(0,sys.argv[1]); from windows_pkgconf import prepare_pkgconf; print(json.dumps(prepare_pkgconf(Path(os.environ['TEMP']))))" "$env:VCPKG_CACHE_REPOSITORY_ROOT/scripts" | ConvertFrom-Json
if ($LASTEXITCODE -ne 0) { throw 'Native pkgconf setup failed' }
$native.psobject.Properties | ForEach-Object {
    [Environment]::SetEnvironmentVariable($_.Name, [string]$_.Value, 'Process')
}
cmake --preset windows-release
cmake --build --preset windows-release
```

`windows-debug` provides the matching Debug graph. Windows presets preserve
`PKG_CONFIG` through vcpkg's filtered environment. Runtime PATH includes the
active SDK and dependency DLL directories and inherited PATH. No post-build DLL
copy or deployment step is added.

## Linux x64

Use native Clang, Ninja and pkg-config, set the same dependency roots, then run:

```sh
cmake --preset linux-release
cmake --build --preset linux-release
```

`linux-debug`, `linux-asan-ubsan` and `linux-tsan` have matching build presets.
Unsupported sanitizer/platform combinations fail configuration. No test presets
claim an existing suite; formal TinyTest/CTest cases arrive with the engine.

## Planned source ownership

The implementation boundaries remain `include/fsx`, `src/model`, `src/index`,
`src/query`, `src/changes`, `src/storage`, `src/platform`, `src/source/win32`,
`src/cli` and `tests`. Introduce each CMake subdirectory with its implementation.
SQLite remains the sole persistent fact source. Collection, writer publication
and consistent query snapshots remain fsx responsibilities.

## Validation

The Windows x64 LLVM-MinGW Release preset configured and compiled the supplied
native Lemon sources successfully. Core/CMeta and re2c discovery succeeded;
PCRE2 and SQLite FTS5 reused the existing dependency tree. No MSYS2 acquisition,
project installation or consumer validation was performed for this update.
Linux and sanitizer builds have not been run, and there are no engine tests yet.

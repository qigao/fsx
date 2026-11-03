option(BUILD_TESTING "Build FileSearch's formal native tests" OFF)
option(FSX_BUILD_NTFS_LIVE_TESTS "Build privileged real-volume NTFS acceptance and performance tests" OFF)
option(FSX_BUILD_PARSER_TOOLS "Build host Lemon and resolve re2c for query parser generation" ON)
set(FSX_HOST_LEMON_EXECUTABLE "" CACHE FILEPATH
  "Native host Lemon executable for cross compilation")
option(FSX_BUILD_SEARCH_TOOLS "Resolve optional native ugrep/bfs command-line tools" OFF)
option(FSX_ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(FSX_ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer" OFF)
option(FSX_ENABLE_TSAN "Enable ThreadSanitizer" OFF)

# vcpkg reads manifest features during project()/toolchain initialization.
set(VCPKG_MANIFEST_FEATURES "")
if(FSX_BUILD_SEARCH_TOOLS)
  list(APPEND VCPKG_MANIFEST_FEATURES search-tools)
endif()
 

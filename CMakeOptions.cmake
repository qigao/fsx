option(BUILD_TESTING "Build FileSearch's formal native tests" OFF)
option(FSX_BUILD_SEARCH_TOOLS "Install optional native ugrep/bfs command-line tools" OFF)
option(FSX_ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(FSX_ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer" OFF)
option(FSX_ENABLE_TSAN "Enable ThreadSanitizer" OFF)

# vcpkg reads manifest features during project()/toolchain initialization.
set(VCPKG_MANIFEST_FEATURES "")
if(FSX_BUILD_SEARCH_TOOLS)
  list(APPEND VCPKG_MANIFEST_FEATURES search-tools)
endif()

# Validate setup before project() can start dependency installation.
foreach(_fsx_setup_root IN ITEMS VCPKG_ROOT VCPKG_CACHE_REPOSITORY_ROOT SALTSCORE_ROOT CMETA_ROOT)
  if("$ENV{${_fsx_setup_root}}" STREQUAL "" OR
     NOT IS_DIRECTORY "$ENV{${_fsx_setup_root}}")
    message(FATAL_ERROR "${_fsx_setup_root} must name the selected installed SDK/tool checkout; see README.md")
  endif()
endforeach()
unset(_fsx_setup_root)
if(CMAKE_HOST_WIN32)
  if("$ENV{PKG_CONFIG}" STREQUAL "" OR NOT EXISTS "$ENV{PKG_CONFIG}")
    message(FATAL_ERROR "Prepare native Windows PKG_CONFIG with the shared setup before configuring; see README.md")
  endif()
endif()

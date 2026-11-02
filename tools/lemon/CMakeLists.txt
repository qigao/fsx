if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
  cmake_minimum_required(VERSION 3.25)
  project(FsxLemonHost LANGUAGES C)
  include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/CmakeUtils.cmake")
endif()

set(_fsx_target_runs_on_host FALSE)
if(CMAKE_HOST_SYSTEM_NAME STREQUAL CMAKE_SYSTEM_NAME AND
   (NOT CMAKE_SYSTEM_PROCESSOR OR
    CMAKE_HOST_SYSTEM_PROCESSOR STREQUAL CMAKE_SYSTEM_PROCESSOR))
  # The shared native LLVM toolchain deliberately sets CMAKE_SYSTEM_NAME but
  # leaves CMAKE_SYSTEM_PROCESSOR unset. llvm-mingw sets both. In either case
  # the generated executable is host-runnable when the known fields match.
  set(_fsx_target_runs_on_host TRUE)
endif()

if(CMAKE_CROSSCOMPILING AND NOT _fsx_target_runs_on_host)
  if(NOT FSX_HOST_LEMON_EXECUTABLE OR
     NOT EXISTS "${FSX_HOST_LEMON_EXECUTABLE}" OR
     IS_DIRECTORY "${FSX_HOST_LEMON_EXECUTABLE}")
    message(FATAL_ERROR
      "Cross-compiling requires FSX_HOST_LEMON_EXECUTABLE from a completed host preset build")
  endif()
  add_executable(lemon IMPORTED GLOBAL)
  set_target_properties(lemon PROPERTIES
    IMPORTED_LOCATION "${FSX_HOST_LEMON_EXECUTABLE}")
else()
  cmake_add_executable(lemon SOURCES lemon.c FOLDER tools)
  target_compile_features(lemon PRIVATE c_std_11)
endif()
unset(_fsx_target_runs_on_host)

cmake_config_target(lemon ALIAS Fsx::Lemon)

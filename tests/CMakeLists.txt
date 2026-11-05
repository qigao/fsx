cmake_add_test(fsx_engine_test SOURCES engine_test.c LIBS Fsx::Fsx Salts::TinyTest Salts::Platform unofficial::sqlite3::sqlite3 FOLDER tests)
set_tests_properties(fsx_engine_test PROPERTIES TIMEOUT 120 LABELS "engine;integration")
cmake_add_test(fsx_ntfs_raw_test SOURCES ntfs_raw_test.c LIBS Fsx::Fsx Salts::TinyTest Salts::Platform unofficial::sqlite3::sqlite3 FOLDER tests/ntfs)
set_tests_properties(fsx_ntfs_raw_test PROPERTIES TIMEOUT 30 LABELS "engine;ntfs;parser")
cmake_add_test(fsx_ntfs_benchmark_test SOURCES ntfs_benchmark_test.c ntfs_measure.c
  LIBS Fsx::Fsx Salts::TinyTest Salts::Platform unofficial::sqlite3::sqlite3
  DEFS "FSX_TEST_BUILD_TYPE=\"${CMAKE_BUILD_TYPE}\"" FOLDER tests/ntfs)
set_tests_properties(fsx_ntfs_benchmark_test PROPERTIES TIMEOUT 300 LABELS "engine;ntfs;performance"
  RUN_SERIAL TRUE)
find_package(Python3 COMPONENTS Interpreter REQUIRED)
if(FSX_BUILD_CONTENT_TESTS)
  if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "Content runtime tests currently require Linux")
  endif()
  if(NOT IS_ABSOLUTE "${FSX_CONTENT_TEST_UGREP}" OR NOT EXISTS "${FSX_CONTENT_TEST_UGREP}")
    message(FATAL_ERROR "FSX_CONTENT_TEST_UGREP must name the real native ugrep executable")
  endif()
  cmake_add_test(fsx_content_test SOURCES content_test.c LIBS Fsx::Fsx Salts::TinyTest Salts::Platform
    DEFS "FSX_TEST_UGREP=\"${FSX_CONTENT_TEST_UGREP}\"" FOLDER tests/content)
  set_tests_properties(fsx_content_test PROPERTIES TIMEOUT 60 LABELS "content;integration"
    ENVIRONMENT "TEMP=${CMAKE_CURRENT_BINARY_DIR};TMP=${CMAKE_CURRENT_BINARY_DIR}")
  add_test(NAME fsx_content_cli_test COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/content_cli_test.py" "$<TARGET_FILE:fsx_cli>" "${FSX_CONTENT_TEST_UGREP}")
  set_tests_properties(fsx_content_cli_test PROPERTIES TIMEOUT 30 LABELS "content;cli;e2e")
endif()
add_test(NAME fsx_cli_test COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/cli_test.py" "$<TARGET_FILE:fsx_cli>")
set_tests_properties(fsx_cli_test PROPERTIES TIMEOUT 60 LABELS "cli;e2e")

# Keep all native test temporary files, including SQLite spill files, in the build tree.
set_tests_properties(fsx_engine_test fsx_ntfs_raw_test fsx_ntfs_benchmark_test fsx_cli_test PROPERTIES
  ENVIRONMENT "TEMP=${CMAKE_CURRENT_BINARY_DIR};TMP=${CMAKE_CURRENT_BINARY_DIR}")

if(FSX_BUILD_NTFS_LIVE_TESTS)
  if(NOT WIN32)
    message(FATAL_ERROR "FSX_BUILD_NTFS_LIVE_TESTS requires Windows and a readable local NTFS volume")
  endif()
  cmake_add_test(fsx_ntfs_live_test SOURCES ntfs_live_test.c ntfs_measure.c
    LIBS Fsx::Fsx Salts::TinyTest Salts::Platform unofficial::sqlite3::sqlite3 psapi
    DEFS "FSX_TEST_BUILD_TYPE=\"${CMAKE_BUILD_TYPE}\"" FOLDER tests/ntfs)
  add_test(NAME fsx_ntfs_link_test COMMAND fsx_ntfs_live_test --filter "enumerates all current hardlink")
  set_tests_properties(fsx_ntfs_link_test PROPERTIES TIMEOUT 30
    LABELS "ntfs;live;privileged" RUN_SERIAL TRUE
    ENVIRONMENT "TEMP=${CMAKE_CURRENT_BINARY_DIR};TMP=${CMAKE_CURRENT_BINARY_DIR}")
  set_tests_properties(fsx_ntfs_live_test PROPERTIES TIMEOUT 1800
    LABELS "ntfs;live;performance;privileged" RUN_SERIAL TRUE
    ENVIRONMENT "TEMP=${CMAKE_CURRENT_BINARY_DIR};TMP=${CMAKE_CURRENT_BINARY_DIR}")
endif()

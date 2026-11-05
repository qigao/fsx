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
  if(NOT IS_ABSOLUTE "${FSX_CONTENT_TEST_UGREP}" OR NOT EXISTS "${FSX_CONTENT_TEST_UGREP}")
    message(FATAL_ERROR "FSX_CONTENT_TEST_UGREP must name the real native ugrep executable")
  endif()
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux" OR CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    if(NOT IS_ABSOLUTE "${FSX_CONTENT_TEST_BFS}" OR NOT EXISTS "${FSX_CONTENT_TEST_BFS}")
      message(FATAL_ERROR "FSX_CONTENT_TEST_BFS must name the real native bfs executable")
    endif()
    cmake_add_executable(fsx_content_process_peer SOURCES content_process_peer.c FOLDER tests/content)
    cmake_add_test(fsx_content_test SOURCES content_test.c LIBS Fsx::Fsx Salts::TinyTest Salts::Platform
      DEFS "FSX_TEST_UGREP=\"${FSX_CONTENT_TEST_UGREP}\"" "FSX_TEST_BFS=\"${FSX_CONTENT_TEST_BFS}\""
        "FSX_TEST_CONTENT_PEER=\"$<TARGET_FILE:fsx_content_process_peer>\"" FOLDER tests/content)
  elseif(WIN32)
    cmake_add_executable(fsx_content_process_peer SOURCES content_windows_peer.c LIBS shell32 FOLDER tests/content)
    cmake_add_test(fsx_content_test SOURCES content_windows_test.c LIBS Fsx::Fsx Salts::TinyTest Salts::Platform
      DEFS "FSX_TEST_UGREP=\"${FSX_CONTENT_TEST_UGREP}\""
        "FSX_TEST_CONTENT_PEER=\"$<TARGET_FILE:fsx_content_process_peer>\"" FOLDER tests/content)
  else()
    message(FATAL_ERROR "Content tests require Linux, macOS or Windows")
  endif()
  add_dependencies(fsx_content_test fsx_content_process_peer)
  set_tests_properties(fsx_content_test PROPERTIES TIMEOUT 60 LABELS "content;integration"
    ENVIRONMENT "TEMP=${CMAKE_CURRENT_BINARY_DIR};TMP=${CMAKE_CURRENT_BINARY_DIR}")
  cmake_add_test(fsx_content_text_test SOURCES content_text_test.c
    LIBS Fsx::Fsx Salts::TinyTest Salts::Platform unofficial::sqlite3::sqlite3 FOLDER tests/content)
  set_tests_properties(fsx_content_text_test PROPERTIES TIMEOUT 60 LABELS "content;unit")
  file(SHA256 "${CMAKE_CURRENT_SOURCE_DIR}/content_benchmark_test.c" content_benchmark_sha256)
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS content_benchmark_test.c)
  cmake_add_test(fsx_content_benchmark_test SOURCES content_benchmark_test.c
    LIBS Fsx::Fsx Salts::TinyTest Salts::Platform unofficial::sqlite3::sqlite3
    DEFS "FSX_TEST_UGREP=\"${FSX_CONTENT_TEST_UGREP}\"" "FSX_TEST_BFS=\"${FSX_CONTENT_TEST_BFS}\""
      "FSX_TEST_BUILD_TYPE=\"${CMAKE_BUILD_TYPE}\""
      "FSX_TEST_PLATFORM=\"${CMAKE_SYSTEM_NAME}/${CMAKE_SYSTEM_PROCESSOR}\""
      "FSX_TEST_SOURCE_SHA256=\"${content_benchmark_sha256}\""
      "FSX_TEST_COMPILER=\"${CMAKE_C_COMPILER_ID}/${CMAKE_C_COMPILER_VERSION}\""
      "FSX_TEST_SANITIZERS=\"asan=${FSX_ENABLE_ASAN}/ubsan=${FSX_ENABLE_UBSAN}/tsan=${FSX_ENABLE_TSAN}\""
    FOLDER tests/content)
  set_tests_properties(fsx_content_benchmark_test PROPERTIES TIMEOUT 300
    LABELS "content;performance" RUN_SERIAL TRUE
    ENVIRONMENT "TEMP=${CMAKE_CURRENT_BINARY_DIR};TMP=${CMAKE_CURRENT_BINARY_DIR}")
  add_test(NAME fsx_content_cli_test COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/content_cli_test.py" "$<TARGET_FILE:fsx_cli>" "${FSX_CONTENT_TEST_UGREP}" "${FSX_CONTENT_TEST_BFS}")
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

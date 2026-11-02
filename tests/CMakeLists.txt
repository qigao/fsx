cmake_add_test(fsx_engine_test SOURCES engine_test.c LIBS Fsx::Fsx Salts::TinyTest Salts::Platform unofficial::sqlite3::sqlite3 FOLDER tests)
set_tests_properties(fsx_engine_test PROPERTIES TIMEOUT 120 LABELS "engine;integration")
find_package(Python3 COMPONENTS Interpreter REQUIRED)
add_test(NAME fsx_cli_test COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/cli_test.py" "$<TARGET_FILE:fsx_cli>")
set_tests_properties(fsx_cli_test PROPERTIES TIMEOUT 60 LABELS "cli;e2e")

# Keep all native test temporary files, including SQLite spill files, in the build tree.
set_tests_properties(fsx_engine_test fsx_cli_test PROPERTIES
  ENVIRONMENT "TEMP=${CMAKE_CURRENT_BINARY_DIR};TMP=${CMAKE_CURRENT_BINARY_DIR}")

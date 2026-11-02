set(RE2C_MIN_VERSION "4.6")

if(NOT DEFINED ENV{RE2C_ROOT} OR "$ENV{RE2C_ROOT}" STREQUAL "")
  message(FATAL_ERROR "RE2C_ROOT is required")
endif()
if(NOT IS_DIRECTORY "$ENV{RE2C_ROOT}")
  message(FATAL_ERROR "RE2C_ROOT is not a directory: $ENV{RE2C_ROOT}")
endif()

set(RE2C_ROOT "$ENV{RE2C_ROOT}")
unset(RE2C_EXECUTABLE CACHE)
unset(RE2C_EXECUTABLE)
find_program(RE2C_EXECUTABLE
  NAMES re2c
  PATHS "${RE2C_ROOT}/bin"
  NO_DEFAULT_PATH
  REQUIRED)
file(REAL_PATH "${RE2C_ROOT}" _re2c_root_realpath)
file(REAL_PATH "${RE2C_EXECUTABLE}" _re2c_executable_realpath)
cmake_path(IS_PREFIX _re2c_root_realpath "${_re2c_executable_realpath}"
           NORMALIZE _re2c_executable_in_root)
if(NOT _re2c_executable_in_root)
  message(FATAL_ERROR
          "re2c executable is outside RE2C_ROOT: ${RE2C_EXECUTABLE}")
endif()
execute_process(
  COMMAND "${RE2C_EXECUTABLE}" --version
  RESULT_VARIABLE RE2C_VERSION_RESULT
  OUTPUT_VARIABLE RE2C_VERSION_OUTPUT
  ERROR_VARIABLE RE2C_VERSION_ERROR
  OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT RE2C_VERSION_RESULT EQUAL 0 OR
   NOT RE2C_VERSION_OUTPUT MATCHES "re2c ([0-9]+\\.[0-9]+(\\.[0-9]+)?)")
  message(FATAL_ERROR
          "Unable to determine re2c version from '${RE2C_EXECUTABLE}': ${RE2C_VERSION_ERROR}")
endif()
set(RE2C_VERSION "${CMAKE_MATCH_1}")
if(RE2C_VERSION VERSION_LESS RE2C_MIN_VERSION)
  message(FATAL_ERROR
          "re2c ${RE2C_MIN_VERSION} or newer is required; found ${RE2C_VERSION} at ${RE2C_EXECUTABLE}")
endif()

set(RE2C_UNICODE_PROPERTIES "${RE2C_ROOT}/share/re2c/stdlib/unicode_properties.re")
set(RE2C_UNICODE_CATEGORIES "${RE2C_ROOT}/share/re2c/stdlib/unicode_categories.re")
foreach(_re2c_data IN ITEMS RE2C_UNICODE_PROPERTIES RE2C_UNICODE_CATEGORIES)
  if(NOT EXISTS "${${_re2c_data}}" OR IS_DIRECTORY "${${_re2c_data}}")
    message(FATAL_ERROR "The required re2c Unicode data is missing: ${${_re2c_data}}")
  endif()
endforeach()
unset(_re2c_data)

if(NOT TARGET lemon)
  message(FATAL_ERROR "The required in-tree lemon target is missing")
endif()

set(LEMPAR "${PROJECT_SOURCE_DIR}/tools/lemon/lempar.c")
if(NOT EXISTS "${LEMPAR}")
  message(FATAL_ERROR "The required lemon template is missing: ${LEMPAR}")
endif()

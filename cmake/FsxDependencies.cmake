# Product dependencies, kept outside the generic sanitizer helper.
find_package(Threads REQUIRED)

find_package(SaltsCore CONFIG REQUIRED PATHS "$ENV{SALTSCORE_ROOT}"
  NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
find_package(CMeta CONFIG REQUIRED PATHS "$ENV{CMETA_ROOT}"
  NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
foreach(_fsx_dependency IN ITEMS Salts::Core Salts::Platform Salts::Concurrency
    Salts::FS Salts::Unicode CMeta::CMeta)
  if(NOT TARGET "${_fsx_dependency}")
    message(FATAL_ERROR "Selected SDKs do not provide ${_fsx_dependency}; use compatible split Core/CMeta SDKs with Unicode")
  endif()
endforeach()
if(BUILD_TESTING AND NOT TARGET Salts::TinyTest)
  message(FATAL_ERROR "BUILD_TESTING requires the Core SDK's Salts::TinyTest")
endif()
unset(_fsx_dependency)

find_package(unofficial-sqlite3 CONFIG REQUIRED)
find_package(pcre2 CONFIG REQUIRED)
# Future storage/regex modules privately link unofficial::sqlite3::sqlite3
# and PCRE2::8BIT. Runtime FTS5/trigram/JIT admission belongs to those modules.

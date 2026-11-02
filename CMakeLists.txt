cmake_minimum_required(VERSION 3.25)
include(CMakeOptions.cmake)
project(FileSearch VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set_property(GLOBAL PROPERTY USE_FOLDERS ON)
if(WIN32)
  set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/bin")
endif()

include(GNUInstallDirs)
include(cmake/Sanitizers.cmake)
include(cmake/FsxDependencies.cmake)
if(BUILD_TESTING)
  include(CTest)
endif()

# Source-owned library, CLI and test subdirectories are added with their real
# implementations. Do not create empty libraries or synthetic consumer targets.
install(FILES README.md DESTINATION "${CMAKE_INSTALL_DOCDIR}")
install(FILES docs/FileSearch-C11-设计.md
  DESTINATION "${CMAKE_INSTALL_DOCDIR}/docs")
message(STATUS "FileSearch build scaffold configured; engine and CLI are not implemented")

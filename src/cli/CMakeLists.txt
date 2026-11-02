cmake_add_executable(fsx_cli SOURCES main.c LIBS Fsx::Fsx FOLDER fsx)
cmake_config_target(fsx_cli OUTPUT_NAME fsx)
# A Windows Unicode console entry point preserves UTF-16 command-line paths.
if(MINGW)
  target_link_options(fsx_cli PRIVATE -municode)
endif()

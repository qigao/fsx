add_library(fsx STATIC engine.c model/memory.c storage/sqlite.c source/directory.c)
cmake_config_target(fsx ALIAS Fsx::Fsx FOLDER fsx)
target_include_directories(fsx PUBLIC "${PROJECT_SOURCE_DIR}/include" PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(fsx PUBLIC c_std_11)
target_link_libraries(fsx PUBLIC CMeta::CMeta PRIVATE Salts::Core Salts::Platform
  Salts::Concurrency Salts::Unicode unofficial::sqlite3::sqlite3 PCRE2::8BIT)
add_subdirectory(query)
add_subdirectory(cli)

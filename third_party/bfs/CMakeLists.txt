# Private, pinned POSIX traversal implementation. See UPSTREAM.md.
add_library(fsx_bfs STATIC
  bridge.c
  src/alloc.c src/bfstd.c src/bftw.c src/diag.c src/dir.c
  src/dstring.c src/ioq.c src/mtab.c src/stat.c src/thread.c src/trie.c)
set_target_properties(fsx_bfs PROPERTIES
  C_STANDARD 17 C_STANDARD_REQUIRED ON C_EXTENSIONS ON
  C_VISIBILITY_PRESET hidden POSITION_INDEPENDENT_CODE ON
  FOLDER "fsx/vendor")
target_include_directories(fsx_bfs PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_options(fsx_bfs PRIVATE
  -include "${CMAKE_CURRENT_SOURCE_DIR}/private.h")
set_source_files_properties(bridge.c PROPERTIES
  COMPILE_DEFINITIONS FSX_BFS_ALLOC_IMPLEMENTATION=1)
target_link_libraries(fsx_bfs PRIVATE Threads::Threads)

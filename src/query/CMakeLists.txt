cmake_add_grammar(fsx_query
  LEXER_RE lexer.re
  RE2C_EXECUTABLE "${RE2C_EXECUTABLE}"
  LEXER_OPTIONS --no-generation-date --no-version
  FOLDER codegen)
target_sources(fsx PRIVATE query.c "${fsx_query_LEXER_GEN}")
target_include_directories(fsx PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

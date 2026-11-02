# Project-independent target, source collection and code generation helpers.
include_guard(GLOBAL)

function(_cmake_check_arguments caller prefix)
    if(DEFINED ${prefix}_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "${caller}: unknown arguments: ${${prefix}_UNPARSED_ARGUMENTS}")
    endif()
    if(DEFINED ${prefix}_KEYWORDS_MISSING_VALUES)
        message(FATAL_ERROR "${caller}: missing values for: ${${prefix}_KEYWORDS_MISSING_VALUES}")
    endif()
endfunction()

# Only explicitly supplied attributes are changed. VERSION/SOVERSION, including
# zero, belong to the caller's ABI policy; omitted attributes retain their value.
function(cmake_config_target target_name)
    set(options)
    set(oneValueArgs FOLDER VERSION SOVERSION EXPORT_NAME ALIAS OUTPUT_NAME)
    set(multiValueArgs)
    cmake_parse_arguments(PARSE_ARGV 1 ARG "${options}" "${oneValueArgs}" "${multiValueArgs}")
    _cmake_check_arguments(cmake_config_target ARG)

    if(NOT TARGET "${target_name}")
        message(FATAL_ERROR "cmake_config_target: target '${target_name}' does not exist")
    endif()
    get_target_property(target_type "${target_name}" TYPE)

    if(DEFINED ARG_ALIAS)
        if(target_type STREQUAL "EXECUTABLE")
            add_executable(${ARG_ALIAS} ALIAS ${target_name})
        else()
            add_library(${ARG_ALIAS} ALIAS ${target_name})
        endif()
    endif()

    foreach(attribute IN ITEMS FOLDER EXPORT_NAME OUTPUT_NAME VERSION SOVERSION)
        if(DEFINED ARG_${attribute})
            set_target_properties("${target_name}" PROPERTIES
                ${attribute} "${ARG_${attribute}}")
        endif()
    endforeach()
endfunction()

# LEXER_RE requires an absolute host RE2C_EXECUTABLE path. GRAMMAR_Y requires
# a host executable LEMON_TARGET and LEMON_TEMPLATE path. Inputs and dependency
# paths are relative to the calling source directory; LEXER_OUTPUT is relative
# to its binary directory. Generated paths and codegen targets are returned as
# <TARGET_NAME>_LEXER_GEN/_LEXER_TARGET and _GRAMMAR_GEN/_GRAMMAR_H/_GRAMMAR_TARGET.
# Missing inputs/tools fail configuration; generated sources belong to callers.
function(cmake_add_grammar TARGET_NAME)
  set(options LEXER_DEPENDS_ON_GRAMMAR)
  set(oneValueArgs LEXER_RE GRAMMAR_Y FOLDER LEXER_OUTPUT
                   RE2C_EXECUTABLE LEMON_TARGET LEMON_TEMPLATE)
  set(multiValueArgs LEXER_DEPENDS LEXER_OPTIONS)
  cmake_parse_arguments(PARSE_ARGV 1 ARG "${options}" "${oneValueArgs}" "${multiValueArgs}")
  _cmake_check_arguments(cmake_add_grammar ARG)
  if(NOT ARG_LEXER_RE AND NOT ARG_GRAMMAR_Y)
    message(FATAL_ERROR "cmake_add_grammar: LEXER_RE or GRAMMAR_Y is required")
  endif()
  if(ARG_LEXER_DEPENDS_ON_GRAMMAR AND (NOT ARG_LEXER_RE OR NOT ARG_GRAMMAR_Y))
    message(FATAL_ERROR "cmake_add_grammar: LEXER_DEPENDS_ON_GRAMMAR requires LEXER_RE and GRAMMAR_Y")
  endif()
  string(TOLOWER "${TARGET_NAME}" target_name_lower)

  if(ARG_LEXER_RE)
    get_filename_component(ARG_LEXER_RE "${ARG_LEXER_RE}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
    if(NOT EXISTS "${ARG_LEXER_RE}" OR IS_DIRECTORY "${ARG_LEXER_RE}")
      message(FATAL_ERROR "cmake_add_grammar: invalid LEXER_RE: '${ARG_LEXER_RE}'")
    endif()
    if(NOT IS_ABSOLUTE "${ARG_RE2C_EXECUTABLE}" OR
       NOT EXISTS "${ARG_RE2C_EXECUTABLE}" OR IS_DIRECTORY "${ARG_RE2C_EXECUTABLE}")
      message(FATAL_ERROR "cmake_add_grammar: RE2C_EXECUTABLE must name an existing absolute host executable")
    endif()
  endif()
  if(ARG_GRAMMAR_Y)
    get_filename_component(ARG_GRAMMAR_Y "${ARG_GRAMMAR_Y}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
    if(NOT EXISTS "${ARG_GRAMMAR_Y}" OR IS_DIRECTORY "${ARG_GRAMMAR_Y}")
      message(FATAL_ERROR "cmake_add_grammar: invalid GRAMMAR_Y: '${ARG_GRAMMAR_Y}'")
    endif()
    if(NOT TARGET "${ARG_LEMON_TARGET}")
      message(FATAL_ERROR "cmake_add_grammar: LEMON_TARGET must name a host executable target")
    endif()
    get_target_property(lemon_type "${ARG_LEMON_TARGET}" TYPE)
    if(NOT lemon_type STREQUAL "EXECUTABLE")
      message(FATAL_ERROR "cmake_add_grammar: LEMON_TARGET is not an executable: '${ARG_LEMON_TARGET}'")
    endif()
    if(NOT ARG_LEMON_TEMPLATE)
      message(FATAL_ERROR "cmake_add_grammar: LEMON_TEMPLATE is required for GRAMMAR_Y")
    endif()
    get_filename_component(ARG_LEMON_TEMPLATE "${ARG_LEMON_TEMPLATE}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
    if(NOT EXISTS "${ARG_LEMON_TEMPLATE}" OR IS_DIRECTORY "${ARG_LEMON_TEMPLATE}")
      message(FATAL_ERROR "cmake_add_grammar: invalid LEMON_TEMPLATE: '${ARG_LEMON_TEMPLATE}'")
    endif()
  endif()
  set(absolute_lexer_depends)
  foreach(lexer_depend IN LISTS ARG_LEXER_DEPENDS)
    get_filename_component(lexer_depend "${lexer_depend}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
    list(APPEND absolute_lexer_depends "${lexer_depend}")
  endforeach()

  if(ARG_GRAMMAR_Y)
    set(GRAMMAR_H "${CMAKE_CURRENT_BINARY_DIR}/${target_name_lower}_grammar_gen.h")
  endif()

  if(ARG_LEXER_RE)
    if(ARG_LEXER_OUTPUT)
      set(LEXER_GEN "${CMAKE_CURRENT_BINARY_DIR}/${ARG_LEXER_OUTPUT}")
    else()
      set(LEXER_GEN "${CMAKE_CURRENT_BINARY_DIR}/${target_name_lower}_lexer_gen.c")
    endif()
    set(lexer_depends "${ARG_LEXER_RE}" "${ARG_RE2C_EXECUTABLE}" ${absolute_lexer_depends})
    if(ARG_LEXER_DEPENDS_ON_GRAMMAR)
      list(APPEND lexer_depends "${GRAMMAR_H}")
    endif()
    add_custom_command(
      OUTPUT "${LEXER_GEN}"
      COMMAND "${ARG_RE2C_EXECUTABLE}" ${ARG_LEXER_OPTIONS} -o "${LEXER_GEN}" "${ARG_LEXER_RE}"
      DEPENDS ${lexer_depends}
      COMMENT "Generating ${TARGET_NAME} lexer with re2c"
      VERBATIM)
    set(LEXER_TARGET "${TARGET_NAME}_lexer_codegen")
    add_custom_target(${LEXER_TARGET} DEPENDS ${LEXER_GEN})
    if(ARG_FOLDER)
      set_target_properties(${LEXER_TARGET} PROPERTIES FOLDER ${ARG_FOLDER})
    endif()
    set(${TARGET_NAME}_LEXER_GEN ${LEXER_GEN} PARENT_SCOPE)
    set(${TARGET_NAME}_LEXER_TARGET ${LEXER_TARGET} PARENT_SCOPE)
  endif()

  if(ARG_GRAMMAR_Y)
    set(GRAMMAR_GEN "${CMAKE_CURRENT_BINARY_DIR}/${target_name_lower}_grammar_gen.c")
    set(GRAMMAR_Y_GEN "${CMAKE_CURRENT_BINARY_DIR}/${target_name_lower}_grammar_gen.y")
    add_custom_command(
      OUTPUT "${GRAMMAR_GEN}" "${GRAMMAR_H}"
      BYPRODUCTS "${GRAMMAR_Y_GEN}"
      COMMAND "${CMAKE_COMMAND}" -E copy "${ARG_GRAMMAR_Y}" "${GRAMMAR_Y_GEN}"
      COMMAND "$<TARGET_FILE:${ARG_LEMON_TARGET}>" "-T${ARG_LEMON_TEMPLATE}" "${GRAMMAR_Y_GEN}"
      DEPENDS "${ARG_GRAMMAR_Y}" "${ARG_LEMON_TARGET}" "${ARG_LEMON_TEMPLATE}"
      COMMENT "Generating ${TARGET_NAME} parser with lemon"
      VERBATIM)
    set(GRAMMAR_TARGET "${TARGET_NAME}_grammar_codegen")
    add_custom_target(${GRAMMAR_TARGET} DEPENDS ${GRAMMAR_GEN} ${GRAMMAR_H})
    if(ARG_FOLDER)
      set_target_properties(${GRAMMAR_TARGET} PROPERTIES FOLDER ${ARG_FOLDER})
    endif()
    set(${TARGET_NAME}_GRAMMAR_GEN ${GRAMMAR_GEN} PARENT_SCOPE)
    set(${TARGET_NAME}_GRAMMAR_H ${GRAMMAR_H} PARENT_SCOPE)
    set(${TARGET_NAME}_GRAMMAR_TARGET ${GRAMMAR_TARGET} PARENT_SCOPE)
  endif()
endfunction()

# DIRS is required. Relative DIRS/EXCLUDES resolve against the calling source
# directory. Return absolute files in VAR; track additions/removals at build time.
function(cmake_add_source VAR)
  set(options RECURSE)
  set(oneValueArgs)
  set(multiValueArgs DIRS EXCLUDES PATTERNS)
  cmake_parse_arguments(PARSE_ARGV 1 ARG "${options}" "${oneValueArgs}" "${multiValueArgs}")
  _cmake_check_arguments(cmake_add_source ARG)

  set(glob_mode GLOB)
  if(ARG_RECURSE)
    set(glob_mode GLOB_RECURSE)
  endif()

  if(NOT ARG_DIRS)
    message(FATAL_ERROR "cmake_add_source: DIRS is required")
  endif()

  if(NOT ARG_PATTERNS)
    set(ARG_PATTERNS "*.c" "*.cpp" "*.h" "*.hpp" "*.cc" "*.hh")
  endif()

  set(patterns)
  foreach(dir IN LISTS ARG_DIRS)
    get_filename_component(dir "${dir}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
    if(NOT IS_DIRECTORY "${dir}")
      message(FATAL_ERROR "cmake_add_source: DIRS entry is not a directory: '${dir}'")
    endif()
    foreach(pat IN LISTS ARG_PATTERNS)
      list(APPEND patterns "${dir}/${pat}")
    endforeach()
  endforeach()

  file(${glob_mode} collected LIST_DIRECTORIES false CONFIGURE_DEPENDS ${patterns})

  foreach(excluded IN LISTS ARG_EXCLUDES)
    get_filename_component(excluded "${excluded}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
    list(REMOVE_ITEM collected "${excluded}")
  endforeach()

  set(${VAR} ${collected} PARENT_SCOPE)
endfunction()

function(_cmake_create_executable target_name sources libraries definitions includes folder)
  if(TARGET "${target_name}")
    message(FATAL_ERROR "Cannot create executable: target '${target_name}' already exists")
  endif()
  if("${sources}" STREQUAL "")
    message(FATAL_ERROR "Cannot create executable: SOURCES is required for '${target_name}'")
  endif()

  add_executable("${target_name}" ${sources})
  if(NOT "${libraries}" STREQUAL "")
    target_link_libraries("${target_name}" PRIVATE ${libraries})
  endif()
  if(NOT "${definitions}" STREQUAL "")
    target_compile_definitions("${target_name}" PRIVATE ${definitions})
  endif()
  if(NOT "${includes}" STREQUAL "")
    target_include_directories("${target_name}" PRIVATE ${includes})
  endif()
  if(NOT "${folder}" STREQUAL "")
    cmake_config_target("${target_name}" FOLDER "${folder}")
  endif()
endfunction()

# Executable helpers require SOURCES. LIBS, DEFS and INCLUDES are PRIVATE;
# FOLDER is optional. They never choose dependencies, flags or installation rules.
function(cmake_add_executable target_name)
  set(options)
  set(oneValueArgs FOLDER)
  set(multiValueArgs SOURCES LIBS DEFS INCLUDES)
  cmake_parse_arguments(PARSE_ARGV 1 ARG "${options}" "${oneValueArgs}" "${multiValueArgs}")
  _cmake_check_arguments(cmake_add_executable ARG)
  _cmake_create_executable("${target_name}" "${ARG_SOURCES}" "${ARG_LIBS}"
    "${ARG_DEFS}" "${ARG_INCLUDES}" "${ARG_FOLDER}")
endfunction()

# The caller enables testing and owns additional CTest properties.
function(cmake_add_test target_name)
  set(options)
  set(oneValueArgs FOLDER)
  set(multiValueArgs SOURCES LIBS DEFS INCLUDES)
  cmake_parse_arguments(PARSE_ARGV 1 ARG "${options}" "${oneValueArgs}" "${multiValueArgs}")
  _cmake_check_arguments(cmake_add_test ARG)
  _cmake_create_executable("${target_name}" "${ARG_SOURCES}" "${ARG_LIBS}"
    "${ARG_DEFS}" "${ARG_INCLUDES}" "${ARG_FOLDER}")
  add_test(NAME "${target_name}" COMMAND "${target_name}")
endfunction()

# Benchmark registration and runtime arguments belong to the caller.
function(cmake_add_benchmark target_name)
  set(options)
  set(oneValueArgs FOLDER)
  set(multiValueArgs SOURCES LIBS DEFS INCLUDES)
  cmake_parse_arguments(PARSE_ARGV 1 ARG "${options}" "${oneValueArgs}" "${multiValueArgs}")
  _cmake_check_arguments(cmake_add_benchmark ARG)
  _cmake_create_executable("${target_name}" "${ARG_SOURCES}" "${ARG_LIBS}"
    "${ARG_DEFS}" "${ARG_INCLUDES}" "${ARG_FOLDER}")
endfunction()

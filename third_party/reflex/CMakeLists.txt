add_library(fsx_reflex STATIC
  lib/convert.cpp
  lib/debug.cpp
  lib/error.cpp
  lib/input.cpp
  lib/matcher.cpp
  lib/pattern.cpp
  lib/posix.cpp
  lib/simd.cpp
  lib/unicode.cpp
  lib/utf8.cpp
  lib/block_scripts.cpp
  lib/language_scripts.cpp
  lib/letter_scripts.cpp
  lib/letter_case.cpp
  lib/composer.cpp)
cmake_config_target(fsx_reflex FOLDER third_party)
target_include_directories(fsx_reflex PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_features(fsx_reflex PUBLIC cxx_std_11)
target_compile_definitions(fsx_reflex PUBLIC WITH_NO_INDENT WITH_NO_CODEGEN)
set_target_properties(fsx_reflex PROPERTIES CXX_EXTENSIONS OFF)

# Only baseline ISA features of the selected target are enabled. No host CPU
# probes, -march=native, or optional AVX dispatch objects enter this dependency.
if(CMAKE_OSX_ARCHITECTURES)
  set(_fsx_reflex_arch "${CMAKE_OSX_ARCHITECTURES}")
else()
  set(_fsx_reflex_arch "${CMAKE_SYSTEM_PROCESSOR}")
endif()
if(_fsx_reflex_arch MATCHES "^(x86_64|AMD64|amd64)$")
  target_compile_definitions(fsx_reflex PUBLIC HAVE_SSE2)
elseif(_fsx_reflex_arch MATCHES "^(aarch64|arm64|ARM64)$")
  target_compile_definitions(fsx_reflex PUBLIC HAVE_NEON)
endif()
unset(_fsx_reflex_arch)

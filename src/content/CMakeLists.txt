target_sources(fsx PRIVATE api.c json.c process.c)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_sources(fsx PRIVATE child_linux.c discovery_linux.c)
endif()

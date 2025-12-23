cmake_minimum_required(VERSION 3.16)
project(erkao C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_executable(erkao
  src/main.c
  src/lexer.c
  src/singlepass.c
  src/value.c
  src/chunk.c
  src/disasm.c
  src/vm.c
  src/runtime.c
  src/exec.c
  src/imports.c
  src/stdlib.c
  src/package.c
  src/gc_core.c
  src/gc_trace.c
  src/gc_young.c
  src/gc_sweep.c
  src/program.c
  src/plugin.c
  src/tooling.c
)

target_include_directories(erkao PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)

if(WIN32)
  target_link_libraries(erkao winhttp ws2_32)
else()
  find_package(CURL REQUIRED)
  if(TARGET CURL::libcurl)
    target_link_libraries(erkao dl m CURL::libcurl)
  else()
    target_include_directories(erkao PRIVATE ${CURL_INCLUDE_DIRS})
    target_link_libraries(erkao dl m ${CURL_LIBRARIES})
  endif()
endif()

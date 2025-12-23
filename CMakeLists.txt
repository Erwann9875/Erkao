cmake_minimum_required(VERSION 3.16)
project(erkao C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

option(ERKAO_GRAPHICS "Enable graphics module (requires SDL2)" ON)

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

if(ERKAO_GRAPHICS)
  find_package(SDL2 CONFIG QUIET)
  find_package(SDL2_image CONFIG QUIET)
  find_package(SDL2_ttf CONFIG QUIET)
  find_package(SDL2_mixer CONFIG QUIET)

  # Check if SDL2 targets exist (vcpkg uses targets, not _FOUND variables)
  if(TARGET SDL2::SDL2 AND TARGET SDL2_image::SDL2_image AND TARGET SDL2_ttf::SDL2_ttf AND TARGET SDL2_mixer::SDL2_mixer)
    message(STATUS "SDL2 found - graphics module enabled")
    
    target_sources(erkao PRIVATE src/graphics.c)
    target_compile_definitions(erkao PRIVATE ERKAO_HAS_GRAPHICS=1)
    
    target_link_libraries(erkao 
      SDL2::SDL2 
      SDL2::SDL2main
      SDL2_image::SDL2_image
      SDL2_ttf::SDL2_ttf
      SDL2_mixer::SDL2_mixer
    )
  else()
    message(STATUS "SDL2 not found - graphics module disabled")
    message(STATUS "  SDL2::SDL2 found: $<TARGET_EXISTS:SDL2::SDL2>")
    message(STATUS "  To enable graphics, install SDL2:")
    message(STATUS "    Windows: .\\scripts\\install-sdl2.ps1")
    message(STATUS "    macOS/Linux: ./scripts/install-sdl2.sh")
    set(ERKAO_GRAPHICS OFF)
  endif()
endif()

if(NOT ERKAO_GRAPHICS)
  target_compile_definitions(erkao PRIVATE ERKAO_HAS_GRAPHICS=0)
endif()

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

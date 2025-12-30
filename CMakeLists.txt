cmake_minimum_required(VERSION 3.16)
project(erkao C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

option(ERKAO_GRAPHICS "Enable graphics module (requires SDL2)" ON)
option(ERKAO_SANITIZERS "Enable ASAN/UBSAN" OFF)
option(ERKAO_FUZZ "Build libFuzzer targets" OFF)
option(ERKAO_WARNINGS "Enable compiler warnings" ON)
option(ERKAO_WERROR "Treat warnings as errors" OFF)
option(ERKAO_DB_POSTGRES "Enable PostgreSQL driver" ON)
option(ERKAO_DB_MYSQL "Enable MySQL driver" ON)
option(ERKAO_DB_MONGO "Enable MongoDB driver" ON)

set(ERKAO_CORE_SOURCES
  src/lexer.c
  src/singlepass.c
  src/singlepass_parse.c
  src/singlepass_patterns.c
  src/singlepass_types.c
  src/value.c
  src/chunk.c
  src/disasm.c
  src/vm.c
  src/runtime.c
  src/exec.c
  src/imports.c
  src/stdlib.c
  src/db.c
  src/db_postgres.c
  src/db_mysql.c
  src/db_mongo.c
  src/package.c
  src/gc_core.c
  src/gc_trace.c
  src/gc_young.c
  src/gc_sweep.c
  src/program.c
  src/plugin.c
  src/tooling.c
)

add_executable(erkao
  src/main.c
  ${ERKAO_CORE_SOURCES}
)

set(ERKAO_INCLUDE_DIRS
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_include_directories(erkao PRIVATE ${ERKAO_INCLUDE_DIRS})

function(erkao_apply_warnings target)
  if(ERKAO_WARNINGS)
    if(MSVC)
      target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_WARNINGS)
      target_compile_options(${target} PRIVATE /W4)
      if(ERKAO_WERROR)
        target_compile_options(${target} PRIVATE /WX)
      endif()
    elseif(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
      target_compile_options(${target} PRIVATE -Wall -Wextra)
      if(ERKAO_WERROR)
        target_compile_options(${target} PRIVATE -Werror)
      endif()
    endif()
  endif()
endfunction()

erkao_apply_warnings(erkao)

if(ERKAO_GRAPHICS)
  if(WIN32)
    if(DEFINED ENV{MINGW_PREFIX} AND EXISTS "$ENV{MINGW_PREFIX}")
      list(FIND CMAKE_PREFIX_PATH "$ENV{MINGW_PREFIX}" _erkao_mingw_prefix_index)
      if(_erkao_mingw_prefix_index EQUAL -1)
        list(APPEND CMAKE_PREFIX_PATH "$ENV{MINGW_PREFIX}")
      endif()
    endif()
  endif()

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
    if(WIN32)
      message(STATUS "  Windows tips:")
      message(STATUS "    MSYS2: use the UCRT64 shell and configure with -DCMAKE_PREFIX_PATH=/ucrt64")
      message(STATUS "           or run: cmake --preset msys2-debug")
      message(STATUS "    MSVC: use vcpkg and configure with -DCMAKE_TOOLCHAIN_FILE=.../vcpkg.cmake")
      message(STATUS "           or run: cmake --preset msvc-debug")
    else()
      message(STATUS "  To enable graphics, install SDL2:")
      message(STATUS "    macOS/Linux: ./scripts/install-sdl2.sh")
    endif()
    set(ERKAO_GRAPHICS OFF)
  endif()
endif()

if(NOT ERKAO_GRAPHICS)
  target_compile_definitions(erkao PRIVATE ERKAO_HAS_GRAPHICS=0)
endif()

function(erkao_link_libs target)
  if(WIN32)
    target_link_libraries(${target} winhttp ws2_32)
  else()
    find_package(CURL REQUIRED)
    if(TARGET CURL::libcurl)
      target_link_libraries(${target} dl m CURL::libcurl)
    else()
      target_include_directories(${target} PRIVATE ${CURL_INCLUDE_DIRS})
      target_link_libraries(${target} dl m ${CURL_LIBRARIES})
    endif()
    if(APPLE)
      target_link_options(${target} PRIVATE "-Wl,-export_dynamic")
    else()
      target_link_options(${target} PRIVATE "-rdynamic")
    endif()
  endif()
endfunction()

erkao_link_libs(erkao)

if(ERKAO_DB_POSTGRES)
  find_path(ERKAO_POSTGRES_INCLUDE libpq-fe.h)
  find_library(ERKAO_POSTGRES_LIBRARY NAMES pq libpq)
  if(ERKAO_POSTGRES_INCLUDE AND ERKAO_POSTGRES_LIBRARY)
    target_compile_definitions(erkao PRIVATE ERKAO_DB_POSTGRES=1)
    target_include_directories(erkao PRIVATE ${ERKAO_POSTGRES_INCLUDE})
    target_link_libraries(erkao ${ERKAO_POSTGRES_LIBRARY})
  else()
    target_compile_definitions(erkao PRIVATE ERKAO_DB_POSTGRES=0)
    message(STATUS "PostgreSQL not found - db postgres driver disabled")
  endif()
else()
  target_compile_definitions(erkao PRIVATE ERKAO_DB_POSTGRES=0)
endif()

if(ERKAO_DB_MYSQL)
  find_path(ERKAO_MYSQL_INCLUDE mysql.h PATH_SUFFIXES mysql)
  find_library(ERKAO_MYSQL_LIBRARY NAMES mysqlclient libmysql)
  if(ERKAO_MYSQL_INCLUDE AND ERKAO_MYSQL_LIBRARY)
    target_compile_definitions(erkao PRIVATE ERKAO_DB_MYSQL=1)
    target_include_directories(erkao PRIVATE ${ERKAO_MYSQL_INCLUDE})
    target_link_libraries(erkao ${ERKAO_MYSQL_LIBRARY})
  else()
    target_compile_definitions(erkao PRIVATE ERKAO_DB_MYSQL=0)
    message(STATUS "MySQL not found - db mysql driver disabled")
  endif()
else()
  target_compile_definitions(erkao PRIVATE ERKAO_DB_MYSQL=0)
endif()

if(ERKAO_DB_MONGO)
  find_path(ERKAO_MONGO_INCLUDE mongoc/mongoc.h)
  find_path(ERKAO_BSON_INCLUDE bson/bson.h)
  find_library(ERKAO_MONGO_LIBRARY NAMES mongoc-1.0 mongoc)
  find_library(ERKAO_BSON_LIBRARY NAMES bson-1.0 bson)
  if(ERKAO_MONGO_INCLUDE AND ERKAO_BSON_INCLUDE AND ERKAO_MONGO_LIBRARY AND ERKAO_BSON_LIBRARY)
    target_compile_definitions(erkao PRIVATE ERKAO_DB_MONGO=1)
    target_include_directories(erkao PRIVATE ${ERKAO_MONGO_INCLUDE} ${ERKAO_BSON_INCLUDE})
    target_link_libraries(erkao ${ERKAO_MONGO_LIBRARY} ${ERKAO_BSON_LIBRARY})
  else()
    target_compile_definitions(erkao PRIVATE ERKAO_DB_MONGO=0)
    message(STATUS "MongoDB not found - db mongo driver disabled")
  endif()
else()
  target_compile_definitions(erkao PRIVATE ERKAO_DB_MONGO=0)
endif()

if(ERKAO_SANITIZERS)
  if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(erkao PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(erkao PRIVATE -fsanitize=address,undefined)
  else()
    message(WARNING "ERKAO_SANITIZERS is only supported with Clang/GCC.")
  endif()
endif()

if(ERKAO_FUZZ)
  if(NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "ERKAO_FUZZ requires clang and libFuzzer.")
  endif()

  set(ERKAO_FUZZ_FLAGS -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer)

  add_executable(erkao_fuzz_lexer fuzz/fuzz_lexer.c ${ERKAO_CORE_SOURCES})
  target_include_directories(erkao_fuzz_lexer PRIVATE ${ERKAO_INCLUDE_DIRS})
  target_compile_definitions(erkao_fuzz_lexer PRIVATE ERKAO_HAS_GRAPHICS=0 ERKAO_FUZZING=1)
  target_compile_options(erkao_fuzz_lexer PRIVATE ${ERKAO_FUZZ_FLAGS})
  target_link_options(erkao_fuzz_lexer PRIVATE ${ERKAO_FUZZ_FLAGS})
  erkao_apply_warnings(erkao_fuzz_lexer)
  erkao_link_libs(erkao_fuzz_lexer)

  add_executable(erkao_fuzz_parser fuzz/fuzz_parser.c ${ERKAO_CORE_SOURCES})
  target_include_directories(erkao_fuzz_parser PRIVATE ${ERKAO_INCLUDE_DIRS})
  target_compile_definitions(erkao_fuzz_parser PRIVATE ERKAO_HAS_GRAPHICS=0 ERKAO_FUZZING=1)
  target_compile_options(erkao_fuzz_parser PRIVATE ${ERKAO_FUZZ_FLAGS})
  target_link_options(erkao_fuzz_parser PRIVATE ${ERKAO_FUZZ_FLAGS})
  erkao_apply_warnings(erkao_fuzz_parser)
  erkao_link_libs(erkao_fuzz_parser)

  add_executable(erkao_fuzz_runtime fuzz/fuzz_runtime.c ${ERKAO_CORE_SOURCES})
  target_include_directories(erkao_fuzz_runtime PRIVATE ${ERKAO_INCLUDE_DIRS})
  target_compile_definitions(erkao_fuzz_runtime PRIVATE ERKAO_HAS_GRAPHICS=0 ERKAO_FUZZING=1)
  target_compile_options(erkao_fuzz_runtime PRIVATE ${ERKAO_FUZZ_FLAGS})
  target_link_options(erkao_fuzz_runtime PRIVATE ${ERKAO_FUZZ_FLAGS})
  erkao_apply_warnings(erkao_fuzz_runtime)
  erkao_link_libs(erkao_fuzz_runtime)
endif()

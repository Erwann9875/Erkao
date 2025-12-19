cmake_minimum_required(VERSION 3.16)
project(erkao C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_executable(erkao
  src/main.c
  src/lexer.c
  src/parser.c
  src/ast.c
  src/value.c
  src/interpreter.c
  src/stdlib.c
)

#ifndef ERKAO_SINGLEPASS_H
#define ERKAO_SINGLEPASS_H

#include "value.h"
#include "lexer.h"
#include "chunk.h"

typedef struct {
  int* offsets;
  int count;
  int capacity;
} JumpList;

typedef enum {
  BREAK_LOOP,
  BREAK_SWITCH
} BreakContextType;

typedef struct BreakContext {
  BreakContextType type;
  struct BreakContext* enclosing;
  int scopeDepth;
  JumpList breaks;
  JumpList continues;
} BreakContext;

typedef struct Compiler {
  VM* vm;
  const TokenArray* tokens;
  const char* source;
  const char* path;
  int current;
  bool panicMode;
  bool hadError;
  Chunk* chunk;
  int scopeDepth;
  int tempIndex;
  BreakContext* breakContext;
  struct Compiler* enclosing;
} Compiler;

ObjFunction* compile(VM* vm, const TokenArray* tokens, const char* source,
                     const char* path, bool* hadError);

#endif

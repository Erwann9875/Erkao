#ifndef ERKAO_PROGRAM_H
#define ERKAO_PROGRAM_H

#include "ast.h"

typedef struct VM VM;

typedef struct Program {
  char* source;
  char* path;
  StmtArray statements;
  int refCount;
  int running;
  struct Program* next;
} Program;

Program* programCreate(VM* vm, char* source, const char* path, StmtArray statements);
void programRetain(Program* program);
void programRelease(VM* vm, Program* program);
void programRunBegin(Program* program);
void programRunEnd(VM* vm, Program* program);
void programFreeAll(VM* vm);

#endif

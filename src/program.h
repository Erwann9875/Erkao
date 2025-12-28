#ifndef ERKAO_PROGRAM_H
#define ERKAO_PROGRAM_H

#include "value.h"

struct Program {
  char* source;
  char* path;
  ObjFunction* function;
  int refCount;
  int running;
  struct Program* next;
};

Program* programCreate(VM* vm, char* source, const char* path, ObjFunction* function);
void programRetain(Program* program);
void programRelease(VM* vm, Program* program);
void programRunBegin(Program* program);
void programRunEnd(VM* vm, Program* program);
void programFreeAll(VM* vm);

#endif

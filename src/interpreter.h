#ifndef ERKAO_INTERPRETER_H
#define ERKAO_INTERPRETER_H

#include "ast.h"
#include "value.h"

typedef struct Program Program;

typedef struct Env {
  struct Env* enclosing;
  ObjMap* values;
  struct Env* next;
  bool marked;
} Env;

typedef struct VM {
  Env* globals;
  Env* env;
  Env* envs;
  Obj* objects;
  ObjArray* args;
  ObjMap* modules;
  Program* programs;
  Program* currentProgram;
  void** pluginHandles;
  int pluginCount;
  int pluginCapacity;
  size_t gcAllocCount;
  size_t gcNext;
  bool gcPending;
  bool gcLog;
  bool hadError;
} VM;

void vmInit(VM* vm);
void vmFree(VM* vm);
void vmSetArgs(VM* vm, int argc, const char** argv);
void defineNative(VM* vm, const char* name, NativeFn function, int arity);
void defineGlobal(VM* vm, const char* name, Value value);

bool interpret(VM* vm, Program* program);

#endif

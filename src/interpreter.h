#ifndef ERKAO_INTERPRETER_H
#define ERKAO_INTERPRETER_H

#include "ast.h"
#include "value.h"

typedef struct Env {
  struct Env* enclosing;
  ObjMap* values;
  struct Env* next;
} Env;

typedef struct VM {
  Env* globals;
  Env* env;
  Env* envs;
  Obj* objects;
  ObjArray* args;
  char** sources;
  int sourceCount;
  int sourceCapacity;
  StmtArray** programs;
  int programCount;
  int programCapacity;
  bool hadError;
} VM;

void vmInit(VM* vm);
void vmFree(VM* vm);
void vmSetArgs(VM* vm, int argc, const char** argv);
void vmKeepSource(VM* vm, char* source);
void vmKeepProgram(VM* vm, StmtArray* program);
void defineNative(VM* vm, const char* name, NativeFn function, int arity);

bool interpret(VM* vm, const StmtArray* statements);

#endif

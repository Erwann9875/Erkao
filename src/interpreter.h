#ifndef ERKAO_INTERPRETER_H
#define ERKAO_INTERPRETER_H

#include "value.h"

#define FRAMES_MAX 64
#define STACK_MAX (FRAMES_MAX * 256)

typedef struct {
  ObjFunction* function;
  uint8_t* ip;
  Value* slots;
  Env* previousEnv;
  Program* previousProgram;
  Value receiver;
  int argCount;
  bool isModule;
  bool discardResult;
  ObjInstance* moduleInstance;
  ObjString* moduleAlias;
  ObjString* moduleKey;
  bool moduleHasAlias;
  bool modulePushResult;
} CallFrame;

struct Env {
  struct Env* enclosing;
  ObjMap* values;
  ObjMap* consts;
  struct Env* next;
  bool marked;
};

struct VM {
  Env* globals;
  Env* env;
  Env* envs;
  Obj* youngObjects;
  Obj* oldObjects;
  ObjArray* args;
  ObjMap* modules;
  ObjMap* strings;
  Program* programs;
  Program* currentProgram;
  CallFrame frames[FRAMES_MAX];
  int frameCount;
  Value stack[STACK_MAX];
  Value* stackTop;
  void** pluginHandles;
  int pluginCount;
  int pluginCapacity;
  size_t gcYoungBytes;
  size_t gcOldBytes;
  size_t gcEnvBytes;
  size_t gcYoungNext;
  size_t gcNext;
  bool gcPendingYoung;
  bool gcPendingFull;
  bool gcSweeping;
  bool gcLog;
  Obj** gcGrayObjects;
  size_t gcGrayObjectCount;
  size_t gcGrayObjectCapacity;
  Env** gcGrayEnvs;
  size_t gcGrayEnvCount;
  size_t gcGrayEnvCapacity;
  Obj** gcRemembered;
  size_t gcRememberedCount;
  size_t gcRememberedCapacity;
  Obj** gcSweepOld;
  Env** gcSweepEnv;
  clock_t gcLogStart;
  size_t gcLogBeforeYoung;
  size_t gcLogBeforeOld;
  size_t gcLogBeforeEnv;
  bool gcLogFullActive;
  bool hadError;
  bool debugBytecode;
  bool typecheck;
  char** modulePaths;
  int modulePathCount;
  int modulePathCapacity;
  char* projectRoot;
  char* globalPackagesDir;
  void* compiler;
};

void vmInit(VM* vm);
void vmFree(VM* vm);
void vmSetArgs(VM* vm, int argc, const char** argv);
void vmAddModulePath(VM* vm, const char* path);
void vmSetProjectRoot(VM* vm, const char* path);
void defineNative(VM* vm, const char* name, NativeFn function, int arity);
void defineGlobal(VM* vm, const char* name, Value value);

bool interpret(VM* vm, Program* program);

#endif

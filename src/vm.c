#include "interpreter_internal.h"
#include "erkao_stdlib.h"
#include "gc.h"
#include "plugin.h"
#include "program.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool envFlagEnabled(const char* name) {
  const char* value = getenv(name);
  if (!value || value[0] == '\0') return false;

  char lower[6];
  size_t i = 0;
  while (value[i] && i < sizeof(lower) - 1) {
    lower[i] = (char)tolower((unsigned char)value[i]);
    i++;
  }
  lower[i] = '\0';

  if (strcmp(lower, "0") == 0 || strcmp(lower, "no") == 0 ||
      strcmp(lower, "off") == 0 || strcmp(lower, "false") == 0) {
    return false;
  }

  return true;
}

Env* newEnv(VM* vm, Env* enclosing) {
  Env* env = (Env*)malloc(sizeof(Env));
  if (!env) {
    fprintf(stderr, "Out of memory.\n");
    exit(1);
  }
  env->enclosing = enclosing;
  env->values = newMap(vm);
  env->next = vm->envs;
  env->marked = false;
  vm->envs = env;
  gcTrackEnvAlloc(vm, sizeof(Env));
  return env;
}

bool envGet(Env* env, Token name, Value* out) {
  for (Env* current = env; current != NULL; current = current->enclosing) {
    if (mapGetByToken(current->values, name, out)) return true;
  }
  return false;
}

bool envAssign(Env* env, Token name, Value value) {
  for (Env* current = env; current != NULL; current = current->enclosing) {
    if (mapSetByTokenIfExists(current->values, name, value)) return true;
  }
  return false;
}

void envDefine(Env* env, ObjString* name, Value value) {
  mapSet(env->values, name, value);
}

void defineNative(VM* vm, const char* name, NativeFn function, int arity) {
  ObjString* nameObj = copyString(vm, name);
  ObjNative* native = newNative(vm, function, arity, nameObj);
  envDefine(vm->globals, nameObj, OBJ_VAL(native));
}

void defineGlobal(VM* vm, const char* name, Value value) {
  ObjString* nameObj = copyString(vm, name);
  envDefine(vm->globals, nameObj, value);
}

void vmInit(VM* vm) {
  vm->youngObjects = NULL;
  vm->oldObjects = NULL;
  vm->envs = NULL;
  vm->programs = NULL;
  vm->currentProgram = NULL;
  vm->pluginHandles = NULL;
  vm->pluginCount = 0;
  vm->pluginCapacity = 0;
  vm->gcYoungBytes = 0;
  vm->gcOldBytes = 0;
  vm->gcEnvBytes = 0;
  vm->gcYoungNext = GC_MIN_YOUNG_HEAP_BYTES;
  vm->gcNext = GC_MIN_HEAP_BYTES;
  vm->gcPendingYoung = false;
  vm->gcPendingFull = false;
  vm->gcSweeping = false;
  vm->gcLog = envFlagEnabled("ERKAO_GC_LOG");
  vm->gcGrayObjects = NULL;
  vm->gcGrayObjectCount = 0;
  vm->gcGrayObjectCapacity = 0;
  vm->gcGrayEnvs = NULL;
  vm->gcGrayEnvCount = 0;
  vm->gcGrayEnvCapacity = 0;
  vm->gcSweepOld = NULL;
  vm->gcSweepEnv = NULL;
  vm->gcLogStart = 0;
  vm->gcLogBeforeYoung = 0;
  vm->gcLogBeforeOld = 0;
  vm->gcLogBeforeEnv = 0;
  vm->gcLogFullActive = false;
  vm->hadError = false;
  vm->globals = newEnv(vm, NULL);
  vm->env = vm->globals;
  vm->args = newArray(vm);
  vm->modules = newMap(vm);

  defineStdlib(vm);
}

void vmFree(VM* vm) {
  pluginUnloadAll(vm);

  FREE_ARRAY(Obj*, vm->gcGrayObjects, vm->gcGrayObjectCapacity);
  FREE_ARRAY(Env*, vm->gcGrayEnvs, vm->gcGrayEnvCapacity);
  vm->gcGrayObjects = NULL;
  vm->gcGrayEnvs = NULL;
  vm->gcGrayObjectCount = 0;
  vm->gcGrayObjectCapacity = 0;
  vm->gcGrayEnvCount = 0;
  vm->gcGrayEnvCapacity = 0;

  Obj* object = vm->youngObjects;
  while (object) {
    Obj* next = object->next;
    freeObject(vm, object);
    object = next;
  }
  vm->youngObjects = NULL;

  object = vm->oldObjects;
  while (object) {
    Obj* next = object->next;
    freeObject(vm, object);
    object = next;
  }
  vm->oldObjects = NULL;

  Env* env = vm->envs;
  while (env) {
    Env* next = env->next;
    free(env);
    env = next;
  }
  vm->envs = NULL;

  programFreeAll(vm);
}

void vmSetArgs(VM* vm, int argc, const char** argv) {
  ObjArray* array = newArray(vm);
  for (int i = 0; i < argc; i++) {
    ObjString* arg = copyString(vm, argv[i]);
    arrayWrite(array, OBJ_VAL(arg));
  }
  vm->args = array;
}

#include "interpreter_internal.h"
#include "erkao_stdlib.h"
#include "db.h"
#include "gc.h"
#include "plugin.h"
#include "program.h"

#include <ctype.h>
#include <limits.h>
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

static bool parseUint64Value(const char* value, uint64_t* out) {
  if (!value || !out) return false;
  char* end = NULL;
  unsigned long long parsed = strtoull(value, &end, 10);
  if (end == value) return false;
  while (*end && isspace((unsigned char)*end)) end++;
  if (*end != '\0') return false;
  *out = (uint64_t)parsed;
  return true;
}

static bool parseIntValue(const char* value, int* out) {
  if (!value || !out) return false;
  char* end = NULL;
  long parsed = strtol(value, &end, 10);
  if (end == value) return false;
  while (*end && isspace((unsigned char)*end)) end++;
  if (*end != '\0') return false;
  if (parsed <= 0) return false;
  if (parsed > INT_MAX) parsed = INT_MAX;
  *out = (int)parsed;
  return true;
}

static bool parseSizeValue(const char* value, size_t* out) {
  if (!value || !out) return false;
  char* end = NULL;
  unsigned long long parsed = strtoull(value, &end, 10);
  if (end == value) return false;
  while (*end && isspace((unsigned char)*end)) end++;
  unsigned long long multiplier = 1;
  if (*end != '\0') {
    char suffix = (char)tolower((unsigned char)*end);
    if (suffix == 'k') {
      multiplier = 1024ULL;
    } else if (suffix == 'm') {
      multiplier = 1024ULL * 1024ULL;
    } else if (suffix == 'g') {
      multiplier = 1024ULL * 1024ULL * 1024ULL;
    } else {
      return false;
    }
    if (end[1] != '\0') return false;
  }
  unsigned long long maxValue = (unsigned long long)SIZE_MAX;
  if (parsed > maxValue / multiplier) {
    *out = SIZE_MAX;
    return true;
  }
  *out = (size_t)(parsed * multiplier);
  return true;
}

static char* copyCString(const char* src) {
  size_t length = strlen(src);
  char* out = (char*)malloc(length + 1);
  if (!out) {
    fprintf(stderr, "Out of memory.\n");
    exit(1);
  }
  memcpy(out, src, length + 1);
  return out;
}

static char* resolveGlobalPackagesDir(void) {
  const char* overridePath = getenv("ERKAO_PACKAGES");
  if (overridePath && overridePath[0] != '\0') {
    return copyCString(overridePath);
  }

#ifdef _WIN32
  const char* home = getenv("USERPROFILE");
  char* homeBuffer = NULL;
  if (!home || home[0] == '\0') {
    const char* drive = getenv("HOMEDRIVE");
    const char* path = getenv("HOMEPATH");
    if (drive && path) {
      size_t length = strlen(drive) + strlen(path);
      homeBuffer = (char*)malloc(length + 1);
      if (!homeBuffer) {
        fprintf(stderr, "Out of memory.\n");
        exit(1);
      }
      strcpy(homeBuffer, drive);
      strcat(homeBuffer, path);
      home = homeBuffer;
    }
  }
  if (!home || home[0] == '\0') {
    home = ".";
  }
  const char* suffix = "\\.erkao\\packages";
  size_t length = strlen(home) + strlen(suffix);
  char* path = (char*)malloc(length + 1);
  if (!path) {
    fprintf(stderr, "Out of memory.\n");
    exit(1);
  }
  strcpy(path, home);
  strcat(path, suffix);
  free(homeBuffer);
  return path;
#else
  const char* home = getenv("HOME");
  if (!home || home[0] == '\0') home = ".";
  const char* suffix = "/.erkao/packages";
  size_t length = strlen(home) + strlen(suffix);
  char* path = (char*)malloc(length + 1);
  if (!path) {
    fprintf(stderr, "Out of memory.\n");
    exit(1);
  }
  strcpy(path, home);
  strcat(path, suffix);
  return path;
#endif
}

void vmAddModulePath(VM* vm, const char* path) {
  if (!vm || !path || path[0] == '\0') return;
  if (vm->modulePathCapacity < vm->modulePathCount + 1) {
    int oldCapacity = vm->modulePathCapacity;
    vm->modulePathCapacity = oldCapacity == 0 ? 4 : oldCapacity * 2;
    vm->modulePaths = (char**)realloc(vm->modulePaths,
                                      sizeof(char*) * (size_t)vm->modulePathCapacity);
    if (!vm->modulePaths) {
      fprintf(stderr, "Out of memory.\n");
      exit(1);
    }
  }
  vm->modulePaths[vm->modulePathCount++] = copyCString(path);
}

void vmSetProjectRoot(VM* vm, const char* path) {
  if (!vm || !path || path[0] == '\0') return;
  free(vm->projectRoot);
  vm->projectRoot = copyCString(path);
}

static void loadEnvModulePaths(VM* vm) {
  const char* envPaths = getenv("ERKAO_PATH");
  if (!envPaths || envPaths[0] == '\0') return;
#ifdef _WIN32
  const char separator = ';';
#else
  const char separator = ':';
#endif
  const char* start = envPaths;
  const char* cursor = envPaths;
  for (;;) {
    if (*cursor == separator || *cursor == '\0') {
      size_t length = (size_t)(cursor - start);
      if (length > 0) {
        char* entry = (char*)malloc(length + 1);
        if (!entry) {
          fprintf(stderr, "Out of memory.\n");
          exit(1);
        }
        memcpy(entry, start, length);
        entry[length] = '\0';
        vmAddModulePath(vm, entry);
        free(entry);
      }
      if (*cursor == '\0') break;
      start = cursor + 1;
    }
    cursor++;
  }
}

Env* newEnv(VM* vm, Env* enclosing) {
  Env* env = (Env*)malloc(sizeof(Env));
  if (!env) {
    fprintf(stderr, "Out of memory.\n");
    exit(1);
  }
  env->enclosing = enclosing;
  env->values = newMap(vm);
  env->consts = newMap(vm);
  env->next = vm->envs;
  env->marked = false;
  vm->envs = env;
  gcTrackEnvAlloc(vm, sizeof(Env));
  return env;
}

bool envGetByName(Env* env, ObjString* name, Value* out) {
  for (Env* current = env; current != NULL; current = current->enclosing) {
    if (mapGet(current->values, name, out)) return true;
  }
  return false;
}

bool envAssignByName(Env* env, ObjString* name, Value value) {
  for (Env* current = env; current != NULL; current = current->enclosing) {
    if (mapSetIfExists(current->values, name, value)) return true;
  }
  return false;
}

void envDefine(Env* env, ObjString* name, Value value) {
  mapSet(env->values, name, value);
}

void envDefineConst(Env* env, ObjString* name, Value value) {
  mapSet(env->values, name, value);
  mapSet(env->consts, name, BOOL_VAL(true));
}

bool envIsConst(Env* env, ObjString* name) {
  for (Env* current = env; current != NULL; current = current->enclosing) {
    Value existing;
    if (mapGet(current->values, name, &existing)) {
      Value flag;
      return mapGet(current->consts, name, &flag);
    }
  }
  return false;
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
  vm->gcRemembered = NULL;
  vm->gcRememberedCount = 0;
  vm->gcRememberedCapacity = 0;
  vm->gcSweepOld = NULL;
  vm->gcSweepEnv = NULL;
  vm->gcLogStart = 0;
  vm->gcLogBeforeYoung = 0;
  vm->gcLogBeforeOld = 0;
  vm->gcLogBeforeEnv = 0;
  vm->gcLogFullActive = false;
  vm->maxHeapBytes = 0;
  vm->instructionBudget = 0;
  vm->instructionCount = 0;
  vm->maxFrames = FRAMES_MAX;
  vm->maxStackSlots = STACK_MAX;
  vm->hadError = false;
  vm->debugBytecode = false;
  vm->debugTrace = envFlagEnabled("ERKAO_DEBUG_TRACE");
  vm->debugTraceLine = -1;
  vm->debugTraceColumn = -1;
  vm->typecheck = false;
  vm->modulePaths = NULL;
  vm->modulePathCount = 0;
  vm->modulePathCapacity = 0;
  vm->projectRoot = NULL;
  vm->globalPackagesDir = resolveGlobalPackagesDir();
  vm->dbState = NULL;
  vm->frameCount = 0;
  vm->stackTop = vm->stack;
  vm->tryCount = 0;
  vm->globals = newEnv(vm, NULL);
  vm->env = vm->globals;
  vm->args = newArray(vm);
  vm->modules = newMap(vm);
  vm->strings = newMap(vm);

  {
    const char* value = getenv("ERKAO_INSTR_BUDGET");
    uint64_t budget = 0;
    if (parseUint64Value(value, &budget) && budget > 0) {
      vm->instructionBudget = budget;
    }
  }
  {
    const char* value = getenv("ERKAO_MAX_HEAP");
    size_t bytes = 0;
    if (parseSizeValue(value, &bytes) && bytes > 0) {
      vm->maxHeapBytes = bytes;
    }
  }
  {
    const char* value = getenv("ERKAO_MAX_FRAMES");
    int limit = 0;
    if (parseIntValue(value, &limit)) {
      if (limit > FRAMES_MAX) limit = FRAMES_MAX;
      vm->maxFrames = limit;
    }
  }
  {
    const char* value = getenv("ERKAO_MAX_STACK");
    int limit = 0;
    if (parseIntValue(value, &limit)) {
      if (limit > STACK_MAX) limit = STACK_MAX;
      vm->maxStackSlots = limit;
    }
  }

  loadEnvModulePaths(vm);
  defineStdlib(vm);
}

void vmFree(VM* vm) {
  dbShutdown(vm);
  pluginUnloadAll(vm);

  for (int i = 0; i < vm->modulePathCount; i++) {
    free(vm->modulePaths[i]);
  }
  free(vm->modulePaths);
  vm->modulePaths = NULL;
  vm->modulePathCount = 0;
  vm->modulePathCapacity = 0;
  free(vm->projectRoot);
  vm->projectRoot = NULL;
  free(vm->globalPackagesDir);
  vm->globalPackagesDir = NULL;

  FREE_ARRAY(Obj*, vm->gcGrayObjects, vm->gcGrayObjectCapacity);
  FREE_ARRAY(Env*, vm->gcGrayEnvs, vm->gcGrayEnvCapacity);
  FREE_ARRAY(Obj*, vm->gcRemembered, vm->gcRememberedCapacity);
  vm->gcGrayObjects = NULL;
  vm->gcGrayEnvs = NULL;
  vm->gcRemembered = NULL;
  vm->gcGrayObjectCount = 0;
  vm->gcGrayObjectCapacity = 0;
  vm->gcGrayEnvCount = 0;
  vm->gcGrayEnvCapacity = 0;
  vm->gcRememberedCount = 0;
  vm->gcRememberedCapacity = 0;

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
  ObjArray* array = newArrayWithCapacity(vm, argc);
  for (int i = 0; i < argc; i++) {
    ObjString* arg = copyString(vm, argv[i]);
    arrayWrite(array, OBJ_VAL(arg));
  }
  vm->args = array;
}

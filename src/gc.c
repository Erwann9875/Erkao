#include "gc.h"
#include "program.h"

static void markValue(VM* vm, Value value);
static void markObject(VM* vm, Obj* object);
static void markEnv(VM* vm, Env* env);
static void blackenObject(VM* vm, Obj* object);
static void blackenEnv(VM* vm, Env* env);

static void grayPushObject(VM* vm, Obj* object) {
  if (vm->gcGrayObjectCapacity < vm->gcGrayObjectCount + 1) {
    size_t oldCapacity = vm->gcGrayObjectCapacity;
    vm->gcGrayObjectCapacity = GROW_CAPACITY(oldCapacity);
    vm->gcGrayObjects = GROW_ARRAY(Obj*, vm->gcGrayObjects, oldCapacity, vm->gcGrayObjectCapacity);
  }
  vm->gcGrayObjects[vm->gcGrayObjectCount++] = object;
}

static void grayPushEnv(VM* vm, Env* env) {
  if (vm->gcGrayEnvCapacity < vm->gcGrayEnvCount + 1) {
    size_t oldCapacity = vm->gcGrayEnvCapacity;
    vm->gcGrayEnvCapacity = GROW_CAPACITY(oldCapacity);
    vm->gcGrayEnvs = GROW_ARRAY(Env*, vm->gcGrayEnvs, oldCapacity, vm->gcGrayEnvCapacity);
  }
  vm->gcGrayEnvs[vm->gcGrayEnvCount++] = env;
}

static Obj* grayPopObject(VM* vm) {
  return vm->gcGrayObjects[--vm->gcGrayObjectCount];
}

static Env* grayPopEnv(VM* vm) {
  return vm->gcGrayEnvs[--vm->gcGrayEnvCount];
}

static size_t countObjects(const VM* vm) {
  size_t count = 0;
  for (Obj* object = vm->objects; object; object = object->next) {
    count++;
  }
  return count;
}

static size_t countEnvs(const VM* vm) {
  size_t count = 0;
  for (Env* env = vm->envs; env; env = env->next) {
    count++;
  }
  return count;
}

void gcTrackAlloc(VM* vm) {
  vm->gcAllocCount++;
  if (vm->gcAllocCount > vm->gcNext) {
    if (!vm->gcPending && vm->gcLog) {
      fprintf(stderr, "[gc] threshold reached: alloc=%zu next=%zu\n",
              vm->gcAllocCount, vm->gcNext);
    }
    vm->gcPending = true;
  }
}

void gcMaybe(VM* vm) {
  if (vm->gcPending) {
    gcCollect(vm);
  }
}

static void markEnv(VM* vm, Env* env) {
  if (!env || env->marked) return;
  env->marked = true;
  grayPushEnv(vm, env);
}

static void markValue(VM* vm, Value value) {
  if (IS_OBJ(value)) {
    markObject(vm, AS_OBJ(value));
  }
}

static void markObject(VM* vm, Obj* object) {
  if (!object || object->marked) return;
  object->marked = true;
  grayPushObject(vm, object);
}

static void blackenEnv(VM* vm, Env* env) {
  markObject(vm, (Obj*)env->values);
  markEnv(vm, env->enclosing);
}

static void blackenObject(VM* vm, Obj* object) {
  switch (object->type) {
    case OBJ_STRING:
      break;
    case OBJ_FUNCTION: {
      ObjFunction* function = (ObjFunction*)object;
      markObject(vm, (Obj*)function->name);
      markEnv(vm, function->closure);
      break;
    }
    case OBJ_NATIVE: {
      ObjNative* native = (ObjNative*)object;
      markObject(vm, (Obj*)native->name);
      break;
    }
    case OBJ_CLASS: {
      ObjClass* klass = (ObjClass*)object;
      markObject(vm, (Obj*)klass->name);
      markObject(vm, (Obj*)klass->methods);
      break;
    }
    case OBJ_INSTANCE: {
      ObjInstance* instance = (ObjInstance*)object;
      markObject(vm, (Obj*)instance->klass);
      markObject(vm, (Obj*)instance->fields);
      break;
    }
    case OBJ_ARRAY: {
      ObjArray* array = (ObjArray*)object;
      for (int i = 0; i < array->count; i++) {
        markValue(vm, array->items[i]);
      }
      break;
    }
    case OBJ_MAP: {
      ObjMap* map = (ObjMap*)object;
      for (int i = 0; i < map->count; i++) {
        markObject(vm, (Obj*)map->entries[i].key);
        markValue(vm, map->entries[i].value);
      }
      break;
    }
    case OBJ_BOUND_METHOD: {
      ObjBoundMethod* bound = (ObjBoundMethod*)object;
      markValue(vm, bound->receiver);
      markObject(vm, (Obj*)bound->method);
      break;
    }
  }
}

static void markRoots(VM* vm) {
  markEnv(vm, vm->globals);
  markEnv(vm, vm->env);
  if (vm->args) {
    markObject(vm, (Obj*)vm->args);
  }
  if (vm->modules) {
    markObject(vm, (Obj*)vm->modules);
  }
}

void freeObject(VM* vm, Obj* object) {
  switch (object->type) {
    case OBJ_STRING: {
      ObjString* string = (ObjString*)object;
      free(string->chars);
      free(string);
      return;
    }
    case OBJ_FUNCTION: {
      ObjFunction* function = (ObjFunction*)object;
      programRelease(vm, function->program);
      free(function);
      return;
    }
    case OBJ_NATIVE:
      free(object);
      return;
    case OBJ_CLASS:
      free(object);
      return;
    case OBJ_INSTANCE:
      free(object);
      return;
    case OBJ_ARRAY: {
      ObjArray* array = (ObjArray*)object;
      FREE_ARRAY(Value, array->items, array->capacity);
      free(array);
      return;
    }
    case OBJ_MAP: {
      ObjMap* map = (ObjMap*)object;
      FREE_ARRAY(MapEntryValue, map->entries, map->capacity);
      free(map);
      return;
    }
    case OBJ_BOUND_METHOD:
      free(object);
      return;
  }
}

static size_t sweepObjects(VM* vm) {
  size_t alive = 0;
  Obj* previous = NULL;
  Obj* object = vm->objects;
  while (object) {
    if (object->marked) {
      object->marked = false;
      alive++;
      previous = object;
      object = object->next;
    } else {
      Obj* unreached = object;
      object = object->next;
      if (previous) {
        previous->next = object;
      } else {
        vm->objects = object;
      }
      freeObject(vm, unreached);
    }
  }
  return alive;
}

static size_t sweepEnvs(VM* vm) {
  size_t alive = 0;
  Env* previous = NULL;
  Env* env = vm->envs;
  while (env) {
    if (env->marked) {
      env->marked = false;
      alive++;
      previous = env;
      env = env->next;
    } else {
      Env* unreached = env;
      env = env->next;
      if (previous) {
        previous->next = env;
      } else {
        vm->envs = env;
      }
      free(unreached);
    }
  }
  return alive;
}

void gcCollect(VM* vm) {
  if (!vm) return;
  vm->gcPending = false;
  vm->gcGrayObjectCount = 0;
  vm->gcGrayEnvCount = 0;

  size_t beforeObjects = 0;
  size_t beforeEnvs = 0;
  clock_t start = 0;
  if (vm->gcLog) {
    beforeObjects = countObjects(vm);
    beforeEnvs = countEnvs(vm);
    start = clock();
    fprintf(stderr, "[gc] begin: objects=%zu envs=%zu alloc=%zu next=%zu\n",
            beforeObjects, beforeEnvs, vm->gcAllocCount, vm->gcNext);
  }

  markRoots(vm);

  while (vm->gcGrayObjectCount > 0 || vm->gcGrayEnvCount > 0) {
    if (vm->gcGrayObjectCount > 0) {
      blackenObject(vm, grayPopObject(vm));
    } else {
      blackenEnv(vm, grayPopEnv(vm));
    }
  }

  size_t aliveObjects = sweepObjects(vm);
  size_t aliveEnvs = sweepEnvs(vm);

  vm->gcAllocCount = aliveObjects + aliveEnvs;
  vm->gcNext = vm->gcAllocCount * 2;
  if (vm->gcNext < 1024) {
    vm->gcNext = 1024;
  }

  if (vm->gcLog) {
    clock_t end = clock();
    size_t freedObjects = beforeObjects > aliveObjects ? beforeObjects - aliveObjects : 0;
    size_t freedEnvs = beforeEnvs > aliveEnvs ? beforeEnvs - aliveEnvs : 0;
    double ms = (double)(end - start) * 1000.0 / (double)CLOCKS_PER_SEC;
    fprintf(stderr,
            "[gc] end: objects=%zu->%zu (-%zu) envs=%zu->%zu (-%zu) next=%zu time=%.2fms\n",
            beforeObjects, aliveObjects, freedObjects,
            beforeEnvs, aliveEnvs, freedEnvs,
            vm->gcNext, ms);
  }
}

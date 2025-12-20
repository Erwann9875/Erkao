#include "gc.h"
#include "program.h"

static void markValue(VM* vm, Value value);
static void markObject(VM* vm, Obj* object);
static void markEnv(VM* vm, Env* env);
static void blackenObject(VM* vm, Obj* object);
static void blackenEnv(VM* vm, Env* env);

static void markYoungValue(VM* vm, Value value);
static void markYoungObject(VM* vm, Obj* object);
static void blackenYoungObject(VM* vm, Obj* object);
static void markYoungFromEnv(VM* vm, Env* env);

static void gcCollectYoung(VM* vm);
static bool sweepOldStep(VM* vm, size_t budget);
static void finishFullSweep(VM* vm);

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

static size_t countObjectList(const Obj* head) {
  size_t count = 0;
  for (const Obj* object = head; object; object = object->next) {
    count++;
  }
  return count;
}

static size_t countEnvs(const VM* vm) {
  size_t count = 0;
  for (const Env* env = vm->envs; env; env = env->next) {
    count++;
  }
  return count;
}

static size_t totalHeapBytes(const VM* vm) {
  return vm->gcYoungBytes + vm->gcOldBytes + vm->gcEnvBytes;
}

void gcTrackAlloc(VM* vm, Obj* object) {
  if (!vm || !object) return;

  vm->gcYoungBytes += object->size;
  if (!vm->gcPendingYoung && vm->gcYoungBytes > vm->gcYoungNext) {
    if (vm->gcLog) {
      fprintf(stderr, "[gc] young threshold reached: bytes=%zu next=%zu\n",
              vm->gcYoungBytes, vm->gcYoungNext);
    }
    vm->gcPendingYoung = true;
  }

  size_t total = totalHeapBytes(vm);
  if (!vm->gcPendingFull && total > vm->gcNext) {
    if (vm->gcLog) {
      fprintf(stderr, "[gc] full threshold reached: bytes=%zu next=%zu\n",
              total, vm->gcNext);
    }
    vm->gcPendingFull = true;
  }
}

void gcTrackResize(VM* vm, Obj* object, size_t oldSize, size_t newSize) {
  if (!vm || !object || oldSize == newSize) return;

  if (newSize > oldSize) {
    size_t delta = newSize - oldSize;
    if (object->generation == OBJ_GEN_OLD) {
      vm->gcOldBytes += delta;
    } else {
      vm->gcYoungBytes += delta;
      if (!vm->gcPendingYoung && vm->gcYoungBytes > vm->gcYoungNext) {
        if (vm->gcLog) {
          fprintf(stderr, "[gc] young threshold reached: bytes=%zu next=%zu\n",
                  vm->gcYoungBytes, vm->gcYoungNext);
        }
        vm->gcPendingYoung = true;
      }
    }
  } else {
    size_t delta = oldSize - newSize;
    if (object->generation == OBJ_GEN_OLD) {
      if (vm->gcOldBytes > delta) vm->gcOldBytes -= delta;
    } else {
      if (vm->gcYoungBytes > delta) vm->gcYoungBytes -= delta;
    }
  }

  size_t total = totalHeapBytes(vm);
  if (!vm->gcPendingFull && total > vm->gcNext) {
    if (vm->gcLog) {
      fprintf(stderr, "[gc] full threshold reached: bytes=%zu next=%zu\n",
              total, vm->gcNext);
    }
    vm->gcPendingFull = true;
  }
}

void gcTrackEnvAlloc(VM* vm, size_t size) {
  if (!vm) return;
  vm->gcEnvBytes += size;

  size_t total = totalHeapBytes(vm);
  if (!vm->gcPendingFull && total > vm->gcNext) {
    if (vm->gcLog) {
      fprintf(stderr, "[gc] full threshold reached: bytes=%zu next=%zu\n",
              total, vm->gcNext);
    }
    vm->gcPendingFull = true;
  }
}

void gcMaybe(VM* vm) {
  if (!vm) return;

  if (vm->gcSweeping) {
    if (sweepOldStep(vm, GC_SWEEP_BATCH)) {
      finishFullSweep(vm);
    }
    return;
  }

  if (vm->gcPendingFull) {
    gcCollect(vm);
    return;
  }

  if (vm->gcPendingYoung) {
    gcCollectYoung(vm);
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

static void markYoungValue(VM* vm, Value value) {
  if (IS_OBJ(value)) {
    markYoungObject(vm, AS_OBJ(value));
  }
}

static void markYoungObject(VM* vm, Obj* object) {
  if (!object || object->generation != OBJ_GEN_YOUNG) return;
  if (object->marked) return;
  object->marked = true;
  grayPushObject(vm, object);
}

static void markYoungFromEnv(VM* vm, Env* env) {
  for (Env* current = env; current; current = current->enclosing) {
    if (current->values && current->values->obj.generation == OBJ_GEN_YOUNG) {
      markYoungObject(vm, (Obj*)current->values);
    }
  }
}

static void blackenYoungObject(VM* vm, Obj* object) {
  switch (object->type) {
    case OBJ_STRING:
      break;
    case OBJ_FUNCTION: {
      ObjFunction* function = (ObjFunction*)object;
      markYoungObject(vm, (Obj*)function->name);
      markYoungFromEnv(vm, function->closure);
      break;
    }
    case OBJ_NATIVE: {
      ObjNative* native = (ObjNative*)object;
      markYoungObject(vm, (Obj*)native->name);
      break;
    }
    case OBJ_CLASS: {
      ObjClass* klass = (ObjClass*)object;
      markYoungObject(vm, (Obj*)klass->name);
      markYoungObject(vm, (Obj*)klass->methods);
      break;
    }
    case OBJ_INSTANCE: {
      ObjInstance* instance = (ObjInstance*)object;
      markYoungObject(vm, (Obj*)instance->klass);
      markYoungObject(vm, (Obj*)instance->fields);
      break;
    }
    case OBJ_ARRAY: {
      ObjArray* array = (ObjArray*)object;
      for (int i = 0; i < array->count; i++) {
        markYoungValue(vm, array->items[i]);
      }
      break;
    }
    case OBJ_MAP: {
      ObjMap* map = (ObjMap*)object;
      for (int i = 0; i < map->count; i++) {
        markYoungObject(vm, (Obj*)map->entries[i].key);
        markYoungValue(vm, map->entries[i].value);
      }
      break;
    }
    case OBJ_BOUND_METHOD: {
      ObjBoundMethod* bound = (ObjBoundMethod*)object;
      markYoungValue(vm, bound->receiver);
      markYoungObject(vm, (Obj*)bound->method);
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

static void markYoungRoots(VM* vm) {
  if (vm->args) {
    markYoungObject(vm, (Obj*)vm->args);
  }
  if (vm->modules) {
    markYoungObject(vm, (Obj*)vm->modules);
  }
  markYoungFromEnv(vm, vm->globals);
  markYoungFromEnv(vm, vm->env);
}

static void traceFull(VM* vm) {
  while (vm->gcGrayObjectCount > 0 || vm->gcGrayEnvCount > 0) {
    if (vm->gcGrayObjectCount > 0) {
      blackenObject(vm, grayPopObject(vm));
    } else {
      blackenEnv(vm, grayPopEnv(vm));
    }
  }
}

static void traceYoung(VM* vm) {
  while (vm->gcGrayObjectCount > 0) {
    blackenYoungObject(vm, grayPopObject(vm));
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

static void updateYoungNext(VM* vm) {
  size_t next = vm->gcYoungBytes * GC_YOUNG_GROW_FACTOR;
  if (next < GC_MIN_YOUNG_HEAP_BYTES) {
    next = GC_MIN_YOUNG_HEAP_BYTES;
  }
  vm->gcYoungNext = next;
}

static void updateFullNext(VM* vm) {
  size_t total = totalHeapBytes(vm);
  size_t next = total * GC_HEAP_GROW_FACTOR;
  if (next < GC_MIN_HEAP_BYTES) {
    next = GC_MIN_HEAP_BYTES;
  }
  vm->gcNext = next;
}

static void sweepYoung(VM* vm, bool fullGc) {
  Obj* newYoung = NULL;
  size_t youngBytes = 0;

  Obj* object = vm->youngObjects;
  while (object) {
    Obj* next = object->next;
    if (object->marked) {
      bool promote = false;
      if (object->age < UINT8_MAX) {
        object->age++;
      }
      if (object->age >= GC_PROMOTION_AGE) {
        promote = true;
      }

      if (promote) {
        object->generation = OBJ_GEN_OLD;
        object->age = 0;
        if (!fullGc) {
          object->marked = false;
        }
        object->next = vm->oldObjects;
        vm->oldObjects = object;
        vm->gcOldBytes += object->size;
      } else {
        object->marked = false;
        object->next = newYoung;
        newYoung = object;
        youngBytes += object->size;
      }
    } else {
      freeObject(vm, object);
    }
    object = next;
  }

  vm->youngObjects = newYoung;
  vm->gcYoungBytes = youngBytes;
}

static bool sweepOldStep(VM* vm, size_t budget) {
  while (budget > 0 && vm->gcSweepOld && *vm->gcSweepOld) {
    Obj* object = *vm->gcSweepOld;
    if (object->marked) {
      object->marked = false;
      vm->gcSweepOld = &object->next;
    } else {
      *vm->gcSweepOld = object->next;
      if (vm->gcOldBytes > object->size) {
        vm->gcOldBytes -= object->size;
      } else {
        vm->gcOldBytes = 0;
      }
      freeObject(vm, object);
    }
    budget--;
  }

  if (vm->gcSweepOld && *vm->gcSweepOld == NULL) {
    vm->gcSweepOld = NULL;
  }

  while (budget > 0 && vm->gcSweepEnv && *vm->gcSweepEnv) {
    Env* env = *vm->gcSweepEnv;
    if (env->marked) {
      env->marked = false;
      vm->gcSweepEnv = &env->next;
    } else {
      *vm->gcSweepEnv = env->next;
      if (vm->gcEnvBytes > sizeof(Env)) {
        vm->gcEnvBytes -= sizeof(Env);
      } else {
        vm->gcEnvBytes = 0;
      }
      free(env);
    }
    budget--;
  }

  if (vm->gcSweepEnv && *vm->gcSweepEnv == NULL) {
    vm->gcSweepEnv = NULL;
  }

  return vm->gcSweepOld == NULL && vm->gcSweepEnv == NULL;
}

static void finishFullSweep(VM* vm) {
  vm->gcSweeping = false;
  updateFullNext(vm);

  if (vm->gcLog && vm->gcLogFullActive) {
    clock_t end = clock();
    size_t afterYoung = vm->gcYoungBytes;
    size_t afterOld = vm->gcOldBytes;
    size_t afterEnv = vm->gcEnvBytes;
    double ms = (double)(end - vm->gcLogStart) * 1000.0 / (double)CLOCKS_PER_SEC;
    fprintf(stderr,
            "[gc] full end: young=%zu->%zu old=%zu->%zu env=%zu->%zu next=%zu time=%.2fms\n",
            vm->gcLogBeforeYoung, afterYoung,
            vm->gcLogBeforeOld, afterOld,
            vm->gcLogBeforeEnv, afterEnv,
            vm->gcNext, ms);
    vm->gcLogFullActive = false;
  }
}

static void gcCollectYoung(VM* vm) {
  if (!vm) return;
  vm->gcPendingYoung = false;
  vm->gcGrayObjectCount = 0;
  vm->gcGrayEnvCount = 0;

  size_t beforeYoung = vm->gcYoungBytes;
  if (vm->gcLog) {
    fprintf(stderr, "[gc] minor begin: young=%zu old=%zu env=%zu nextY=%zu\n",
            vm->gcYoungBytes, vm->gcOldBytes, vm->gcEnvBytes, vm->gcYoungNext);
  }

  markYoungRoots(vm);

  for (Obj* object = vm->oldObjects; object; object = object->next) {
    blackenYoungObject(vm, object);
  }

  traceYoung(vm);
  sweepYoung(vm, false);
  updateYoungNext(vm);

  if (vm->gcLog) {
    fprintf(stderr, "[gc] minor end: young=%zu->%zu old=%zu env=%zu nextY=%zu\n",
            beforeYoung, vm->gcYoungBytes, vm->gcOldBytes, vm->gcEnvBytes, vm->gcYoungNext);
  }

  if (!vm->gcPendingFull && totalHeapBytes(vm) > vm->gcNext) {
    vm->gcPendingFull = true;
  }
}

void gcCollect(VM* vm) {
  if (!vm) return;
  vm->gcPendingFull = false;
  vm->gcPendingYoung = false;
  vm->gcGrayObjectCount = 0;
  vm->gcGrayEnvCount = 0;

  if (vm->gcLog) {
    vm->gcLogBeforeYoung = vm->gcYoungBytes;
    vm->gcLogBeforeOld = vm->gcOldBytes;
    vm->gcLogBeforeEnv = vm->gcEnvBytes;
    vm->gcLogStart = clock();
    vm->gcLogFullActive = true;
    fprintf(stderr, "[gc] full begin: young=%zu old=%zu env=%zu total=%zu next=%zu\n",
            vm->gcYoungBytes, vm->gcOldBytes, vm->gcEnvBytes,
            totalHeapBytes(vm), vm->gcNext);
  }

  markRoots(vm);
  traceFull(vm);
  sweepYoung(vm, true);
  updateYoungNext(vm);

  vm->gcSweepOld = &vm->oldObjects;
  vm->gcSweepEnv = &vm->envs;
  vm->gcSweeping = true;

  if (sweepOldStep(vm, GC_SWEEP_BATCH)) {
    finishFullSweep(vm);
  }
}

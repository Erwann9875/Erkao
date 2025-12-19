#include "gc.h"
#include "program.h"

static void markValue(VM* vm, Value value);
static void markObject(VM* vm, Obj* object);
static void markEnv(VM* vm, Env* env);

void gcTrackAlloc(VM* vm) {
  vm->gcAllocCount++;
  if (vm->gcAllocCount > vm->gcNext) {
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
  markObject(vm, (Obj*)env->values);
  markEnv(vm, env->enclosing);
}

static void markValue(VM* vm, Value value) {
  if (IS_OBJ(value)) {
    markObject(vm, AS_OBJ(value));
  }
}

static void markObject(VM* vm, Obj* object) {
  if (!object || object->marked) return;
  object->marked = true;

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

  markRoots(vm);

  size_t aliveObjects = sweepObjects(vm);
  size_t aliveEnvs = sweepEnvs(vm);

  vm->gcAllocCount = aliveObjects + aliveEnvs;
  vm->gcNext = vm->gcAllocCount * 2;
  if (vm->gcNext < 1024) {
    vm->gcNext = 1024;
  }
}

#include "gc_internal.h"
#include "chunk.h"
#include "program.h"

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
      if (function->chunk) {
        freeChunk(function->chunk);
        free(function->chunk);
      }
      FREE_ARRAY(ObjString*, function->params, function->arity);
      programRelease(vm, function->program);
      free(function);
      return;
    }
    case OBJ_NATIVE:
      free(object);
      return;
    case OBJ_ENUM_CTOR:
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

void sweepYoung(VM* vm, bool fullGc) {
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
        object->remembered = false;
        if (!fullGc) {
          object->marked = false;
        }
        object->next = vm->oldObjects;
        vm->oldObjects = object;
        vm->gcOldBytes += object->size;
        gcRememberObjectIfYoungRefs(vm, object);
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

bool sweepOldStep(VM* vm, size_t budget) {
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

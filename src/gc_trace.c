#include "gc_internal.h"
#include "chunk.h"
#include "singlepass.h"

static void markValue(VM* vm, Value value);
static void markObject(VM* vm, Obj* object);
static void markEnv(VM* vm, Env* env);
static void blackenObject(VM* vm, Obj* object);
static void blackenEnv(VM* vm, Env* env);
static void markChunk(VM* vm, Chunk* chunk);

static void markYoungValue(VM* vm, Value value);
static void markYoungObject(VM* vm, Obj* object);
static void markYoungFromEnv(VM* vm, Env* env);
static void markYoungChunk(VM* vm, Chunk* chunk);
static bool valueHasYoung(Value value);
static bool envHasYoungValues(Env* env);

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
  markObject(vm, (Obj*)env->consts);
  markEnv(vm, env->enclosing);
}

static void blackenObject(VM* vm, Obj* object) {
  switch (object->type) {
    case OBJ_STRING:
      break;
    case OBJ_FUNCTION: {
      ObjFunction* function = (ObjFunction*)object;
      markObject(vm, (Obj*)function->name);
      for (int i = 0; i < function->arity; i++) {
        markObject(vm, (Obj*)function->params[i]);
      }
      markEnv(vm, function->closure);
      markChunk(vm, function->chunk);
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
      for (int i = 0; i < map->capacity; i++) {
        if (!map->entries[i].key) continue;
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
    if (current->consts && current->consts->obj.generation == OBJ_GEN_YOUNG) {
      markYoungObject(vm, (Obj*)current->consts);
    }
  }
}

void blackenYoungObject(VM* vm, Obj* object) {
  switch (object->type) {
    case OBJ_STRING:
      break;
    case OBJ_FUNCTION: {
      ObjFunction* function = (ObjFunction*)object;
      markYoungObject(vm, (Obj*)function->name);
      for (int i = 0; i < function->arity; i++) {
        markYoungObject(vm, (Obj*)function->params[i]);
      }
      markYoungFromEnv(vm, function->closure);
      markYoungChunk(vm, function->chunk);
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
      for (int i = 0; i < map->capacity; i++) {
        if (!map->entries[i].key) continue;
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

static void markChunk(VM* vm, Chunk* chunk) {
  if (!chunk) return;
  for (int i = 0; i < chunk->constantsCount; i++) {
    markValue(vm, chunk->constants[i]);
  }
}

static void markYoungChunk(VM* vm, Chunk* chunk) {
  if (!chunk) return;
  for (int i = 0; i < chunk->constantsCount; i++) {
    markYoungValue(vm, chunk->constants[i]);
  }
}

static bool valueHasYoung(Value value) {
  return IS_OBJ(value) && AS_OBJ(value)->generation == OBJ_GEN_YOUNG;
}

static bool envHasYoungValues(Env* env) {
  for (Env* current = env; current; current = current->enclosing) {
    if (current->values && current->values->obj.generation == OBJ_GEN_YOUNG) {
      return true;
    }
    if (current->consts && current->consts->obj.generation == OBJ_GEN_YOUNG) {
      return true;
    }
  }
  return false;
}

void markRoots(VM* vm) {
  markEnv(vm, vm->globals);
  markEnv(vm, vm->env);
  if (vm->args) {
    markObject(vm, (Obj*)vm->args);
  }
  if (vm->modules) {
    markObject(vm, (Obj*)vm->modules);
  }
  if (vm->strings) {
    markObject(vm, (Obj*)vm->strings);
  }
  for (Value* slot = vm->stack; slot < vm->stackTop; slot++) {
    markValue(vm, *slot);
  }
  for (int i = 0; i < vm->frameCount; i++) {
    markObject(vm, (Obj*)vm->frames[i].function);
    markValue(vm, vm->frames[i].receiver);
    markEnv(vm, vm->frames[i].previousEnv);
    if (vm->frames[i].moduleInstance) {
      markObject(vm, (Obj*)vm->frames[i].moduleInstance);
    }
    if (vm->frames[i].moduleKey) {
      markObject(vm, (Obj*)vm->frames[i].moduleKey);
    }
    if (vm->frames[i].moduleAlias) {
      markObject(vm, (Obj*)vm->frames[i].moduleAlias);
    }
  }

  if (vm->compiler) {
    Compiler* c = (Compiler*)vm->compiler;
    while (c) {
      markChunk(vm, c->chunk);
      c = c->enclosing;
    }
  }
}

void markYoungRoots(VM* vm) {
  if (vm->args) {
    markYoungObject(vm, (Obj*)vm->args);
  }
  if (vm->modules) {
    markYoungObject(vm, (Obj*)vm->modules);
  }
  if (vm->strings) {
    markYoungObject(vm, (Obj*)vm->strings);
  }
  markYoungFromEnv(vm, vm->globals);
  markYoungFromEnv(vm, vm->env);
  for (Value* slot = vm->stack; slot < vm->stackTop; slot++) {
    markYoungValue(vm, *slot);
  }
  for (int i = 0; i < vm->frameCount; i++) {
    markYoungObject(vm, (Obj*)vm->frames[i].function);
    markYoungValue(vm, vm->frames[i].receiver);
    markYoungFromEnv(vm, vm->frames[i].previousEnv);
    if (vm->frames[i].moduleInstance) {
      markYoungObject(vm, (Obj*)vm->frames[i].moduleInstance);
    }
    if (vm->frames[i].moduleKey) {
      markYoungObject(vm, (Obj*)vm->frames[i].moduleKey);
    }
    if (vm->frames[i].moduleAlias) {
      markYoungObject(vm, (Obj*)vm->frames[i].moduleAlias);
    }
  }
}

void traceFull(VM* vm) {
  while (vm->gcGrayObjectCount > 0 || vm->gcGrayEnvCount > 0) {
    if (vm->gcGrayObjectCount > 0) {
      blackenObject(vm, grayPopObject(vm));
    } else {
      blackenEnv(vm, grayPopEnv(vm));
    }
  }
}

void traceYoung(VM* vm) {
  while (vm->gcGrayObjectCount > 0) {
    blackenYoungObject(vm, grayPopObject(vm));
  }
}

bool gcObjectHasYoungRefs(Obj* object) {
  if (!object) return false;

  switch (object->type) {
    case OBJ_STRING:
      return false;
    case OBJ_FUNCTION: {
      ObjFunction* function = (ObjFunction*)object;
      if (function->name && function->name->obj.generation == OBJ_GEN_YOUNG) {
        return true;
      }
      for (int i = 0; i < function->arity; i++) {
        if (function->params[i] && function->params[i]->obj.generation == OBJ_GEN_YOUNG) {
          return true;
        }
      }
      if (envHasYoungValues(function->closure)) return true;
      if (function->chunk) {
        for (int i = 0; i < function->chunk->constantsCount; i++) {
          if (valueHasYoung(function->chunk->constants[i])) return true;
        }
      }
      return false;
    }
    case OBJ_NATIVE: {
      ObjNative* native = (ObjNative*)object;
      return native->name && native->name->obj.generation == OBJ_GEN_YOUNG;
    }
    case OBJ_CLASS: {
      ObjClass* klass = (ObjClass*)object;
      if (klass->name && klass->name->obj.generation == OBJ_GEN_YOUNG) return true;
      return klass->methods && klass->methods->obj.generation == OBJ_GEN_YOUNG;
    }
    case OBJ_INSTANCE: {
      ObjInstance* instance = (ObjInstance*)object;
      if (instance->klass && instance->klass->obj.generation == OBJ_GEN_YOUNG) return true;
      return instance->fields && instance->fields->obj.generation == OBJ_GEN_YOUNG;
    }
    case OBJ_ARRAY: {
      ObjArray* array = (ObjArray*)object;
      for (int i = 0; i < array->count; i++) {
        if (valueHasYoung(array->items[i])) return true;
      }
      return false;
    }
    case OBJ_MAP: {
      ObjMap* map = (ObjMap*)object;
      for (int i = 0; i < map->capacity; i++) {
        if (!map->entries[i].key) continue;
        if (map->entries[i].key->obj.generation == OBJ_GEN_YOUNG) return true;
        if (valueHasYoung(map->entries[i].value)) return true;
      }
      return false;
    }
    case OBJ_BOUND_METHOD: {
      ObjBoundMethod* bound = (ObjBoundMethod*)object;
      if (valueHasYoung(bound->receiver)) return true;
      return bound->method && bound->method->obj.generation == OBJ_GEN_YOUNG;
    }
  }

  return false;
}

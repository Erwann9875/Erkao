#include "stdlib_internal.h"

static ObjString* expectStringArg(VM* vm, Value value, const char* message) {
  if (!isObjType(value, OBJ_STRING)) {
    runtimeErrorValue(vm, message);
    return NULL;
  }
  return (ObjString*)AS_OBJ(value);
}

static ObjMap* expectMapArg(VM* vm, Value value, const char* message) {
  if (!isObjType(value, OBJ_MAP)) {
    runtimeErrorValue(vm, message);
    return NULL;
  }
  return (ObjMap*)AS_OBJ(value);
}

static ObjMap* diGetMapField(VM* vm, ObjMap* container, const char* field, const char* message) {
  ObjString* key = copyString(vm, field);
  Value value;
  if (!mapGet(container, key, &value) || !isObjType(value, OBJ_MAP)) {
    runtimeErrorValue(vm, message);
    return NULL;
  }
  return (ObjMap*)AS_OBJ(value);
}

static bool diIsCallable(Value value) {
  return isObjType(value, OBJ_NATIVE) ||
         isObjType(value, OBJ_FUNCTION) ||
         isObjType(value, OBJ_CLASS);
}

static Value nativeDiContainer(VM* vm, int argc, Value* args) {
  (void)argc;
  (void)args;
  ObjMap* container = newMap(vm);
  mapSet(container, copyString(vm, "providers"), OBJ_VAL(newMap(vm)));
  mapSet(container, copyString(vm, "instances"), OBJ_VAL(newMap(vm)));
  mapSet(container, copyString(vm, "singletons"), OBJ_VAL(newMap(vm)));
  return OBJ_VAL(container);
}

static Value nativeDiBind(VM* vm, int argc, Value* args) {
  (void)argc;
  ObjMap* container = expectMapArg(vm, args[0], "di.bind expects a container map.");
  ObjString* name = expectStringArg(vm, args[1], "di.bind expects a name string.");
  if (!container || !name) return NULL_VAL;
  ObjMap* providers = diGetMapField(vm, container, "providers", "di.bind expects a container.");
  ObjMap* singletons = diGetMapField(vm, container, "singletons", "di.bind expects a container.");
  ObjMap* instances = diGetMapField(vm, container, "instances", "di.bind expects a container.");
  if (!providers || !singletons || !instances) return NULL_VAL;
  mapSet(providers, name, args[2]);
  mapSet(singletons, name, BOOL_VAL(false));
  mapSet(instances, name, NULL_VAL);
  return NULL_VAL;
}

static Value nativeDiSingleton(VM* vm, int argc, Value* args) {
  (void)argc;
  ObjMap* container = expectMapArg(vm, args[0], "di.singleton expects a container map.");
  ObjString* name = expectStringArg(vm, args[1], "di.singleton expects a name string.");
  if (!container || !name) return NULL_VAL;
  ObjMap* providers = diGetMapField(vm, container, "providers", "di.singleton expects a container.");
  ObjMap* singletons = diGetMapField(vm, container, "singletons", "di.singleton expects a container.");
  ObjMap* instances = diGetMapField(vm, container, "instances", "di.singleton expects a container.");
  if (!providers || !singletons || !instances) return NULL_VAL;
  mapSet(providers, name, args[2]);
  mapSet(singletons, name, BOOL_VAL(true));
  mapSet(instances, name, NULL_VAL);
  return NULL_VAL;
}

static Value nativeDiValue(VM* vm, int argc, Value* args) {
  (void)argc;
  ObjMap* container = expectMapArg(vm, args[0], "di.value expects a container map.");
  ObjString* name = expectStringArg(vm, args[1], "di.value expects a name string.");
  if (!container || !name) return NULL_VAL;
  ObjMap* providers = diGetMapField(vm, container, "providers", "di.value expects a container.");
  ObjMap* singletons = diGetMapField(vm, container, "singletons", "di.value expects a container.");
  ObjMap* instances = diGetMapField(vm, container, "instances", "di.value expects a container.");
  if (!providers || !singletons || !instances) return NULL_VAL;
  mapSet(providers, name, args[2]);
  mapSet(singletons, name, BOOL_VAL(true));
  mapSet(instances, name, args[2]);
  return NULL_VAL;
}

static Value nativeDiResolve(VM* vm, int argc, Value* args) {
  (void)argc;
  ObjMap* container = expectMapArg(vm, args[0], "di.resolve expects a container map.");
  ObjString* name = expectStringArg(vm, args[1], "di.resolve expects a name string.");
  if (!container || !name) return NULL_VAL;
  ObjMap* providers = diGetMapField(vm, container, "providers", "di.resolve expects a container.");
  ObjMap* singletons = diGetMapField(vm, container, "singletons", "di.resolve expects a container.");
  ObjMap* instances = diGetMapField(vm, container, "instances", "di.resolve expects a container.");
  if (!providers || !singletons || !instances) return NULL_VAL;

  Value singletonFlag;
  bool isSingleton = false;
  if (mapGet(singletons, name, &singletonFlag) && IS_BOOL(singletonFlag)) {
    isSingleton = AS_BOOL(singletonFlag);
  }

  if (isSingleton) {
    Value cached;
    if (mapGet(instances, name, &cached) && !IS_NULL(cached)) {
      return cached;
    }
  }

  Value provider;
  if (!mapGet(providers, name, &provider)) {
    return runtimeErrorValue(vm, "di.resolve missing provider.");
  }

  Value instance = provider;
  if (diIsCallable(provider)) {
    if (!vmCallValue(vm, provider, 0, NULL, &instance)) {
      return NULL_VAL;
    }
  }

  if (isSingleton) {
    mapSet(instances, name, instance);
  }
  return instance;
}


void stdlib_register_di(VM* vm, ObjInstance* module) {
  moduleAdd(vm, module, "container", nativeDiContainer, 0);
  moduleAdd(vm, module, "bind", nativeDiBind, 3);
  moduleAdd(vm, module, "singleton", nativeDiSingleton, 3);
  moduleAdd(vm, module, "value", nativeDiValue, 3);
  moduleAdd(vm, module, "resolve", nativeDiResolve, 2);
}

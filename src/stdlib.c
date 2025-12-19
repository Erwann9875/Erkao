#include "stdlib.h"

static Value runtimeErrorValue(VM* vm, const char* message) {
  fprintf(stderr, "RuntimeError: %s\n", message);
  vm->hadError = true;
  return NULL_VAL;
}

static Value nativePrint(VM* vm, int argc, Value* args) {
  (void)vm;
  for (int i = 0; i < argc; i++) {
    if (i > 0) printf(" ");
    printValue(args[i]);
  }
  printf("\n");
  return NULL_VAL;
}

static Value nativeClock(VM* vm, int argc, Value* args) {
  (void)vm;
  (void)argc;
  (void)args;
  double seconds = (double)clock() / (double)CLOCKS_PER_SEC;
  return NUMBER_VAL(seconds);
}

static Value nativeType(VM* vm, int argc, Value* args) {
  (void)argc;
  const char* name = valueTypeName(args[0]);
  return OBJ_VAL(copyString(vm, name));
}

static Value nativeLen(VM* vm, int argc, Value* args) {
  (void)argc;
  if (isObjType(args[0], OBJ_STRING)) {
    ObjString* string = (ObjString*)AS_OBJ(args[0]);
    return NUMBER_VAL(string->length);
  }
  if (isObjType(args[0], OBJ_ARRAY)) {
    ObjArray* array = (ObjArray*)AS_OBJ(args[0]);
    return NUMBER_VAL(array->count);
  }
  if (isObjType(args[0], OBJ_MAP)) {
    ObjMap* map = (ObjMap*)AS_OBJ(args[0]);
    return NUMBER_VAL(mapCount(map));
  }
  return runtimeErrorValue(vm, "len() expects a string, array, or map.");
}

static Value nativeArgs(VM* vm, int argc, Value* args) {
  (void)argc;
  (void)args;
  return OBJ_VAL(vm->args);
}

void defineStdlib(VM* vm) {
  defineNative(vm, "print", nativePrint, -1);
  defineNative(vm, "clock", nativeClock, 0);
  defineNative(vm, "type", nativeType, 1);
  defineNative(vm, "len", nativeLen, 1);
  defineNative(vm, "args", nativeArgs, 0);
}

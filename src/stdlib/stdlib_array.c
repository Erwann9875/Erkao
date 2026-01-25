#include "stdlib_internal.h"

static Value nativeArraySlice(VM* vm, int argc, Value* args) {
  if (!isObjType(args[0], OBJ_ARRAY)) {
    return runtimeErrorValue(vm, "array.slice expects an array.");
  }
  ObjArray* array = (ObjArray*)AS_OBJ(args[0]);
  int count = array->count;
  int start = 0;
  int end = count;
  if (argc >= 2) {
    if (!IS_NUMBER(args[1])) {
      return runtimeErrorValue(vm, "array.slice expects numeric indices.");
    }
    start = (int)AS_NUMBER(args[1]);
  }
  if (argc >= 3) {
    if (!IS_NUMBER(args[2])) {
      return runtimeErrorValue(vm, "array.slice expects numeric indices.");
    }
    end = (int)AS_NUMBER(args[2]);
  }
  if (start < 0) start = count + start;
  if (end < 0) end = count + end;
  if (start < 0) start = 0;
  if (end < 0) end = 0;
  if (start > count) start = count;
  if (end > count) end = count;
  if (end < start) end = start;

  ObjArray* result = newArrayWithCapacity(vm, end - start);
  for (int i = start; i < end; i++) {
    arrayWrite(result, array->items[i]);
  }
  return OBJ_VAL(result);
}

static Value nativeArrayMap(VM* vm, int argc, Value* args) {
  (void)argc;
  if (!isObjType(args[0], OBJ_ARRAY)) {
    return runtimeErrorValue(vm, "array.map expects (array, fn).");
  }
  ObjArray* array = (ObjArray*)AS_OBJ(args[0]);
  Value fn = args[1];
  ObjArray* result = newArrayWithCapacity(vm, array->count);
  for (int i = 0; i < array->count; i++) {
    Value arg = array->items[i];
    Value out;
    if (!vmCallValue(vm, fn, 1, &arg, &out)) {
      return NULL_VAL;
    }
    arrayWrite(result, out);
  }
  return OBJ_VAL(result);
}

static Value nativeArrayFilter(VM* vm, int argc, Value* args) {
  (void)argc;
  if (!isObjType(args[0], OBJ_ARRAY)) {
    return runtimeErrorValue(vm, "array.filter expects (array, fn).");
  }
  ObjArray* array = (ObjArray*)AS_OBJ(args[0]);
  Value fn = args[1];
  ObjArray* result = newArrayWithCapacity(vm, array->count);
  for (int i = 0; i < array->count; i++) {
    Value arg = array->items[i];
    Value out;
    if (!vmCallValue(vm, fn, 1, &arg, &out)) {
      return NULL_VAL;
    }
    if (isTruthy(out)) {
      arrayWrite(result, arg);
    }
  }
  return OBJ_VAL(result);
}

static Value nativeArrayReduce(VM* vm, int argc, Value* args) {
  if (argc < 2) {
    return runtimeErrorValue(vm, "array.reduce expects (array, fn, initial?).");
  }
  if (!isObjType(args[0], OBJ_ARRAY)) {
    return runtimeErrorValue(vm, "array.reduce expects (array, fn, initial?).");
  }
  ObjArray* array = (ObjArray*)AS_OBJ(args[0]);
  Value fn = args[1];
  int index = 0;
  Value acc = NULL_VAL;
  if (argc >= 3) {
    acc = args[2];
  } else {
    if (array->count == 0) {
      return runtimeErrorValue(vm, "array.reduce expects an initial value for empty arrays.");
    }
    acc = array->items[0];
    index = 1;
  }

  for (int i = index; i < array->count; i++) {
    Value callArgs[2] = {acc, array->items[i]};
    Value out;
    if (!vmCallValue(vm, fn, 2, callArgs, &out)) {
      return NULL_VAL;
    }
    acc = out;
  }

  return acc;
}

static Value nativeArrayContains(VM* vm, int argc, Value* args) {
  (void)argc;
  if (!isObjType(args[0], OBJ_ARRAY)) {
    return runtimeErrorValue(vm, "array.contains expects (array, value).");
  }
  ObjArray* array = (ObjArray*)AS_OBJ(args[0]);
  for (int i = 0; i < array->count; i++) {
    if (valuesEqual(array->items[i], args[1])) {
      return BOOL_VAL(true);
    }
  }
  return BOOL_VAL(false);
}

static Value nativeArrayIndexOf(VM* vm, int argc, Value* args) {
  (void)argc;
  if (!isObjType(args[0], OBJ_ARRAY)) {
    return runtimeErrorValue(vm, "array.indexOf expects (array, value).");
  }
  ObjArray* array = (ObjArray*)AS_OBJ(args[0]);
  for (int i = 0; i < array->count; i++) {
    if (valuesEqual(array->items[i], args[1])) {
      return NUMBER_VAL((double)i);
    }
  }
  return NUMBER_VAL(-1);
}

static Value nativeArrayConcat(VM* vm, int argc, Value* args) {
  (void)argc;
  if (!isObjType(args[0], OBJ_ARRAY) || !isObjType(args[1], OBJ_ARRAY)) {
    return runtimeErrorValue(vm, "array.concat expects (left, right) arrays.");
  }
  ObjArray* left = (ObjArray*)AS_OBJ(args[0]);
  ObjArray* right = (ObjArray*)AS_OBJ(args[1]);
  ObjArray* result = newArrayWithCapacity(vm, left->count + right->count);
  for (int i = 0; i < left->count; i++) {
    arrayWrite(result, left->items[i]);
  }
  for (int i = 0; i < right->count; i++) {
    arrayWrite(result, right->items[i]);
  }
  return OBJ_VAL(result);
}

static Value nativeArrayReverse(VM* vm, int argc, Value* args) {
  (void)argc;
  if (!isObjType(args[0], OBJ_ARRAY)) {
    return runtimeErrorValue(vm, "array.reverse expects an array.");
  }
  ObjArray* array = (ObjArray*)AS_OBJ(args[0]);
  ObjArray* result = newArrayWithCapacity(vm, array->count);
  for (int i = array->count - 1; i >= 0; i--) {
    arrayWrite(result, array->items[i]);
  }
  return OBJ_VAL(result);
}


void stdlib_register_array(VM* vm, ObjInstance* module) {
  moduleAdd(vm, module, "slice", nativeArraySlice, -1);
  moduleAdd(vm, module, "map", nativeArrayMap, 2);
  moduleAdd(vm, module, "filter", nativeArrayFilter, 2);
  moduleAdd(vm, module, "reduce", nativeArrayReduce, -1);
  moduleAdd(vm, module, "contains", nativeArrayContains, 2);
  moduleAdd(vm, module, "indexOf", nativeArrayIndexOf, 2);
  moduleAdd(vm, module, "concat", nativeArrayConcat, 2);
  moduleAdd(vm, module, "reverse", nativeArrayReverse, 1);
}

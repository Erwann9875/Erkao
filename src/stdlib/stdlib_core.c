#include "stdlib_internal.h"

static Value nativePrint(VM* vm, int argc, Value* args) {
  (void)vm;
  for (int i = 0; i < argc; i++) {
    if (i > 0) printf(" ");
    printValue(args[i]);
  }
  printf("\n");
  return NULL_VAL;
}

static Value nativeFmt(VM* vm, int argc, Value* args) {
  if (argc < 1 || !isObjType(args[0], OBJ_STRING)) {
    return runtimeErrorValue(vm, "fmt expects a format string.");
  }
  ObjString* format = (ObjString*)AS_OBJ(args[0]);
  ByteBuffer buffer;
  bufferInit(&buffer);
  int argIndex = 1;

  for (int i = 0; i < format->length; i++) {
    char c = format->chars[i];
    if (c == '{') {
      if (i + 1 < format->length && format->chars[i + 1] == '{') {
        bufferAppendChar(&buffer, '{');
        i++;
        continue;
      }
      if (i + 1 < format->length && format->chars[i + 1] == '}') {
        if (argIndex >= argc) {
          bufferFree(&buffer);
          return runtimeErrorValue(vm, "fmt expects a value for '{}'.");
        }
        ObjString* text = stringifyValue(vm, args[argIndex++]);
        bufferAppendN(&buffer, text->chars, (size_t)text->length);
        i++;
        continue;
      }
      bufferFree(&buffer);
      return runtimeErrorValue(vm, "fmt expects '{}' or '{{'.");
    }
    if (c == '}') {
      if (i + 1 < format->length && format->chars[i + 1] == '}') {
        bufferAppendChar(&buffer, '}');
        i++;
        continue;
      }
      bufferFree(&buffer);
      return runtimeErrorValue(vm, "fmt expects '}' to be escaped as '}}'.");
    }
    bufferAppendChar(&buffer, c);
  }

  ObjString* result = copyStringWithLength(vm, buffer.data ? buffer.data : "",
                                           (int)buffer.length);
  bufferFree(&buffer);
  return OBJ_VAL(result);
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

static Value nativePush(VM* vm, int argc, Value* args) {
  (void)argc;
  if (!isObjType(args[0], OBJ_ARRAY)) {
    return runtimeErrorValue(vm, "push() expects an array as the first argument.");
  }
  ObjArray* array = (ObjArray*)AS_OBJ(args[0]);
  arrayWrite(array, args[1]);
  return NUMBER_VAL(array->count);
}

static Value nativeKeys(VM* vm, int argc, Value* args) {
  (void)argc;
  if (!isObjType(args[0], OBJ_MAP)) {
    return runtimeErrorValue(vm, "keys() expects a map.");
  }
  ObjMap* map = (ObjMap*)AS_OBJ(args[0]);
  ObjArray* array = newArrayWithCapacity(vm, map->count);
  for (int i = 0; i < map->capacity; i++) {
    if (!map->entries[i].key) continue;
    arrayWrite(array, OBJ_VAL(map->entries[i].key));
  }
  return OBJ_VAL(array);
}

static Value nativeValues(VM* vm, int argc, Value* args) {
  (void)argc;
  if (!isObjType(args[0], OBJ_MAP)) {
    return runtimeErrorValue(vm, "values() expects a map.");
  }
  ObjMap* map = (ObjMap*)AS_OBJ(args[0]);
  ObjArray* array = newArrayWithCapacity(vm, map->count);
  for (int i = 0; i < map->capacity; i++) {
    if (!map->entries[i].key) continue;
    arrayWrite(array, map->entries[i].value);
  }
  return OBJ_VAL(array);
}

static bool stringEquals(ObjString* str, const char* text) {
  if (!str || !text) return false;
  size_t len = strlen(text);
  if ((size_t)str->length != len) return false;
  return memcmp(str->chars, text, len) == 0;
}

static void mapSetField(VM* vm, ObjMap* map, const char* name, Value value) {
  ObjString* key = copyString(vm, name);
  mapSet(map, key, value);
}

static bool mapGetField(VM* vm, ObjMap* map, const char* name, Value* out) {
  ObjString* key = copyString(vm, name);
  return mapGet(map, key, out);
}

static ObjArray* mapKeysArray(VM* vm, ObjMap* map) {
  ObjArray* array = newArrayWithCapacity(vm, map->count);
  for (int i = 0; i < map->capacity; i++) {
    if (!map->entries[i].key) continue;
    arrayWrite(array, OBJ_VAL(map->entries[i].key));
  }
  return array;
}

static Value makeIterResult(VM* vm, bool done, Value key, Value value) {
  ObjMap* result = newMap(vm);
  mapSetField(vm, result, "done", BOOL_VAL(done));
  if (!done) {
    mapSetField(vm, result, "value", value);
    mapSetField(vm, result, "key", key);
  }
  return OBJ_VAL(result);
}

static bool instanceGetCallable(VM* vm, ObjInstance* instance, const char* name, Value* out) {
  ObjString* key = copyString(vm, name);
  if (mapGet(instance->fields, key, out)) {
    return true;
  }
  Value methodValue;
  if (mapGet(instance->klass->methods, key, &methodValue)) {
    if (isObjType(methodValue, OBJ_FUNCTION)) {
      ObjBoundMethod* bound = newBoundMethod(vm, OBJ_VAL(instance),
                                             (ObjFunction*)AS_OBJ(methodValue));
      *out = OBJ_VAL(bound);
    } else {
      *out = methodValue;
    }
    return true;
  }
  return false;
}

static Value nativeRange(VM* vm, int argc, Value* args) {
  (void)argc;
  if (!IS_NUMBER(args[0]) || !IS_NUMBER(args[1])) {
    return runtimeErrorValue(vm, "range() expects (start, end) numbers.");
  }
  double start = AS_NUMBER(args[0]);
  double end = AS_NUMBER(args[1]);
  double step = start <= end ? 1.0 : -1.0;

  ObjMap* range = newMap(vm);
  mapSetField(vm, range, "_iter_type", OBJ_VAL(copyString(vm, "range")));
  mapSetField(vm, range, "current", NUMBER_VAL(start));
  mapSetField(vm, range, "end", NUMBER_VAL(end));
  mapSetField(vm, range, "step", NUMBER_VAL(step));
  return OBJ_VAL(range);
}

static Value nativeIter(VM* vm, int argc, Value* args) {
  (void)argc;
  Value target = args[0];
  if (isObjType(target, OBJ_ARRAY)) {
    ObjMap* iter = newMap(vm);
    mapSetField(vm, iter, "_iter_type", OBJ_VAL(copyString(vm, "array")));
    mapSetField(vm, iter, "_array", target);
    mapSetField(vm, iter, "_index", NUMBER_VAL(0));
    return OBJ_VAL(iter);
  }
  if (isObjType(target, OBJ_MAP)) {
    ObjMap* map = (ObjMap*)AS_OBJ(target);
    Value iterType;
    if (mapGetField(vm, map, "_iter_type", &iterType)) {
      return target;
    }
    Value iterFn;
    if (mapGetField(vm, map, "iter", &iterFn)) {
      Value result;
      if (!vmCallValue(vm, iterFn, 0, NULL, &result)) return NULL_VAL;
      return result;
    }
    ObjMap* iter = newMap(vm);
    ObjArray* keys = mapKeysArray(vm, map);
    mapSetField(vm, iter, "_iter_type", OBJ_VAL(copyString(vm, "map")));
    mapSetField(vm, iter, "_map", target);
    mapSetField(vm, iter, "_keys", OBJ_VAL(keys));
    mapSetField(vm, iter, "_index", NUMBER_VAL(0));
    return OBJ_VAL(iter);
  }
  if (isObjType(target, OBJ_INSTANCE)) {
    Value iterFn;
    if (instanceGetCallable(vm, (ObjInstance*)AS_OBJ(target), "iter", &iterFn)) {
      Value result;
      if (!vmCallValue(vm, iterFn, 0, NULL, &result)) return NULL_VAL;
      return result;
    }
  }
  return runtimeErrorValue(vm, "iter() expects an array, map, or iterable.");
}

static Value nativeNext(VM* vm, int argc, Value* args) {
  (void)argc;
  Value target = args[0];
  if (isObjType(target, OBJ_MAP)) {
    ObjMap* map = (ObjMap*)AS_OBJ(target);
    Value iterType;
    if (mapGetField(vm, map, "_iter_type", &iterType) && isString(iterType)) {
      ObjString* type = asString(iterType);
      if (stringEquals(type, "array")) {
        Value arrayValue;
        Value indexValue;
        if (!mapGetField(vm, map, "_array", &arrayValue) ||
            !mapGetField(vm, map, "_index", &indexValue) ||
            !isObjType(arrayValue, OBJ_ARRAY) || !IS_NUMBER(indexValue)) {
          return runtimeErrorValue(vm, "next() invalid array iterator.");
        }
        ObjArray* array = (ObjArray*)AS_OBJ(arrayValue);
        int index = (int)AS_NUMBER(indexValue);
        if (index >= array->count) {
          return makeIterResult(vm, true, NULL_VAL, NULL_VAL);
        }
        Value value = array->items[index];
        mapSetField(vm, map, "_index", NUMBER_VAL(index + 1));
        return makeIterResult(vm, false, NUMBER_VAL(index), value);
      }
      if (stringEquals(type, "map")) {
        Value mapValue;
        Value keysValue;
        Value indexValue;
        if (!mapGetField(vm, map, "_map", &mapValue) ||
            !mapGetField(vm, map, "_keys", &keysValue) ||
            !mapGetField(vm, map, "_index", &indexValue) ||
            !isObjType(mapValue, OBJ_MAP) ||
            !isObjType(keysValue, OBJ_ARRAY) ||
            !IS_NUMBER(indexValue)) {
          return runtimeErrorValue(vm, "next() invalid map iterator.");
        }
        ObjMap* source = (ObjMap*)AS_OBJ(mapValue);
        ObjArray* keys = (ObjArray*)AS_OBJ(keysValue);
        int index = (int)AS_NUMBER(indexValue);
        if (index >= keys->count) {
          return makeIterResult(vm, true, NULL_VAL, NULL_VAL);
        }
        Value key = keys->items[index];
        Value value = NULL_VAL;
        if (isString(key)) {
          mapGet(source, asString(key), &value);
        }
        mapSetField(vm, map, "_index", NUMBER_VAL(index + 1));
        return makeIterResult(vm, false, key, value);
      }
      if (stringEquals(type, "range")) {
        Value currentValue;
        Value endValue;
        Value stepValue;
        if (!mapGetField(vm, map, "current", &currentValue) ||
            !mapGetField(vm, map, "end", &endValue) ||
            !mapGetField(vm, map, "step", &stepValue) ||
            !IS_NUMBER(currentValue) || !IS_NUMBER(endValue) || !IS_NUMBER(stepValue)) {
          return runtimeErrorValue(vm, "next() invalid range iterator.");
        }
        double current = AS_NUMBER(currentValue);
        double end = AS_NUMBER(endValue);
        double step = AS_NUMBER(stepValue);
        if (step == 0) {
          return makeIterResult(vm, true, NULL_VAL, NULL_VAL);
        }
        if ((step > 0 && current > end) || (step < 0 && current < end)) {
          return makeIterResult(vm, true, NULL_VAL, NULL_VAL);
        }
        mapSetField(vm, map, "current", NUMBER_VAL(current + step));
        return makeIterResult(vm, false, NUMBER_VAL(current), NUMBER_VAL(current));
      }
    }

    Value nextFn;
    if (mapGetField(vm, map, "next", &nextFn)) {
      Value result;
      if (!vmCallValue(vm, nextFn, 0, NULL, &result)) return NULL_VAL;
      return result;
    }
  }
  if (isObjType(target, OBJ_INSTANCE)) {
    Value nextFn;
    if (instanceGetCallable(vm, (ObjInstance*)AS_OBJ(target), "next", &nextFn)) {
      Value result;
      if (!vmCallValue(vm, nextFn, 0, NULL, &result)) return NULL_VAL;
      return result;
    }
  }
  return runtimeErrorValue(vm, "next() expects an iterator.");
}

static Value nativeArrayRest(VM* vm, int argc, Value* args) {
  (void)argc;
  if (!isObjType(args[0], OBJ_ARRAY) || !IS_NUMBER(args[1])) {
    return runtimeErrorValue(vm, "arrayRest() expects (array, start).");
  }
  ObjArray* array = (ObjArray*)AS_OBJ(args[0]);
  int count = array->count;
  int start = (int)AS_NUMBER(args[1]);
  if (start < 0) start = count + start;
  if (start < 0) start = 0;
  if (start > count) start = count;
  ObjArray* result = newArrayWithCapacity(vm, count - start);
  for (int i = start; i < count; i++) {
    arrayWrite(result, array->items[i]);
  }
  return OBJ_VAL(result);
}

static Value nativeMapRest(VM* vm, int argc, Value* args) {
  (void)argc;
  if (!isObjType(args[0], OBJ_MAP) || !isObjType(args[1], OBJ_ARRAY)) {
    return runtimeErrorValue(vm, "mapRest() expects (map, keys).");
  }
  ObjMap* map = (ObjMap*)AS_OBJ(args[0]);
  ObjArray* keys = (ObjArray*)AS_OBJ(args[1]);
  ObjMap* result = newMapWithCapacity(vm, map->count);
  for (int i = 0; i < map->capacity; i++) {
    if (!map->entries[i].key) continue;
    Value keyValue = OBJ_VAL(map->entries[i].key);
    bool excluded = false;
    for (int j = 0; j < keys->count; j++) {
      if (!isString(keys->items[j])) continue;
      if (valuesEqual(keyValue, keys->items[j])) {
        excluded = true;
        break;
      }
    }
    if (excluded) continue;
    mapSet(result, map->entries[i].key, map->entries[i].value);
  }
  return OBJ_VAL(result);
}

static Value nativeSpawn(VM* vm, int argc, Value* args) {
  if (argc < 1) {
    return runtimeErrorValue(vm, "spawn() expects a function.");
  }
  Value result;
  if (!vmCallValue(vm, args[0], argc - 1, argc > 1 ? &args[1] : NULL, &result)) {
    return NULL_VAL;
  }
  ObjMap* task = newMap(vm);
  mapSetField(vm, task, "done", BOOL_VAL(true));
  mapSetField(vm, task, "value", result);
  return OBJ_VAL(task);
}

static Value nativeAwait(VM* vm, int argc, Value* args) {
  (void)argc;
  if (!isObjType(args[0], OBJ_MAP)) {
    return runtimeErrorValue(vm, "await() expects a task.");
  }
  ObjMap* task = (ObjMap*)AS_OBJ(args[0]);
  Value value;
  if (mapGetField(vm, task, "value", &value)) {
    return value;
  }
  return NULL_VAL;
}

static Value nativeChannel(VM* vm, int argc, Value* args) {
  (void)argc;
  (void)args;
  ObjMap* channel = newMap(vm);
  ObjArray* queue = newArray(vm);
  mapSetField(vm, channel, "_queue", OBJ_VAL(queue));
  return OBJ_VAL(channel);
}

static Value nativeSend(VM* vm, int argc, Value* args) {
  (void)argc;
  if (!isObjType(args[0], OBJ_MAP)) {
    return runtimeErrorValue(vm, "send() expects a channel.");
  }
  ObjMap* channel = (ObjMap*)AS_OBJ(args[0]);
  Value queueValue;
  if (!mapGetField(vm, channel, "_queue", &queueValue) ||
      !isObjType(queueValue, OBJ_ARRAY)) {
    return runtimeErrorValue(vm, "send() expects a channel queue.");
  }
  ObjArray* queue = (ObjArray*)AS_OBJ(queueValue);
  arrayWrite(queue, args[1]);
  return NULL_VAL;
}

static Value nativeRecv(VM* vm, int argc, Value* args) {
  (void)argc;
  if (!isObjType(args[0], OBJ_MAP)) {
    return runtimeErrorValue(vm, "recv() expects a channel.");
  }
  ObjMap* channel = (ObjMap*)AS_OBJ(args[0]);
  Value queueValue;
  if (!mapGetField(vm, channel, "_queue", &queueValue) ||
      !isObjType(queueValue, OBJ_ARRAY)) {
    return runtimeErrorValue(vm, "recv() expects a channel queue.");
  }
  ObjArray* queue = (ObjArray*)AS_OBJ(queueValue);
  if (queue->count <= 0) return NULL_VAL;
  Value result = queue->items[0];
  for (int i = 1; i < queue->count; i++) {
    queue->items[i - 1] = queue->items[i];
  }
  queue->count--;
  return result;
}

static Value nativeSleep(VM* vm, int argc, Value* args) {
  (void)argc;
  if (!IS_NUMBER(args[0])) {
    return runtimeErrorValue(vm, "sleep() expects seconds as a number.");
  }
  double seconds = AS_NUMBER(args[0]);
  if (seconds < 0) {
    return runtimeErrorValue(vm, "sleep() expects a non-negative number.");
  }
#ifdef _WIN32
  DWORD ms = (DWORD)(seconds * 1000.0);
  Sleep(ms);
#else
  struct timespec ts;
  ts.tv_sec = (time_t)seconds;
  ts.tv_nsec = (long)((seconds - (double)ts.tv_sec) * 1000000000.0);
  if (ts.tv_nsec < 0) ts.tv_nsec = 0;
  if (nanosleep(&ts, NULL) != 0) {
    return runtimeErrorValue(vm, "sleep() failed.");
  }
#endif
  return NULL_VAL;
}


void stdlib_register_globals(VM* vm) {
  defineNative(vm, "print", nativePrint, -1);
  defineNative(vm, "fmt", nativeFmt, -1);
  defineNative(vm, "clock", nativeClock, 0);
  defineNative(vm, "type", nativeType, 1);
  defineNative(vm, "len", nativeLen, 1);
  defineNative(vm, "args", nativeArgs, 0);
  defineNative(vm, "push", nativePush, 2);
  defineNative(vm, "keys", nativeKeys, 1);
  defineNative(vm, "values", nativeValues, 1);
  defineNative(vm, "range", nativeRange, 2);
  defineNative(vm, "iter", nativeIter, 1);
  defineNative(vm, "next", nativeNext, 1);
  defineNative(vm, "arrayRest", nativeArrayRest, 2);
  defineNative(vm, "mapRest", nativeMapRest, 2);
  defineNative(vm, "spawn", nativeSpawn, -1);
  defineNative(vm, "await", nativeAwait, 1);
  defineNative(vm, "channel", nativeChannel, 0);
  defineNative(vm, "send", nativeSend, 2);
  defineNative(vm, "recv", nativeRecv, 1);
  defineNative(vm, "sleep", nativeSleep, 1);

  ObjMap* option = newMap(vm);
  ObjString* optionName = copyString(vm, "Option");
  ObjString* someKey = copyString(vm, "Some");
  ObjString* noneKey = copyString(vm, "None");
  ObjEnumCtor* someCtor = newEnumCtor(vm, optionName, someKey, 1);
  ObjMap* noneValue = newEnumVariant(vm, optionName, noneKey, 0, NULL);
  mapSet(option, someKey, OBJ_VAL(someCtor));
  mapSet(option, noneKey, OBJ_VAL(noneValue));
  defineGlobal(vm, "Option", OBJ_VAL(option));

  ObjMap* result = newMap(vm);
  ObjString* resultName = copyString(vm, "Result");
  ObjString* okKey = copyString(vm, "Ok");
  ObjString* errKey = copyString(vm, "Err");
  ObjEnumCtor* okCtor = newEnumCtor(vm, resultName, okKey, 1);
  ObjEnumCtor* errCtor = newEnumCtor(vm, resultName, errKey, 1);
  mapSet(result, okKey, OBJ_VAL(okCtor));
  mapSet(result, errKey, OBJ_VAL(errCtor));
  defineGlobal(vm, "Result", OBJ_VAL(result));
}

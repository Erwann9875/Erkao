#include "stdlib.h"
#include "plugin.h"

#ifdef _WIN32
#include <windows.h>
#endif

static Value runtimeErrorValue(VM* vm, const char* message) {
  fprintf(stderr, "RuntimeError: %s\n", message);
  vm->hadError = true;
  return NULL_VAL;
}

static ObjInstance* makeModule(VM* vm, const char* name) {
  ObjString* className = copyString(vm, name);
  ObjMap* methods = newMap(vm);
  ObjClass* klass = newClass(vm, className, methods);
  return newInstance(vm, klass);
}

static void moduleAdd(VM* vm, ObjInstance* module, const char* name, NativeFn fn, int arity) {
  ObjString* fieldName = copyString(vm, name);
  ObjNative* native = newNative(vm, fn, arity, fieldName);
  mapSet(module->fields, fieldName, OBJ_VAL(native));
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
  ObjArray* array = newArray(vm);
  for (int i = 0; i < map->count; i++) {
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
  ObjArray* array = newArray(vm);
  for (int i = 0; i < map->count; i++) {
    arrayWrite(array, map->entries[i].value);
  }
  return OBJ_VAL(array);
}

static Value nativeFsReadText(VM* vm, int argc, Value* args) {
  (void)argc;
  if (!isObjType(args[0], OBJ_STRING)) {
    return runtimeErrorValue(vm, "fs.readText expects a path string.");
  }
  ObjString* path = (ObjString*)AS_OBJ(args[0]);
  FILE* file = fopen(path->chars, "rb");
  if (!file) {
    return runtimeErrorValue(vm, "fs.readText failed to open file.");
  }

  fseek(file, 0L, SEEK_END);
  long size = ftell(file);
  rewind(file);
  if (size < 0) {
    fclose(file);
    return runtimeErrorValue(vm, "fs.readText failed to read file size.");
  }

  char* buffer = (char*)malloc((size_t)size + 1);
  if (!buffer) {
    fclose(file);
    return runtimeErrorValue(vm, "fs.readText out of memory.");
  }

  size_t read = fread(buffer, 1, (size_t)size, file);
  buffer[read] = '\0';
  fclose(file);

  ObjString* result = copyStringWithLength(vm, buffer, (int)read);
  free(buffer);
  return OBJ_VAL(result);
}

static Value nativeFsWriteText(VM* vm, int argc, Value* args) {
  (void)argc;
  if (!isObjType(args[0], OBJ_STRING) || !isObjType(args[1], OBJ_STRING)) {
    return runtimeErrorValue(vm, "fs.writeText expects (path, text) strings.");
  }
  ObjString* path = (ObjString*)AS_OBJ(args[0]);
  ObjString* text = (ObjString*)AS_OBJ(args[1]);

  FILE* file = fopen(path->chars, "wb");
  if (!file) {
    return runtimeErrorValue(vm, "fs.writeText failed to open file.");
  }

  size_t written = fwrite(text->chars, 1, (size_t)text->length, file);
  fclose(file);
  if (written != (size_t)text->length) {
    return runtimeErrorValue(vm, "fs.writeText failed to write file.");
  }
  return BOOL_VAL(true);
}

static Value nativeFsListDir(VM* vm, int argc, Value* args) {
  (void)argc;
  if (!isObjType(args[0], OBJ_STRING)) {
    return runtimeErrorValue(vm, "fs.listDir expects a path string.");
  }
  ObjString* path = (ObjString*)AS_OBJ(args[0]);

#ifdef _WIN32
  size_t pathLength = strlen(path->chars);
  size_t patternLength = pathLength + 3;
  char* pattern = (char*)malloc(patternLength);
  if (!pattern) {
    return runtimeErrorValue(vm, "fs.listDir out of memory.");
  }
  snprintf(pattern, patternLength, "%s\\*", path->chars);

  WIN32_FIND_DATAA data;
  HANDLE handle = FindFirstFileA(pattern, &data);
  free(pattern);
  if (handle == INVALID_HANDLE_VALUE) {
    return runtimeErrorValue(vm, "fs.listDir failed to open directory.");
  }

  ObjArray* array = newArray(vm);
  do {
    if (strcmp(data.cFileName, ".") == 0 || strcmp(data.cFileName, "..") == 0) {
      continue;
    }
    arrayWrite(array, OBJ_VAL(copyString(vm, data.cFileName)));
  } while (FindNextFileA(handle, &data));

  FindClose(handle);
  return OBJ_VAL(array);
#else
  (void)path;
  return runtimeErrorValue(vm, "fs.listDir is only supported on Windows.");
#endif
}

static Value nativeProcRun(VM* vm, int argc, Value* args) {
  (void)argc;
  if (!isObjType(args[0], OBJ_STRING)) {
    return runtimeErrorValue(vm, "proc.run expects a command string.");
  }
  ObjString* cmd = (ObjString*)AS_OBJ(args[0]);
  int result = system(cmd->chars);
  return NUMBER_VAL((double)result);
}

static Value nativeEnvGet(VM* vm, int argc, Value* args) {
  (void)argc;
  if (!isObjType(args[0], OBJ_STRING)) {
    return runtimeErrorValue(vm, "env.get expects a name string.");
  }
  ObjString* name = (ObjString*)AS_OBJ(args[0]);
  const char* value = getenv(name->chars);
  if (!value) return NULL_VAL;
  return OBJ_VAL(copyString(vm, value));
}

static Value nativeEnvArgs(VM* vm, int argc, Value* args) {
  (void)argc;
  (void)args;
  return OBJ_VAL(vm->args);
}

static Value nativePluginLoad(VM* vm, int argc, Value* args) {
  (void)argc;
  if (!isObjType(args[0], OBJ_STRING)) {
    return runtimeErrorValue(vm, "plugin.load expects a path string.");
  }
  ObjString* path = (ObjString*)AS_OBJ(args[0]);
  char error[256];
  if (!pluginLoad(vm, path->chars, error, sizeof(error))) {
    return runtimeErrorValue(vm, error);
  }
  return BOOL_VAL(true);
}

void defineStdlib(VM* vm) {
  defineNative(vm, "print", nativePrint, -1);
  defineNative(vm, "clock", nativeClock, 0);
  defineNative(vm, "type", nativeType, 1);
  defineNative(vm, "len", nativeLen, 1);
  defineNative(vm, "args", nativeArgs, 0);
  defineNative(vm, "push", nativePush, 2);
  defineNative(vm, "keys", nativeKeys, 1);
  defineNative(vm, "values", nativeValues, 1);

  ObjInstance* fs = makeModule(vm, "fs");
  moduleAdd(vm, fs, "readText", nativeFsReadText, 1);
  moduleAdd(vm, fs, "writeText", nativeFsWriteText, 2);
  moduleAdd(vm, fs, "listDir", nativeFsListDir, 1);
  defineGlobal(vm, "fs", OBJ_VAL(fs));

  ObjInstance* proc = makeModule(vm, "proc");
  moduleAdd(vm, proc, "run", nativeProcRun, 1);
  defineGlobal(vm, "proc", OBJ_VAL(proc));

  ObjInstance* env = makeModule(vm, "env");
  moduleAdd(vm, env, "args", nativeEnvArgs, 0);
  moduleAdd(vm, env, "get", nativeEnvGet, 1);
  defineGlobal(vm, "env", OBJ_VAL(env));

  ObjInstance* plugin = makeModule(vm, "plugin");
  moduleAdd(vm, plugin, "load", nativePluginLoad, 1);
  defineGlobal(vm, "plugin", OBJ_VAL(plugin));
}

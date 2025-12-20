#include "erkao_stdlib.h"
#include "interpreter_internal.h"
#include "plugin.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

static Value runtimeErrorValue(VM* vm, const char* message) {
  Token token;
  memset(&token, 0, sizeof(Token));
  runtimeError(vm, token, message);
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

static const char* findLastSeparator(const char* path) {
  const char* lastSlash = strrchr(path, '/');
  const char* lastBackslash = strrchr(path, '\\');
  if (!lastSlash) return lastBackslash;
  if (!lastBackslash) return lastSlash;
  return lastSlash > lastBackslash ? lastSlash : lastBackslash;
}

static bool isAbsolutePathString(const char* path) {
  if (!path || path[0] == '\0') return false;
  if (path[0] == '/' || path[0] == '\\') return true;
  if (((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z')) &&
      path[1] == ':' && (path[2] == '\\' || path[2] == '/')) {
    return true;
  }
  return false;
}

static char pickSeparator(const char* left, const char* right) {
  if ((left && strchr(left, '\\')) || (right && strchr(right, '\\'))) return '\\';
  return '/';
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

static Value nativeFsExists(VM* vm, int argc, Value* args) {
  (void)argc;
  if (!isObjType(args[0], OBJ_STRING)) {
    return runtimeErrorValue(vm, "fs.exists expects a path string.");
  }
  ObjString* path = (ObjString*)AS_OBJ(args[0]);
#ifdef _WIN32
  DWORD attrs = GetFileAttributesA(path->chars);
  return BOOL_VAL(attrs != INVALID_FILE_ATTRIBUTES);
#else
  struct stat st;
  return BOOL_VAL(stat(path->chars, &st) == 0);
#endif
}

static Value nativeFsCwd(VM* vm, int argc, Value* args) {
  (void)argc;
  (void)args;
#ifdef _WIN32
  DWORD length = GetCurrentDirectoryA(0, NULL);
  if (length == 0) {
    return runtimeErrorValue(vm, "fs.cwd failed to read current directory.");
  }
  char* buffer = (char*)malloc((size_t)length);
  if (!buffer) {
    return runtimeErrorValue(vm, "fs.cwd out of memory.");
  }
  if (GetCurrentDirectoryA(length, buffer) == 0) {
    free(buffer);
    return runtimeErrorValue(vm, "fs.cwd failed to read current directory.");
  }
  ObjString* result = copyString(vm, buffer);
  free(buffer);
  return OBJ_VAL(result);
#else
  char* buffer = getcwd(NULL, 0);
  if (!buffer) {
    return runtimeErrorValue(vm, "fs.cwd failed to read current directory.");
  }
  ObjString* result = copyString(vm, buffer);
  free(buffer);
  return OBJ_VAL(result);
#endif
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

static Value nativePathJoin(VM* vm, int argc, Value* args) {
  (void)argc;
  if (!isObjType(args[0], OBJ_STRING) || !isObjType(args[1], OBJ_STRING)) {
    return runtimeErrorValue(vm, "path.join expects (left, right) strings.");
  }
  ObjString* left = (ObjString*)AS_OBJ(args[0]);
  ObjString* right = (ObjString*)AS_OBJ(args[1]);
  if (isAbsolutePathString(right->chars)) {
    return OBJ_VAL(copyStringWithLength(vm, right->chars, right->length));
  }

  char sep = pickSeparator(left->chars, right->chars);
  bool needSep = left->length > 0 &&
                 left->chars[left->length - 1] != '/' &&
                 left->chars[left->length - 1] != '\\';
  size_t total = (size_t)left->length + (needSep ? 1 : 0) + (size_t)right->length;
  char* buffer = (char*)malloc(total + 1);
  if (!buffer) {
    return runtimeErrorValue(vm, "path.join out of memory.");
  }
  memcpy(buffer, left->chars, (size_t)left->length);
  size_t offset = (size_t)left->length;
  if (needSep) {
    buffer[offset++] = sep;
  }
  memcpy(buffer + offset, right->chars, (size_t)right->length);
  buffer[total] = '\0';

  ObjString* result = copyStringWithLength(vm, buffer, (int)total);
  free(buffer);
  return OBJ_VAL(result);
}

static Value nativePathDirname(VM* vm, int argc, Value* args) {
  (void)argc;
  if (!isObjType(args[0], OBJ_STRING)) {
    return runtimeErrorValue(vm, "path.dirname expects a path string.");
  }
  ObjString* path = (ObjString*)AS_OBJ(args[0]);
  const char* sep = findLastSeparator(path->chars);
  if (!sep) {
    return OBJ_VAL(copyString(vm, "."));
  }

  size_t length = (size_t)(sep - path->chars);
  if (length == 0) {
    length = 1;
  } else if (length == 2 && path->chars[1] == ':' &&
             (path->chars[2] == '\\' || path->chars[2] == '/')) {
    length = 3;
  }

  if (length > (size_t)path->length) {
    length = (size_t)path->length;
  }

  return OBJ_VAL(copyStringWithLength(vm, path->chars, (int)length));
}

static Value nativePathBasename(VM* vm, int argc, Value* args) {
  (void)argc;
  if (!isObjType(args[0], OBJ_STRING)) {
    return runtimeErrorValue(vm, "path.basename expects a path string.");
  }
  ObjString* path = (ObjString*)AS_OBJ(args[0]);
  const char* sep = findLastSeparator(path->chars);
  const char* base = sep ? sep + 1 : path->chars;
  return OBJ_VAL(copyString(vm, base));
}

static Value nativePathExtname(VM* vm, int argc, Value* args) {
  (void)argc;
  if (!isObjType(args[0], OBJ_STRING)) {
    return runtimeErrorValue(vm, "path.extname expects a path string.");
  }
  ObjString* path = (ObjString*)AS_OBJ(args[0]);
  const char* sep = findLastSeparator(path->chars);
  const char* base = sep ? sep + 1 : path->chars;
  const char* dot = strrchr(base, '.');
  if (!dot || dot == base) {
    return OBJ_VAL(copyString(vm, ""));
  }
  return OBJ_VAL(copyStringWithLength(vm, dot, (int)strlen(dot)));
}

static Value nativeTimeNow(VM* vm, int argc, Value* args) {
  (void)argc;
  (void)args;
  time_t now = time(NULL);
  if (now == (time_t)-1) {
    return runtimeErrorValue(vm, "time.now failed.");
  }
  return NUMBER_VAL((double)now);
}

static Value nativeTimeSleep(VM* vm, int argc, Value* args) {
  (void)argc;
  if (!IS_NUMBER(args[0])) {
    return runtimeErrorValue(vm, "time.sleep expects seconds as a number.");
  }
  double seconds = AS_NUMBER(args[0]);
  if (seconds < 0) {
    return runtimeErrorValue(vm, "time.sleep expects a non-negative number.");
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
    return runtimeErrorValue(vm, "time.sleep failed.");
  }
#endif
  return NULL_VAL;
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
  moduleAdd(vm, fs, "exists", nativeFsExists, 1);
  moduleAdd(vm, fs, "cwd", nativeFsCwd, 0);
  moduleAdd(vm, fs, "listDir", nativeFsListDir, 1);
  defineGlobal(vm, "fs", OBJ_VAL(fs));

  ObjInstance* path = makeModule(vm, "path");
  moduleAdd(vm, path, "join", nativePathJoin, 2);
  moduleAdd(vm, path, "dirname", nativePathDirname, 1);
  moduleAdd(vm, path, "basename", nativePathBasename, 1);
  moduleAdd(vm, path, "extname", nativePathExtname, 1);
  defineGlobal(vm, "path", OBJ_VAL(path));

  ObjInstance* timeModule = makeModule(vm, "time");
  moduleAdd(vm, timeModule, "now", nativeTimeNow, 0);
  moduleAdd(vm, timeModule, "sleep", nativeTimeSleep, 1);
  defineGlobal(vm, "time", OBJ_VAL(timeModule));

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

#include "stdlib_internal.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

static Value nativeEnvGet(VM* vm, int argc, Value* args) {
  (void)argc;
  if (!isObjType(args[0], OBJ_STRING)) {
    return runtimeErrorValue(vm, "env.get expects a name string.");
  }
  ObjString* name = (ObjString*)AS_OBJ(args[0]);
#ifdef _WIN32
  DWORD length = GetEnvironmentVariableA(name->chars, NULL, 0);
  if (length == 0) {
    DWORD err = GetLastError();
    if (err == ERROR_ENVVAR_NOT_FOUND) {
      return NULL_VAL;
    }
    if (err == ERROR_SUCCESS) {
      return OBJ_VAL(copyString(vm, ""));
    }
    return runtimeErrorValue(vm, "env.get failed.");
  }
  char* buffer = (char*)malloc((size_t)length);
  if (!buffer) {
    return runtimeErrorValue(vm, "env.get out of memory.");
  }
  DWORD written = GetEnvironmentVariableA(name->chars, buffer, length);
  if (written == 0 && GetLastError() == ERROR_ENVVAR_NOT_FOUND) {
    free(buffer);
    return NULL_VAL;
  }
  ObjString* result = copyStringWithLength(vm, buffer, (int)written);
  free(buffer);
  return OBJ_VAL(result);
#else
  const char* value = getenv(name->chars);
  if (!value) return NULL_VAL;
  return OBJ_VAL(copyString(vm, value));
#endif
}

static Value nativeEnvSet(VM* vm, int argc, Value* args) {
  (void)argc;
  if (!isObjType(args[0], OBJ_STRING) || !isObjType(args[1], OBJ_STRING)) {
    return runtimeErrorValue(vm, "env.set expects (name, value) strings.");
  }
  ObjString* name = (ObjString*)AS_OBJ(args[0]);
  ObjString* value = (ObjString*)AS_OBJ(args[1]);
#ifdef _WIN32
  if (!SetEnvironmentVariableA(name->chars, value->chars)) {
    return runtimeErrorValue(vm, "env.set failed.");
  }
#else
  if (setenv(name->chars, value->chars, 1) != 0) {
    return runtimeErrorValue(vm, "env.set failed.");
  }
#endif
  return BOOL_VAL(true);
}

static Value nativeEnvHas(VM* vm, int argc, Value* args) {
  (void)argc;
  if (!isObjType(args[0], OBJ_STRING)) {
    return runtimeErrorValue(vm, "env.has expects a name string.");
  }
  ObjString* name = (ObjString*)AS_OBJ(args[0]);
#ifdef _WIN32
  DWORD length = GetEnvironmentVariableA(name->chars, NULL, 0);
  if (length == 0) {
    DWORD err = GetLastError();
    if (err == ERROR_ENVVAR_NOT_FOUND) return BOOL_VAL(false);
    if (err != ERROR_SUCCESS) return BOOL_VAL(false);
    return BOOL_VAL(true);
  }
  return BOOL_VAL(true);
#else
  return BOOL_VAL(getenv(name->chars) != NULL);
#endif
}

static Value nativeEnvUnset(VM* vm, int argc, Value* args) {
  (void)argc;
  if (!isObjType(args[0], OBJ_STRING)) {
    return runtimeErrorValue(vm, "env.unset expects a name string.");
  }
  ObjString* name = (ObjString*)AS_OBJ(args[0]);
#ifdef _WIN32
  if (!SetEnvironmentVariableA(name->chars, NULL)) {
    return runtimeErrorValue(vm, "env.unset failed.");
  }
#else
  if (unsetenv(name->chars) != 0) {
    return runtimeErrorValue(vm, "env.unset failed.");
  }
#endif
  return BOOL_VAL(true);
}

static Value nativeEnvAll(VM* vm, int argc, Value* args) {
  (void)argc;
  (void)args;
  ObjMap* result = newMap(vm);
#ifdef _WIN32
  LPCH block = GetEnvironmentStringsA();
  if (!block) {
    return runtimeErrorValue(vm, "env.all failed to read environment.");
  }
  for (char* entry = block; *entry; entry += strlen(entry) + 1) {
    char* eq = strchr(entry, '=');
    if (!eq || eq == entry || entry[0] == '=') {
      continue;
    }
    int keyLen = (int)(eq - entry);
    ObjString* key = copyStringWithLength(vm, entry, keyLen);
    ObjString* val = copyString(vm, eq + 1);
    mapSet(result, key, OBJ_VAL(val));
  }
  FreeEnvironmentStringsA(block);
#else
  extern char** environ;
  if (!environ) return OBJ_VAL(result);
  for (char** env = environ; *env; env++) {
    char* entry = *env;
    char* eq = strchr(entry, '=');
    if (!eq || eq == entry) continue;
    int keyLen = (int)(eq - entry);
    ObjString* key = copyStringWithLength(vm, entry, keyLen);
    ObjString* val = copyString(vm, eq + 1);
    mapSet(result, key, OBJ_VAL(val));
  }
#endif
  return OBJ_VAL(result);
}

static Value nativeEnvArgs(VM* vm, int argc, Value* args) {
  (void)argc;
  (void)args;
  return OBJ_VAL(vm->args);
}

void stdlib_register_env(VM* vm, ObjInstance* module) {
  moduleAdd(vm, module, "args", nativeEnvArgs, 0);
  moduleAdd(vm, module, "get", nativeEnvGet, 1);
  moduleAdd(vm, module, "set", nativeEnvSet, 2);
  moduleAdd(vm, module, "has", nativeEnvHas, 1);
  moduleAdd(vm, module, "unset", nativeEnvUnset, 1);
  moduleAdd(vm, module, "all", nativeEnvAll, 0);
}

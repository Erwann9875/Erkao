#include "stdlib_internal.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

static Value nativeOsPlatform(VM* vm, int argc, Value* args) {
  (void)argc; (void)args;
#ifdef _WIN32
  return OBJ_VAL(copyString(vm, "windows"));
#elif __APPLE__
  return OBJ_VAL(copyString(vm, "mac"));
#elif __linux__
  return OBJ_VAL(copyString(vm, "linux"));
#else
  return OBJ_VAL(copyString(vm, "unknown"));
#endif
}

static Value nativeOsArch(VM* vm, int argc, Value* args) {
  (void)argc; (void)args;
#if defined(_M_X64) || defined(__x86_64__) || defined(__amd64__)
  return OBJ_VAL(copyString(vm, "x64"));
#elif defined(_M_IX86) || defined(__i386__)
  return OBJ_VAL(copyString(vm, "x86"));
#elif defined(_M_ARM64) || defined(__aarch64__)
  return OBJ_VAL(copyString(vm, "arm64"));
#elif defined(_M_ARM) || defined(__arm__)
  return OBJ_VAL(copyString(vm, "arm"));
#else
  return OBJ_VAL(copyString(vm, "unknown"));
#endif
}

static Value nativeOsSep(VM* vm, int argc, Value* args) {
  (void)argc; (void)args;
#ifdef _WIN32
  return OBJ_VAL(copyString(vm, "\\"));
#else
  return OBJ_VAL(copyString(vm, "/"));
#endif
}

static Value nativeOsEol(VM* vm, int argc, Value* args) {
  (void)argc; (void)args;
#ifdef _WIN32
  return OBJ_VAL(copyString(vm, "\r\n"));
#else
  return OBJ_VAL(copyString(vm, "\n"));
#endif
}

static Value nativeOsCwd(VM* vm, int argc, Value* args) {
  (void)argc;
  (void)args;
  return stdlibCwdValue(vm);
}

static Value nativeOsHome(VM* vm, int argc, Value* args) {
  (void)argc; (void)args;
#ifdef _WIN32
  const char* home = getenv("USERPROFILE");
  if (home && *home) {
    return OBJ_VAL(copyString(vm, home));
  }
  const char* drive = getenv("HOMEDRIVE");
  const char* path = getenv("HOMEPATH");
  if (drive && path) {
    size_t total = strlen(drive) + strlen(path);
    char* buffer = (char*)malloc(total + 1);
    if (!buffer) {
      return runtimeErrorValue(vm, "os.home out of memory.");
    }
    snprintf(buffer, total + 1, "%s%s", drive, path);
    ObjString* result = copyStringWithLength(vm, buffer, (int)total);
    free(buffer);
    return OBJ_VAL(result);
  }
  return NULL_VAL;
#else
  const char* home = getenv("HOME");
  if (!home || !*home) return NULL_VAL;
  return OBJ_VAL(copyString(vm, home));
#endif
}

static Value nativeOsTmp(VM* vm, int argc, Value* args) {
  (void)argc; (void)args;
#ifdef _WIN32
  DWORD length = GetTempPathA(0, NULL);
  if (length == 0) {
    return runtimeErrorValue(vm, "os.tmp failed to read temp path.");
  }
  char* buffer = (char*)malloc((size_t)length + 1);
  if (!buffer) {
    return runtimeErrorValue(vm, "os.tmp out of memory.");
  }
  DWORD written = GetTempPathA(length + 1, buffer);
  if (written == 0) {
    free(buffer);
    return runtimeErrorValue(vm, "os.tmp failed to read temp path.");
  }
  ObjString* result = copyString(vm, buffer);
  free(buffer);
  return OBJ_VAL(result);
#else
  const char* tmp = getenv("TMPDIR");
  if (!tmp || !*tmp) tmp = getenv("TMP");
  if (!tmp || !*tmp) tmp = getenv("TEMP");
  if (!tmp || !*tmp) tmp = "/tmp";
  return OBJ_VAL(copyString(vm, tmp));
#endif
}


void stdlib_register_os(VM* vm, ObjInstance* module) {
  moduleAdd(vm, module, "platform", nativeOsPlatform, 0);
  moduleAdd(vm, module, "arch", nativeOsArch, 0);
  moduleAdd(vm, module, "sep", nativeOsSep, 0);
  moduleAdd(vm, module, "eol", nativeOsEol, 0);
  moduleAdd(vm, module, "cwd", nativeOsCwd, 0);
  moduleAdd(vm, module, "home", nativeOsHome, 0);
  moduleAdd(vm, module, "tmp", nativeOsTmp, 0);
}

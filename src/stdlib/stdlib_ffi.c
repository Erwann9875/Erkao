#include "stdlib_internal.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

static int ffiStoreHandle(VM* vm, void* handle, bool owns) {
  for (int i = 0; i < vm->ffiCount; i++) {
    if (!vm->ffiHandles[i].handle) {
      vm->ffiHandles[i].handle = handle;
      vm->ffiHandles[i].owns = owns;
      return i;
    }
  }
  if (vm->ffiCapacity < vm->ffiCount + 1) {
    int oldCap = vm->ffiCapacity;
    vm->ffiCapacity = GROW_CAPACITY(oldCap);
    FfiHandle* resized = GROW_ARRAY(FfiHandle, vm->ffiHandles, oldCap, vm->ffiCapacity);
    if (!resized) {
      vm->ffiCapacity = oldCap;
      return -1;
    }
    vm->ffiHandles = resized;
  }
  vm->ffiHandles[vm->ffiCount].handle = handle;
  vm->ffiHandles[vm->ffiCount].owns = owns;
  return vm->ffiCount++;
}

static bool ffiGetHandle(VM* vm, Value handleValue, int* outId, FfiHandle** outHandle) {
  if (!isObjType(handleValue, OBJ_MAP)) return false;
  ObjMap* map = (ObjMap*)AS_OBJ(handleValue);
  ObjString* key = copyString(vm, "_ffi");
  Value idValue;
  if (!mapGet(map, key, &idValue) || !IS_NUMBER(idValue)) return false;
  int id = (int)AS_NUMBER(idValue);
  if (id < 0 || id >= vm->ffiCount) return false;
  FfiHandle* handle = &vm->ffiHandles[id];
  if (!handle->handle) return false;
  if (outId) *outId = id;
  if (outHandle) *outHandle = handle;
  return true;
}

static Value nativeFfiOpen(VM* vm, int argc, Value* args) {
  if (!stdlibUnsafeEnabled(vm, ERKAO_UNSAFE_FFI, "ERKAO_ALLOW_FFI")) {
    return runtimeErrorValue(vm,
                             "ffi is disabled. Use --allow-unsafe=ffi or set ERKAO_ALLOW_FFI=1.");
  }
  if (argc != 1 || (!IS_NULL(args[0]) && !isObjType(args[0], OBJ_STRING))) {
    return runtimeErrorValue(vm, "ffi.open expects a path string or null.");
  }

  void* handle = NULL;
  bool owns = false;

  if (IS_NULL(args[0])) {
#ifdef _WIN32
    handle = (void*)GetModuleHandleA(NULL);
#else
    handle = dlopen(NULL, RTLD_NOW);
#endif
    owns = false;
  } else {
    ObjString* path = (ObjString*)AS_OBJ(args[0]);
#ifdef _WIN32
    handle = (void*)LoadLibraryA(path->chars);
    if (!handle) {
      char buffer[128];
      snprintf(buffer, sizeof(buffer), "LoadLibrary failed (%lu).", (unsigned long)GetLastError());
      return runtimeErrorValue(vm, buffer);
    }
#else
    handle = dlopen(path->chars, RTLD_NOW);
    if (!handle) {
      const char* error = dlerror();
      return runtimeErrorValue(vm, error ? error : "dlopen failed.");
    }
#endif
    owns = true;
  }

  if (!handle) {
    return runtimeErrorValue(vm, "ffi.open failed.");
  }

  int id = ffiStoreHandle(vm, handle, owns);
  if (id < 0) {
#ifdef _WIN32
    if (owns) FreeLibrary((HMODULE)handle);
#else
    if (owns) dlclose(handle);
#endif
    return runtimeErrorValue(vm, "ffi.open out of memory.");
  }
  ObjMap* out = newMap(vm);
  ObjString* key = copyString(vm, "_ffi");
  mapSet(out, key, NUMBER_VAL((double)id));
  return OBJ_VAL(out);
}

static Value nativeFfiClose(VM* vm, int argc, Value* args) {
  if (argc != 1) {
    return runtimeErrorValue(vm, "ffi.close expects a handle.");
  }
  int id = -1;
  FfiHandle* handle = NULL;
  if (!ffiGetHandle(vm, args[0], &id, &handle)) {
    return runtimeErrorValue(vm, "ffi.close expects a valid handle.");
  }
  if (handle->handle && handle->owns) {
#ifdef _WIN32
    FreeLibrary((HMODULE)handle->handle);
#else
    dlclose(handle->handle);
#endif
  }
  handle->handle = NULL;
  handle->owns = false;
  return NULL_VAL;
}

static Value nativeFfiCall(VM* vm, int argc, Value* args) {
  if (!stdlibUnsafeEnabled(vm, ERKAO_UNSAFE_FFI, "ERKAO_ALLOW_FFI")) {
    return runtimeErrorValue(vm,
                             "ffi is disabled. Use --allow-unsafe=ffi or set ERKAO_ALLOW_FFI=1.");
  }
  if (argc < 2) {
    return runtimeErrorValue(vm, "ffi.call expects (handle, name, ...args).");
  }
  FfiHandle* handle = NULL;
  if (!ffiGetHandle(vm, args[0], NULL, &handle)) {
    return runtimeErrorValue(vm, "ffi.call expects a valid handle.");
  }
  if (!isObjType(args[1], OBJ_STRING)) {
    return runtimeErrorValue(vm, "ffi.call expects a symbol name.");
  }
  ObjString* name = (ObjString*)AS_OBJ(args[1]);
#ifdef _WIN32
  FARPROC proc = GetProcAddress((HMODULE)handle->handle, name->chars);
  if (!proc) {
    char buffer[128];
    snprintf(buffer, sizeof(buffer), "GetProcAddress failed (%lu).",
             (unsigned long)GetLastError());
    return runtimeErrorValue(vm, buffer);
  }
  void* symbol = (void*)proc;
#else
  dlerror();
  void* symbol = dlsym(handle->handle, name->chars);
  const char* error = dlerror();
  if (!symbol || error) {
    return runtimeErrorValue(vm, error ? error : "dlsym failed.");
  }
#endif

  int argCount = argc - 2;
  if (argCount > 4) {
    return runtimeErrorValue(vm, "ffi.call supports up to 4 arguments.");
  }
  double values[4] = {0};
  for (int i = 0; i < argCount; i++) {
    Value value = args[i + 2];
    if (IS_NUMBER(value)) {
      values[i] = AS_NUMBER(value);
    } else if (IS_BOOL(value)) {
      values[i] = AS_BOOL(value) ? 1.0 : 0.0;
    } else {
      return runtimeErrorValue(vm, "ffi.call expects number arguments.");
    }
  }

  double result = 0.0;
  switch (argCount) {
    case 0:
      result = ((double (*)(void))symbol)();
      break;
    case 1:
      result = ((double (*)(double))symbol)(values[0]);
      break;
    case 2:
      result = ((double (*)(double, double))symbol)(values[0], values[1]);
      break;
    case 3:
      result = ((double (*)(double, double, double))symbol)(values[0], values[1], values[2]);
      break;
    case 4:
      result = ((double (*)(double, double, double, double))symbol)(values[0], values[1],
                                                                    values[2], values[3]);
      break;
    default:
      break;
  }

  return NUMBER_VAL(result);
}


void stdlib_register_ffi(VM* vm, ObjInstance* module) {
  moduleAdd(vm, module, "open", nativeFfiOpen, 1);
  moduleAdd(vm, module, "close", nativeFfiClose, 1);
  moduleAdd(vm, module, "call", nativeFfiCall, -1);
}

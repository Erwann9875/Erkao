#include "plugin.h"
#include "erkao_plugin.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

static void setError(char* error, size_t errorSize, const char* message) {
  if (!error || errorSize == 0) return;
  snprintf(error, errorSize, "%s", message);
  error[errorSize - 1] = '\0';
}

static void pluginStore(VM* vm, void* handle) {
  if (vm->pluginCapacity < vm->pluginCount + 1) {
    int oldCapacity = vm->pluginCapacity;
    vm->pluginCapacity = GROW_CAPACITY(oldCapacity);
    vm->pluginHandles = GROW_ARRAY(void*, vm->pluginHandles, oldCapacity, vm->pluginCapacity);
  }
  vm->pluginHandles[vm->pluginCount++] = handle;
}

static ErkaoModule* apiCreateModule(VM* vm, const char* name) {
  if (!vm || !name || name[0] == '\0') return NULL;
  ObjString* className = copyString(vm, name);
  ObjMap* methods = newMap(vm);
  ObjClass* klass = newClass(vm, className, methods);
  ObjInstance* module = newInstance(vm, klass);
  return (ErkaoModule*)module;
}

static void apiDefineModule(VM* vm, const char* name, ErkaoModule* module) {
  if (!vm || !name || name[0] == '\0' || !module) return;
  defineGlobal(vm, name, OBJ_VAL((ObjInstance*)module));
}

static void apiModuleAddNative(VM* vm, ErkaoModule* module, const char* name,
                               NativeFn function, int arity) {
  if (!vm || !module || !name || name[0] == '\0' || !function) return;
  ObjInstance* instance = (ObjInstance*)module;
  ObjString* fieldName = copyString(vm, name);
  ObjNative* native = newNative(vm, function, arity, fieldName);
  mapSet(instance->fields, fieldName, OBJ_VAL(native));
}

static void apiModuleAddValue(VM* vm, ErkaoModule* module, const char* name, Value value) {
  if (!vm || !module || !name || name[0] == '\0') return;
  ObjInstance* instance = (ObjInstance*)module;
  ObjString* fieldName = copyString(vm, name);
  mapSet(instance->fields, fieldName, value);
}

bool pluginLoad(VM* vm, const char* path, char* error, size_t errorSize) {
  if (!path || path[0] == '\0') {
    setError(error, errorSize, "plugin.load expects a path string.");
    return false;
  }

#ifdef _WIN32
  HMODULE library = LoadLibraryA(path);
  if (!library) {
    char buffer[128];
    snprintf(buffer, sizeof(buffer), "LoadLibrary failed (%lu).", (unsigned long)GetLastError());
    setError(error, errorSize, buffer);
    return false;
  }

  FARPROC proc = GetProcAddress(library, ERKAO_PLUGIN_INIT);
  if (!proc) {
    char buffer[128];
    snprintf(buffer, sizeof(buffer), "Missing %s export (%lu).", ERKAO_PLUGIN_INIT, (unsigned long)GetLastError());
    setError(error, errorSize, buffer);
    FreeLibrary(library);
    return false;
  }

  ErkaoPluginInit init = (ErkaoPluginInit)proc;
#else
  void* library = dlopen(path, RTLD_NOW);
  if (!library) {
    setError(error, errorSize, dlerror());
    return false;
  }

  ErkaoPluginInit init = (ErkaoPluginInit)dlsym(library, ERKAO_PLUGIN_INIT);
  if (!init) {
    setError(error, errorSize, "Missing erkao_init export.");
    dlclose(library);
    return false;
  }
#endif

  ErkaoApi api;
  memset(&api, 0, sizeof(api));
  api.apiVersion = ERKAO_PLUGIN_API_VERSION;
  api.vm = vm;
  api.defineNative = defineNative;
  api.size = sizeof(ErkaoApi);
  api.abiVersion = ERKAO_PLUGIN_ABI_VERSION;
  api.features = ERKAO_PLUGIN_FEATURE_MODULES;
  api.createModule = apiCreateModule;
  api.defineModule = apiDefineModule;
  api.moduleAddNative = apiModuleAddNative;
  api.moduleAddValue = apiModuleAddValue;

  if (!init(&api)) {
    setError(error, errorSize, "Plugin init failed.");
#ifdef _WIN32
    FreeLibrary(library);
#else
    dlclose(library);
#endif
    return false;
  }

  pluginStore(vm, (void*)library);
  return true;
}

void pluginUnloadAll(VM* vm) {
#ifdef _WIN32
  for (int i = 0; i < vm->pluginCount; i++) {
    FreeLibrary((HMODULE)vm->pluginHandles[i]);
  }
#else
  for (int i = 0; i < vm->pluginCount; i++) {
    dlclose(vm->pluginHandles[i]);
  }
#endif

  FREE_ARRAY(void*, vm->pluginHandles, vm->pluginCapacity);
  vm->pluginHandles = NULL;
  vm->pluginCount = 0;
  vm->pluginCapacity = 0;
}

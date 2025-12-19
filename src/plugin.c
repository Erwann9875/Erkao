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
  api.apiVersion = ERKAO_PLUGIN_API_VERSION;
  api.vm = vm;
  api.defineNative = defineNative;

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

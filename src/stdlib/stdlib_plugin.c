#include "stdlib_internal.h"
#include "plugin.h"

static Value nativePluginLoad(VM* vm, int argc, Value* args) {
  (void)argc;
  if (!stdlibUnsafeEnabled("ERKAO_ALLOW_PLUGINS")) {
    return runtimeErrorValue(vm,
                             "plugin.load is disabled. Set ERKAO_ALLOW_PLUGINS=1 to enable.");
  }
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


void stdlib_register_plugin(VM* vm, ObjInstance* module) {
  moduleAdd(vm, module, "load", nativePluginLoad, 1);
}

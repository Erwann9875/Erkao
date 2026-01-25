#include "stdlib_internal.h"

static Value nativeProcRun(VM* vm, int argc, Value* args) {
  (void)argc;
  if (!isObjType(args[0], OBJ_STRING)) {
    return runtimeErrorValue(vm, "proc.run expects a command string.");
  }
  ObjString* cmd = (ObjString*)AS_OBJ(args[0]);
  int result = system(cmd->chars);
  return NUMBER_VAL((double)result);
}


void stdlib_register_proc(VM* vm, ObjInstance* module) {
  moduleAdd(vm, module, "run", nativeProcRun, 1);
}

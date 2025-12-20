#include "interpreter_internal.h"
#include "program.h"

void runtimeError(VM* vm, Token token, const char* message) {
  const char* displayPath = "<repl>";
  if (vm->currentProgram && vm->currentProgram->path) {
    displayPath = vm->currentProgram->path;
  }

  if (token.line > 0 && token.column > 0) {
    fprintf(stderr, "%s:%d:%d: RuntimeError", displayPath, token.line, token.column);
    if (token.length > 0) {
      fprintf(stderr, " at '%.*s'", token.length, token.start);
    }
    fprintf(stderr, ": %s\n", message);
    if (vm->currentProgram) {
      int length = token.length > 0 ? token.length : 1;
      printErrorContext(vm->currentProgram->source, token.line, token.column, length);
    }
  } else {
    fprintf(stderr, "%s: RuntimeError: %s\n", displayPath, message);
  }
  vm->hadError = true;
}

bool isTruthy(Value value) {
  if (IS_NULL(value)) return false;
  if (IS_BOOL(value)) return AS_BOOL(value);
  return true;
}

bool isString(Value value) {
  return isObjType(value, OBJ_STRING);
}

ObjString* asString(Value value) {
  return (ObjString*)AS_OBJ(value);
}

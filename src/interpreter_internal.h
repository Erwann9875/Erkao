#ifndef ERKAO_INTERPRETER_INTERNAL_H
#define ERKAO_INTERPRETER_INTERNAL_H

#include "interpreter.h"

Env* newEnv(VM* vm, Env* enclosing);
bool envGetByName(Env* env, ObjString* name, Value* out);
bool envAssignByName(Env* env, ObjString* name, Value value);
void envDefine(Env* env, ObjString* name, Value value);

void runtimeError(VM* vm, Token token, const char* message);
bool isTruthy(Value value);
bool isString(Value value);
ObjString* asString(Value value);

char* resolveImportPath(VM* vm, const char* currentPath, const char* importPath);
bool hasExtension(const char* path);
ObjFunction* loadModuleFunction(VM* vm, Token keyword, const char* path);

#endif

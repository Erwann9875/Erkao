#ifndef ERKAO_INTERPRETER_INTERNAL_H
#define ERKAO_INTERPRETER_INTERNAL_H

#include "interpreter.h"

typedef enum {
  EXEC_OK,
  EXEC_RETURN,
  EXEC_ERROR
} ExecType;

typedef struct {
  ExecType type;
  Value value;
} ExecResult;

Env* newEnv(VM* vm, Env* enclosing);
bool envGet(Env* env, Token name, Value* out);
bool envAssign(Env* env, Token name, Value value);
void envDefine(Env* env, ObjString* name, Value value);

void runtimeError(VM* vm, Token token, const char* message);
bool isTruthy(Value value);
bool isString(Value value);
ObjString* asString(Value value);

Value evaluate(VM* vm, Expr* expr);
ExecResult execute(VM* vm, Stmt* stmt);
ExecResult executeBlock(VM* vm, const StmtArray* statements, Env* env);

char* resolveImportPath(const char* currentPath, const char* importPath);
bool hasExtension(const char* path);
ObjInstance* loadModule(VM* vm, Token keyword, const char* path);

#endif

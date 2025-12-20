#include "interpreter_internal.h"
#include "program.h"

#include <stdlib.h>
#include <string.h>

static ExecResult execOk(void) {
  ExecResult result;
  result.type = EXEC_OK;
  result.value = NULL_VAL;
  return result;
}

static ExecResult execReturn(Value value) {
  ExecResult result;
  result.type = EXEC_RETURN;
  result.value = value;
  return result;
}

static ExecResult execError(void) {
  ExecResult result;
  result.type = EXEC_ERROR;
  result.value = NULL_VAL;
  return result;
}

ExecResult executeBlock(VM* vm, const StmtArray* statements, Env* env) {
  Env* previous = vm->env;
  vm->env = env;

  for (int i = 0; i < statements->count; i++) {
    ExecResult result = execute(vm, statements->items[i]);
    if (result.type != EXEC_OK) {
      vm->env = previous;
      return result;
    }
    gcMaybe(vm);
  }

  vm->env = previous;
  return execOk();
}

ExecResult execute(VM* vm, Stmt* stmt) {
  if (vm->hadError) return execError();

  switch (stmt->type) {
    case STMT_EXPR:
      (void)evaluate(vm, stmt->as.expr.expression);
      return vm->hadError ? execError() : execOk();
    case STMT_VAR: {
      Value value = NULL_VAL;
      if (stmt->as.var.initializer) {
        value = evaluate(vm, stmt->as.var.initializer);
        if (vm->hadError) return execError();
      }
      ObjString* name = stringFromToken(vm, stmt->as.var.name);
      envDefine(vm->env, name, value);
      return execOk();
    }
    case STMT_BLOCK: {
      Env* blockEnv = newEnv(vm, vm->env);
      return executeBlock(vm, &stmt->as.block.statements, blockEnv);
    }
    case STMT_IF: {
      Value condition = evaluate(vm, stmt->as.ifStmt.condition);
      if (vm->hadError) return execError();
      if (isTruthy(condition)) {
        return execute(vm, stmt->as.ifStmt.thenBranch);
      }
      if (stmt->as.ifStmt.elseBranch) {
        return execute(vm, stmt->as.ifStmt.elseBranch);
      }
      return execOk();
    }
    case STMT_WHILE: {
      while (true) {
        Value condition = evaluate(vm, stmt->as.whileStmt.condition);
        if (vm->hadError) return execError();
        if (!isTruthy(condition)) break;
        ExecResult result = execute(vm, stmt->as.whileStmt.body);
        if (result.type != EXEC_OK) return result;
        gcMaybe(vm);
      }
      return execOk();
    }
    case STMT_IMPORT: {
      Value pathValue = evaluate(vm, stmt->as.importStmt.path);
      if (vm->hadError) return execError();
      if (!isString(pathValue)) {
        runtimeError(vm, stmt->as.importStmt.keyword, "Import path must be a string.");
        return execError();
      }

      ObjString* pathString = asString(pathValue);
      char* resolvedPath = resolveImportPath(
          vm->currentProgram ? vm->currentProgram->path : NULL,
          pathString->chars);
      if (!resolvedPath) {
        runtimeError(vm, stmt->as.importStmt.keyword, "Failed to resolve import path.");
        return execError();
      }

      char* candidatePath = resolvedPath;
      if (!hasExtension(candidatePath)) {
        size_t length = strlen(candidatePath);
        char* withExt = (char*)malloc(length + 4);
        if (!withExt) {
          fprintf(stderr, "Out of memory.\n");
          exit(1);
        }
        memcpy(withExt, candidatePath, length);
        memcpy(withExt + length, ".ek", 4);
        candidatePath = withExt;
      }

      Token pathToken;
      memset(&pathToken, 0, sizeof(Token));
      pathToken.start = candidatePath;
      pathToken.length = (int)strlen(candidatePath);

      Value cached;
      if (mapGetByToken(vm->modules, pathToken, &cached)) {
        if (IS_OBJ(cached)) {
          if (stmt->as.importStmt.hasAlias) {
            ObjString* name = stringFromToken(vm, stmt->as.importStmt.alias);
            envDefine(vm->env, name, cached);
          }
          if (candidatePath != resolvedPath) free(candidatePath);
          free(resolvedPath);
          return execOk();
        }
        if (IS_BOOL(cached) && AS_BOOL(cached)) {
          runtimeError(vm, stmt->as.importStmt.keyword, "Circular import detected.");
          if (candidatePath != resolvedPath) free(candidatePath);
          free(resolvedPath);
          return execError();
        }
      }

      ObjString* key = copyStringWithLength(vm, candidatePath, pathToken.length);
      mapSet(vm->modules, key, BOOL_VAL(true));

      ObjInstance* module = loadModule(vm, stmt->as.importStmt.keyword, candidatePath);
      if (module) {
        mapSet(vm->modules, key, OBJ_VAL(module));
        if (stmt->as.importStmt.hasAlias) {
          ObjString* name = stringFromToken(vm, stmt->as.importStmt.alias);
          envDefine(vm->env, name, OBJ_VAL(module));
        }
      } else {
        mapSet(vm->modules, key, NULL_VAL);
      }

      if (candidatePath != resolvedPath) free(candidatePath);
      free(resolvedPath);
      return module ? execOk() : execError();
    }
    case STMT_FUNCTION: {
      ObjString* name = stringFromToken(vm, stmt->as.function.name);
      int arity = stmt->as.function.params.count;
      bool isInitializer = false;
      ObjFunction* function = newFunction(vm, stmt, name, arity, isInitializer, vm->env,
                                          vm->currentProgram);
      envDefine(vm->env, name, OBJ_VAL(function));
      return execOk();
    }
    case STMT_RETURN: {
      Value value = NULL_VAL;
      if (stmt->as.ret.value) {
        value = evaluate(vm, stmt->as.ret.value);
        if (vm->hadError) return execError();
      }
      return execReturn(value);
    }
    case STMT_CLASS: {
      ObjString* name = stringFromToken(vm, stmt->as.classStmt.name);
      envDefine(vm->env, name, NULL_VAL);

      ObjMap* methods = newMap(vm);
      for (int i = 0; i < stmt->as.classStmt.methods.count; i++) {
        Stmt* methodStmt = stmt->as.classStmt.methods.items[i];
        ObjString* methodName = stringFromToken(vm, methodStmt->as.function.name);
        bool isInitializer = methodStmt->as.function.name.length == 4 &&
                             memcmp(methodStmt->as.function.name.start, "init", 4) == 0;
        ObjFunction* method = newFunction(vm, methodStmt, methodName,
                                          methodStmt->as.function.params.count,
                                          isInitializer, vm->env, vm->currentProgram);
        mapSet(methods, methodName, OBJ_VAL(method));
      }

      ObjClass* klass = newClass(vm, name, methods);
      envAssign(vm->env, stmt->as.classStmt.name, OBJ_VAL(klass));
      return execOk();
    }
  }

  return execOk();
}

bool interpret(VM* vm, Program* program) {
  vm->hadError = false;
  Program* previousProgram = vm->currentProgram;
  vm->currentProgram = program;
  programRunBegin(program);

  for (int i = 0; i < program->statements.count; i++) {
    ExecResult result = execute(vm, program->statements.items[i]);
    if (result.type == EXEC_ERROR) {
      programRunEnd(vm, program);
      vm->currentProgram = previousProgram;
      return false;
    }
    gcMaybe(vm);
  }

  programRunEnd(vm, program);
  vm->currentProgram = previousProgram;
  return !vm->hadError;
}

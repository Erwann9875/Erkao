#include "interpreter_internal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static bool callFunction(VM* vm, ObjFunction* function, Value receiver,
                         bool hasReceiver, int argc, Value* args, Value* out) {
  if (argc != function->arity) {
    Token token;
    memset(&token, 0, sizeof(Token));
    token.line = 0;
    token.column = 0;
    runtimeError(vm, token, "Wrong number of arguments.");
    return false;
  }

  Env* env = newEnv(vm, function->closure);
  if (hasReceiver) {
    ObjString* thisName = copyString(vm, "this");
    envDefine(env, thisName, receiver);
  }

  Stmt* decl = function->declaration;
  for (int i = 0; i < function->arity; i++) {
    Token param = decl->as.function.params.items[i];
    ObjString* name = stringFromToken(vm, param);
    envDefine(env, name, args[i]);
  }

  Program* previousProgram = vm->currentProgram;
  vm->currentProgram = function->program;
  ExecResult result = executeBlock(vm, &decl->as.function.body, env);
  vm->currentProgram = previousProgram;
  if (result.type == EXEC_ERROR) return false;

  if (function->isInitializer) {
    *out = receiver;
    return true;
  }

  if (result.type == EXEC_RETURN) {
    *out = result.value;
    return true;
  }

  *out = NULL_VAL;
  return true;
}

static bool callValue(VM* vm, Value callee, int argc, Value* args, Value* out) {
  if (isObjType(callee, OBJ_FUNCTION)) {
    return callFunction(vm, (ObjFunction*)AS_OBJ(callee), NULL_VAL, false, argc, args, out);
  }

  if (isObjType(callee, OBJ_BOUND_METHOD)) {
    ObjBoundMethod* bound = (ObjBoundMethod*)AS_OBJ(callee);
    return callFunction(vm, bound->method, bound->receiver, true, argc, args, out);
  }

  if (isObjType(callee, OBJ_NATIVE)) {
    ObjNative* native = (ObjNative*)AS_OBJ(callee);
    if (native->arity >= 0 && argc != native->arity) {
      Token token;
      memset(&token, 0, sizeof(Token));
      token.line = 0;
      token.column = 0;
      runtimeError(vm, token, "Wrong number of arguments.");
      return false;
    }
    *out = native->function(vm, argc, args);
    return !vm->hadError;
  }

  if (isObjType(callee, OBJ_CLASS)) {
    ObjClass* klass = (ObjClass*)AS_OBJ(callee);
    ObjInstance* instance = newInstance(vm, klass);
    Value instanceValue = OBJ_VAL(instance);

    Token initToken;
    memset(&initToken, 0, sizeof(Token));
    initToken.start = "init";
    initToken.length = 4;
    Value initValue;
    if (mapGetByToken(klass->methods, initToken, &initValue)) {
      ObjFunction* init = (ObjFunction*)AS_OBJ(initValue);
      if (!callFunction(vm, init, instanceValue, true, argc, args, out)) return false;
      *out = instanceValue;
      return true;
    }

    if (argc != 0) {
      Token token;
      memset(&token, 0, sizeof(Token));
      token.line = 0;
      token.column = 0;
      runtimeError(vm, token, "Expected 0 arguments to construct this class.");
      return false;
    }

    *out = instanceValue;
    return true;
  }

  Token token;
  memset(&token, 0, sizeof(Token));
  token.line = 0;
  token.column = 0;
  runtimeError(vm, token, "Can only call functions and classes.");
  return false;
}

static bool ensureNumberOperand(VM* vm, Token op, Value value) {
  if (IS_NUMBER(value)) return true;
  runtimeError(vm, op, "Operand must be a number.");
  return false;
}

static bool ensureNumberOperands(VM* vm, Token op, Value left, Value right) {
  if (IS_NUMBER(left) && IS_NUMBER(right)) return true;
  runtimeError(vm, op, "Operands must be numbers.");
  return false;
}

static bool valueIsInteger(Value value, int* out) {
  if (!IS_NUMBER(value)) return false;
  double number = AS_NUMBER(value);
  double truncated = floor(number);
  if (number != truncated) return false;
  *out = (int)truncated;
  return true;
}

static Value concatenateStrings(VM* vm, ObjString* a, ObjString* b) {
  int length = a->length + b->length;
  char* buffer = (char*)malloc((size_t)length + 1);
  if (!buffer) {
    fprintf(stderr, "Out of memory.\n");
    exit(1);
  }
  memcpy(buffer, a->chars, (size_t)a->length);
  memcpy(buffer + a->length, b->chars, (size_t)b->length);
  buffer[length] = '\0';
  ObjString* result = copyStringWithLength(vm, buffer, length);
  free(buffer);
  return OBJ_VAL(result);
}

static bool findMethodByToken(ObjClass* klass, Token name, ObjFunction** out) {
  Value value;
  if (mapGetByToken(klass->methods, name, &value)) {
    if (isObjType(value, OBJ_FUNCTION)) {
      *out = (ObjFunction*)AS_OBJ(value);
      return true;
    }
  }
  return false;
}

static Value evaluateIndex(VM* vm, Token token, Value object, Value index) {
  if (isObjType(object, OBJ_ARRAY)) {
    int i = 0;
    if (!valueIsInteger(index, &i)) {
      runtimeError(vm, token, "Array index must be an integer.");
      return NULL_VAL;
    }
    Value out;
    if (!arrayGet((ObjArray*)AS_OBJ(object), i, &out)) {
      runtimeError(vm, token, "Array index out of bounds.");
      return NULL_VAL;
    }
    return out;
  }

  if (isObjType(object, OBJ_MAP)) {
    if (!isString(index)) {
      runtimeError(vm, token, "Map index must be a string.");
      return NULL_VAL;
    }
    Value out;
    if (mapGet((ObjMap*)AS_OBJ(object), asString(index), &out)) {
      return out;
    }
    return NULL_VAL;
  }

  runtimeError(vm, token, "Only arrays and maps can be indexed.");
  return NULL_VAL;
}

static Value evaluateSetIndex(VM* vm, Token token, Value object, Value index, Value value) {
  if (isObjType(object, OBJ_ARRAY)) {
    int i = 0;
    if (!valueIsInteger(index, &i)) {
      runtimeError(vm, token, "Array index must be an integer.");
      return NULL_VAL;
    }
    if (!arraySet((ObjArray*)AS_OBJ(object), i, value)) {
      runtimeError(vm, token, "Array index out of bounds.");
      return NULL_VAL;
    }
    return value;
  }

  if (isObjType(object, OBJ_MAP)) {
    if (!isString(index)) {
      runtimeError(vm, token, "Map index must be a string.");
      return NULL_VAL;
    }
    mapSet((ObjMap*)AS_OBJ(object), asString(index), value);
    return value;
  }

  runtimeError(vm, token, "Only arrays and maps can be indexed.");
  return NULL_VAL;
}

Value evaluate(VM* vm, Expr* expr) {
  if (vm->hadError) return NULL_VAL;

  switch (expr->type) {
    case EXPR_LITERAL: {
      Literal literal = expr->as.literal.literal;
      switch (literal.type) {
        case LIT_NUMBER:
          return NUMBER_VAL(literal.as.number);
        case LIT_STRING:
          return OBJ_VAL(copyString(vm, literal.as.string));
        case LIT_BOOL:
          return BOOL_VAL(literal.as.boolean);
        case LIT_NULL:
          return NULL_VAL;
      }
      break;
    }
    case EXPR_GROUPING:
      return evaluate(vm, expr->as.grouping.expression);
    case EXPR_UNARY: {
      Value right = evaluate(vm, expr->as.unary.right);
      if (vm->hadError) return NULL_VAL;
      switch (expr->as.unary.op.type) {
        case TOKEN_MINUS:
          if (!ensureNumberOperand(vm, expr->as.unary.op, right)) return NULL_VAL;
          return NUMBER_VAL(-AS_NUMBER(right));
        case TOKEN_BANG:
          return BOOL_VAL(!isTruthy(right));
        default:
          break;
      }
      break;
    }
    case EXPR_BINARY: {
      Value left = evaluate(vm, expr->as.binary.left);
      if (vm->hadError) return NULL_VAL;
      Value right = evaluate(vm, expr->as.binary.right);
      if (vm->hadError) return NULL_VAL;

      switch (expr->as.binary.op.type) {
        case TOKEN_PLUS:
          if (IS_NUMBER(left) && IS_NUMBER(right)) {
            return NUMBER_VAL(AS_NUMBER(left) + AS_NUMBER(right));
          }
          if (isString(left) && isString(right)) {
            return concatenateStrings(vm, asString(left), asString(right));
          }
          runtimeError(vm, expr->as.binary.op, "Operands must be two numbers or two strings.");
          return NULL_VAL;
        case TOKEN_MINUS:
          if (!ensureNumberOperands(vm, expr->as.binary.op, left, right)) return NULL_VAL;
          return NUMBER_VAL(AS_NUMBER(left) - AS_NUMBER(right));
        case TOKEN_STAR:
          if (!ensureNumberOperands(vm, expr->as.binary.op, left, right)) return NULL_VAL;
          return NUMBER_VAL(AS_NUMBER(left) * AS_NUMBER(right));
        case TOKEN_SLASH:
          if (!ensureNumberOperands(vm, expr->as.binary.op, left, right)) return NULL_VAL;
          return NUMBER_VAL(AS_NUMBER(left) / AS_NUMBER(right));
        case TOKEN_GREATER:
          if (!ensureNumberOperands(vm, expr->as.binary.op, left, right)) return NULL_VAL;
          return BOOL_VAL(AS_NUMBER(left) > AS_NUMBER(right));
        case TOKEN_GREATER_EQUAL:
          if (!ensureNumberOperands(vm, expr->as.binary.op, left, right)) return NULL_VAL;
          return BOOL_VAL(AS_NUMBER(left) >= AS_NUMBER(right));
        case TOKEN_LESS:
          if (!ensureNumberOperands(vm, expr->as.binary.op, left, right)) return NULL_VAL;
          return BOOL_VAL(AS_NUMBER(left) < AS_NUMBER(right));
        case TOKEN_LESS_EQUAL:
          if (!ensureNumberOperands(vm, expr->as.binary.op, left, right)) return NULL_VAL;
          return BOOL_VAL(AS_NUMBER(left) <= AS_NUMBER(right));
        case TOKEN_BANG_EQUAL:
          return BOOL_VAL(!valuesEqual(left, right));
        case TOKEN_EQUAL_EQUAL:
          return BOOL_VAL(valuesEqual(left, right));
        default:
          break;
      }
      break;
    }
    case EXPR_VARIABLE: {
      Value value;
      if (!envGet(vm->env, expr->as.variable.name, &value)) {
        runtimeError(vm, expr->as.variable.name, "Undefined variable.");
        return NULL_VAL;
      }
      return value;
    }
    case EXPR_ASSIGN: {
      Value value = evaluate(vm, expr->as.assign.value);
      if (vm->hadError) return NULL_VAL;
      if (!envAssign(vm->env, expr->as.assign.name, value)) {
        runtimeError(vm, expr->as.assign.name, "Undefined variable.");
        return NULL_VAL;
      }
      return value;
    }
    case EXPR_LOGICAL: {
      Value left = evaluate(vm, expr->as.logical.left);
      if (vm->hadError) return NULL_VAL;
      if (expr->as.logical.op.type == TOKEN_OR) {
        if (isTruthy(left)) return left;
      } else {
        if (!isTruthy(left)) return left;
      }
      return evaluate(vm, expr->as.logical.right);
    }
    case EXPR_CALL: {
      Value callee = evaluate(vm, expr->as.call.callee);
      if (vm->hadError) return NULL_VAL;

      int argCount = expr->as.call.args.count;
      Value* args = NULL;
      if (argCount > 0) {
        args = (Value*)malloc(sizeof(Value) * (size_t)argCount);
        if (!args) {
          fprintf(stderr, "Out of memory.\n");
          exit(1);
        }
        for (int i = 0; i < argCount; i++) {
          args[i] = evaluate(vm, expr->as.call.args.items[i]);
          if (vm->hadError) {
            free(args);
            return NULL_VAL;
          }
        }
      }

      Value result = NULL_VAL;
      bool ok = callValue(vm, callee, argCount, args, &result);
      free(args);
      if (!ok) return NULL_VAL;
      return result;
    }
    case EXPR_GET: {
      Value object = evaluate(vm, expr->as.get.object);
      if (vm->hadError) return NULL_VAL;
      if (isObjType(object, OBJ_INSTANCE)) {
        ObjInstance* instance = (ObjInstance*)AS_OBJ(object);
        Value value;
        if (mapGetByToken(instance->fields, expr->as.get.name, &value)) {
          return value;
        }
        ObjFunction* method = NULL;
        if (findMethodByToken(instance->klass, expr->as.get.name, &method)) {
          ObjBoundMethod* bound = newBoundMethod(vm, object, method);
          return OBJ_VAL(bound);
        }
        runtimeError(vm, expr->as.get.name, "Undefined property.");
        return NULL_VAL;
      }
      runtimeError(vm, expr->as.get.name, "Only instances have properties.");
      return NULL_VAL;
    }
    case EXPR_SET: {
      Value object = evaluate(vm, expr->as.set.object);
      if (vm->hadError) return NULL_VAL;
      if (!isObjType(object, OBJ_INSTANCE)) {
        runtimeError(vm, expr->as.set.name, "Only instances have fields.");
        return NULL_VAL;
      }
      Value value = evaluate(vm, expr->as.set.value);
      if (vm->hadError) return NULL_VAL;
      ObjInstance* instance = (ObjInstance*)AS_OBJ(object);
      ObjString* key = stringFromToken(vm, expr->as.set.name);
      mapSet(instance->fields, key, value);
      return value;
    }
    case EXPR_THIS: {
      Value value;
      if (!envGet(vm->env, expr->as.thisExpr.keyword, &value)) {
        runtimeError(vm, expr->as.thisExpr.keyword, "Cannot use 'this' outside of a class.");
        return NULL_VAL;
      }
      return value;
    }
    case EXPR_ARRAY: {
      ObjArray* array = newArray(vm);
      for (int i = 0; i < expr->as.array.elements.count; i++) {
        Value value = evaluate(vm, expr->as.array.elements.items[i]);
        if (vm->hadError) return NULL_VAL;
        arrayWrite(array, value);
      }
      return OBJ_VAL(array);
    }
    case EXPR_MAP: {
      ObjMap* map = newMap(vm);
      for (int i = 0; i < expr->as.map.entries.count; i++) {
        Value key = evaluate(vm, expr->as.map.entries.entries[i].key);
        if (vm->hadError) return NULL_VAL;
        if (!isString(key)) {
          Token token;
          memset(&token, 0, sizeof(Token));
          runtimeError(vm, token, "Map keys must be strings.");
          return NULL_VAL;
        }
        Value value = evaluate(vm, expr->as.map.entries.entries[i].value);
        if (vm->hadError) return NULL_VAL;
        mapSet(map, asString(key), value);
      }
      return OBJ_VAL(map);
    }
    case EXPR_INDEX: {
      Value object = evaluate(vm, expr->as.index.object);
      if (vm->hadError) return NULL_VAL;
      Value index = evaluate(vm, expr->as.index.index);
      if (vm->hadError) return NULL_VAL;
      return evaluateIndex(vm, expr->as.index.bracket, object, index);
    }
    case EXPR_SET_INDEX: {
      Value object = evaluate(vm, expr->as.setIndex.object);
      if (vm->hadError) return NULL_VAL;
      Value index = evaluate(vm, expr->as.setIndex.index);
      if (vm->hadError) return NULL_VAL;
      Value value = evaluate(vm, expr->as.setIndex.value);
      if (vm->hadError) return NULL_VAL;
      return evaluateSetIndex(vm, expr->as.setIndex.equals, object, index, value);
    }
  }

  return NULL_VAL;
}

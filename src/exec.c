#include "interpreter_internal.h"
#include "compiler.h"
#include "chunk.h"
#include "gc.h"
#include "program.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static void resetStack(VM* vm) {
  vm->stackTop = vm->stack;
  vm->frameCount = 0;
}

static void push(VM* vm, Value value) {
  *vm->stackTop = value;
  vm->stackTop++;
}

static Value pop(VM* vm) {
  vm->stackTop--;
  return *vm->stackTop;
}

static Value peek(VM* vm, int distance) {
  return vm->stackTop[-1 - distance];
}

static Token currentToken(CallFrame* frame) {
  Token token;
  memset(&token, 0, sizeof(Token));
  size_t offset = (size_t)(frame->ip - frame->function->chunk->code - 1);
  if (offset < (size_t)frame->function->chunk->count) {
    token = frame->function->chunk->tokens[offset];
  }
  return token;
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
  ObjString* result = takeStringWithLength(vm, buffer, length);
  return OBJ_VAL(result);
}

static ObjString* moduleNameFromPath(VM* vm, const char* path) {
  const char* lastSlash = strrchr(path, '/');
  const char* lastBackslash = strrchr(path, '\\');
  const char* base = path;
  if (lastSlash && lastSlash + 1 > base) base = lastSlash + 1;
  if (lastBackslash && lastBackslash + 1 > base) base = lastBackslash + 1;
  const char* dot = strrchr(base, '.');
  int length = dot && dot > base ? (int)(dot - base) : (int)strlen(base);
  return copyStringWithLength(vm, base, length);
}

static bool findMethodByName(ObjClass* klass, ObjString* name, ObjFunction** out) {
  Value value;
  if (mapGet(klass->methods, name, &value)) {
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

static bool callFunction(VM* vm, ObjFunction* function, Value receiver,
                         bool hasReceiver, int argc) {
  if (argc != function->arity) {
    Token token;
    memset(&token, 0, sizeof(Token));
    runtimeError(vm, token, "Wrong number of arguments.");
    return false;
  }

  if (vm->frameCount == FRAMES_MAX) {
    Token token;
    memset(&token, 0, sizeof(Token));
    runtimeError(vm, token, "Stack overflow.");
    return false;
  }

  CallFrame* frame = &vm->frames[vm->frameCount++];
  frame->function = function;
  frame->ip = function->chunk->code;
  frame->slots = vm->stackTop - argc - 1;
  frame->previousEnv = vm->env;
  frame->previousProgram = vm->currentProgram;
  frame->receiver = hasReceiver ? receiver : NULL_VAL;
  frame->isModule = false;
  frame->discardResult = false;
  frame->moduleInstance = NULL;
  frame->moduleAlias = NULL;
  frame->moduleKey = NULL;
  frame->moduleHasAlias = false;

  Env* env = newEnv(vm, function->closure);
  if (hasReceiver) {
    ObjString* thisName = copyString(vm, "this");
    envDefine(env, thisName, receiver);
  }
  for (int i = 0; i < function->arity; i++) {
    envDefine(env, function->params[i], frame->slots[i + 1]);
  }

  vm->env = env;
  vm->currentProgram = function->program;
  return true;
}

static bool callValue(VM* vm, Value callee, int argc) {
  if (isObjType(callee, OBJ_FUNCTION)) {
    return callFunction(vm, (ObjFunction*)AS_OBJ(callee), NULL_VAL, false, argc);
  }

  if (isObjType(callee, OBJ_BOUND_METHOD)) {
    ObjBoundMethod* bound = (ObjBoundMethod*)AS_OBJ(callee);
    return callFunction(vm, bound->method, bound->receiver, true, argc);
  }

  if (isObjType(callee, OBJ_NATIVE)) {
    ObjNative* native = (ObjNative*)AS_OBJ(callee);
    if (native->arity >= 0 && argc != native->arity) {
      Token token;
      memset(&token, 0, sizeof(Token));
      runtimeError(vm, token, "Wrong number of arguments.");
      return false;
    }
    Value result = native->function(vm, argc, vm->stackTop - argc);
    if (vm->hadError) return false;
    vm->stackTop -= argc + 1;
    push(vm, result);
    return true;
  }

  if (isObjType(callee, OBJ_CLASS)) {
    ObjClass* klass = (ObjClass*)AS_OBJ(callee);
    ObjInstance* instance = newInstance(vm, klass);
    Value instanceValue = OBJ_VAL(instance);

    ObjString* initName = copyString(vm, "init");
    Value initValue;
    if (mapGet(klass->methods, initName, &initValue)) {
      ObjFunction* init = (ObjFunction*)AS_OBJ(initValue);
      return callFunction(vm, init, instanceValue, true, argc);
    }

    if (argc != 0) {
      Token token;
      memset(&token, 0, sizeof(Token));
      runtimeError(vm, token, "Expected 0 arguments to construct this class.");
      return false;
    }

    vm->stackTop -= argc + 1;
    push(vm, instanceValue);
    return true;
  }

  Token token;
  memset(&token, 0, sizeof(Token));
  runtimeError(vm, token, "Can only call functions and classes.");
  return false;
}

static bool run(VM* vm) {
  CallFrame* frame = &vm->frames[vm->frameCount - 1];

#define READ_BYTE() (*frame->ip++)
#define READ_SHORT() (frame->ip += 2, (uint16_t)((frame->ip[-2] << 8) | frame->ip[-1]))
#define READ_CONSTANT() (frame->function->chunk->constants[READ_SHORT()])

  for (;;) {
    uint8_t instruction = READ_BYTE();
    size_t instructionOffset = (size_t)(frame->ip - frame->function->chunk->code - 1);
    InlineCache* cache = NULL;
    if (frame->function->chunk->caches &&
        instructionOffset < (size_t)frame->function->chunk->count) {
      cache = &frame->function->chunk->caches[instructionOffset];
    }
    switch (instruction) {
      case OP_CONSTANT: {
        Value constant = READ_CONSTANT();
        push(vm, constant);
        break;
      }
      case OP_NULL:
        push(vm, NULL_VAL);
        break;
      case OP_TRUE:
        push(vm, BOOL_VAL(true));
        break;
      case OP_FALSE:
        push(vm, BOOL_VAL(false));
        break;
      case OP_POP:
        pop(vm);
        break;
      case OP_GET_VAR: {
        ObjString* name = (ObjString*)AS_OBJ(READ_CONSTANT());
        Value value;
        if (!envGetByName(vm->env, name, &value)) {
          runtimeError(vm, currentToken(frame), "Undefined variable.");
          return false;
        }
        push(vm, value);
        break;
      }
      case OP_SET_VAR: {
        ObjString* name = (ObjString*)AS_OBJ(READ_CONSTANT());
        Value value = peek(vm, 0);
        if (!envAssignByName(vm->env, name, value)) {
          runtimeError(vm, currentToken(frame), "Undefined variable.");
          return false;
        }
        break;
      }
      case OP_DEFINE_VAR: {
        ObjString* name = (ObjString*)AS_OBJ(READ_CONSTANT());
        Value value = pop(vm);
        envDefine(vm->env, name, value);
        break;
      }
      case OP_GET_THIS: {
        ObjString* name = (ObjString*)AS_OBJ(READ_CONSTANT());
        Value value;
        if (!envGetByName(vm->env, name, &value)) {
          runtimeError(vm, currentToken(frame), "Cannot use 'this' outside of a class.");
          return false;
        }
        push(vm, value);
        break;
      }
      case OP_GET_PROPERTY: {
        ObjString* name = (ObjString*)AS_OBJ(READ_CONSTANT());
        Value object = pop(vm);
        if (isObjType(object, OBJ_INSTANCE)) {
          ObjInstance* instance = (ObjInstance*)AS_OBJ(object);
          ObjMap* fields = instance->fields;
          if (cache && cache->kind == IC_FIELD && cache->map == fields) {
            int index = cache->index;
            if (index >= 0 && index < fields->capacity &&
                fields->entries[index].key == name) {
              push(vm, fields->entries[index].value);
              break;
            }
          }

          Value value;
          int index = -1;
          if (mapGetIndex(fields, name, &value, &index)) {
            if (cache) {
              cache->kind = IC_FIELD;
              cache->map = fields;
              cache->key = name;
              cache->index = index;
              cache->klass = NULL;
              cache->method = NULL;
            }
            push(vm, value);
            break;
          }

          if (cache && cache->kind == IC_METHOD &&
              cache->klass == instance->klass &&
              cache->key == name && cache->method) {
            ObjBoundMethod* bound = newBoundMethod(vm, object, cache->method);
            push(vm, OBJ_VAL(bound));
            break;
          }

          ObjFunction* method = NULL;
          if (findMethodByName(instance->klass, name, &method)) {
            if (cache) {
              cache->kind = IC_METHOD;
              cache->klass = instance->klass;
              cache->key = name;
              cache->method = method;
              cache->map = NULL;
              cache->index = -1;
            }
            ObjBoundMethod* bound = newBoundMethod(vm, object, method);
            push(vm, OBJ_VAL(bound));
            break;
          }

          runtimeError(vm, currentToken(frame), "Undefined property.");
          return false;
        }
        runtimeError(vm, currentToken(frame), "Only instances have properties.");
        return false;
      }
      case OP_SET_PROPERTY: {
        ObjString* name = (ObjString*)AS_OBJ(READ_CONSTANT());
        Value value = pop(vm);
        Value object = pop(vm);
        if (!isObjType(object, OBJ_INSTANCE)) {
          runtimeError(vm, currentToken(frame), "Only instances have fields.");
          return false;
        }
        ObjInstance* instance = (ObjInstance*)AS_OBJ(object);
        int index = mapSetIndex(instance->fields, name, value);
        if (cache) {
          cache->kind = IC_FIELD;
          cache->map = instance->fields;
          cache->key = name;
          cache->index = index;
          cache->klass = NULL;
          cache->method = NULL;
        }
        push(vm, value);
        break;
      }
      case OP_GET_INDEX: {
        Value index = pop(vm);
        Value object = pop(vm);
        Value result = evaluateIndex(vm, currentToken(frame), object, index);
        if (vm->hadError) return false;
        push(vm, result);
        break;
      }
      case OP_SET_INDEX: {
        Value value = pop(vm);
        Value index = pop(vm);
        Value object = pop(vm);
        Value result = evaluateSetIndex(vm, currentToken(frame), object, index, value);
        if (vm->hadError) return false;
        push(vm, result);
        break;
      }
      case OP_EQUAL: {
        Value b = pop(vm);
        Value a = pop(vm);
        push(vm, BOOL_VAL(valuesEqual(a, b)));
        break;
      }
      case OP_GREATER: {
        Value b = pop(vm);
        Value a = pop(vm);
        Token token = currentToken(frame);
        if (!ensureNumberOperands(vm, token, a, b)) return false;
        push(vm, BOOL_VAL(AS_NUMBER(a) > AS_NUMBER(b)));
        break;
      }
      case OP_GREATER_EQUAL: {
        Value b = pop(vm);
        Value a = pop(vm);
        Token token = currentToken(frame);
        if (!ensureNumberOperands(vm, token, a, b)) return false;
        push(vm, BOOL_VAL(AS_NUMBER(a) >= AS_NUMBER(b)));
        break;
      }
      case OP_LESS: {
        Value b = pop(vm);
        Value a = pop(vm);
        Token token = currentToken(frame);
        if (!ensureNumberOperands(vm, token, a, b)) return false;
        push(vm, BOOL_VAL(AS_NUMBER(a) < AS_NUMBER(b)));
        break;
      }
      case OP_LESS_EQUAL: {
        Value b = pop(vm);
        Value a = pop(vm);
        Token token = currentToken(frame);
        if (!ensureNumberOperands(vm, token, a, b)) return false;
        push(vm, BOOL_VAL(AS_NUMBER(a) <= AS_NUMBER(b)));
        break;
      }
      case OP_ADD: {
        Value b = pop(vm);
        Value a = pop(vm);
        if (IS_NUMBER(a) && IS_NUMBER(b)) {
          push(vm, NUMBER_VAL(AS_NUMBER(a) + AS_NUMBER(b)));
          break;
        }
        if (isString(a) && isString(b)) {
          push(vm, concatenateStrings(vm, asString(a), asString(b)));
          break;
        }
        runtimeError(vm, currentToken(frame), "Operands must be two numbers or two strings.");
        return false;
      }
      case OP_SUBTRACT: {
        Value b = pop(vm);
        Value a = pop(vm);
        Token token = currentToken(frame);
        if (!ensureNumberOperands(vm, token, a, b)) return false;
        push(vm, NUMBER_VAL(AS_NUMBER(a) - AS_NUMBER(b)));
        break;
      }
      case OP_MULTIPLY: {
        Value b = pop(vm);
        Value a = pop(vm);
        Token token = currentToken(frame);
        if (!ensureNumberOperands(vm, token, a, b)) return false;
        push(vm, NUMBER_VAL(AS_NUMBER(a) * AS_NUMBER(b)));
        break;
      }
      case OP_DIVIDE: {
        Value b = pop(vm);
        Value a = pop(vm);
        Token token = currentToken(frame);
        if (!ensureNumberOperands(vm, token, a, b)) return false;
        push(vm, NUMBER_VAL(AS_NUMBER(a) / AS_NUMBER(b)));
        break;
      }
      case OP_NOT: {
        Value value = pop(vm);
        push(vm, BOOL_VAL(!isTruthy(value)));
        break;
      }
      case OP_NEGATE: {
        Value value = pop(vm);
        Token token = currentToken(frame);
        if (!ensureNumberOperand(vm, token, value)) return false;
        push(vm, NUMBER_VAL(-AS_NUMBER(value)));
        break;
      }
      case OP_JUMP: {
        uint16_t offset = READ_SHORT();
        frame->ip += offset;
        break;
      }
      case OP_JUMP_IF_FALSE: {
        uint16_t offset = READ_SHORT();
        if (!isTruthy(peek(vm, 0))) frame->ip += offset;
        break;
      }
      case OP_LOOP: {
        uint16_t offset = READ_SHORT();
        frame->ip -= offset;
        break;
      }
      case OP_CALL: {
        int argCount = READ_BYTE();
        Value callee = peek(vm, argCount);
        if (!callValue(vm, callee, argCount)) return false;
        frame = &vm->frames[vm->frameCount - 1];
        break;
      }
      case OP_CLOSURE: {
        ObjFunction* proto = (ObjFunction*)AS_OBJ(READ_CONSTANT());
        ObjFunction* function = cloneFunction(vm, proto, vm->env);
        push(vm, OBJ_VAL(function));
        break;
      }
      case OP_RETURN: {
        Value result = pop(vm);
        CallFrame* finished = frame;
        vm->frameCount--;
        vm->env = finished->previousEnv;
        vm->currentProgram = finished->previousProgram;
        if (finished->isModule && finished->moduleInstance && finished->moduleKey) {
          mapSet(vm->modules, finished->moduleKey, OBJ_VAL(finished->moduleInstance));
          if (finished->moduleHasAlias && finished->moduleAlias) {
            envDefine(vm->env, finished->moduleAlias, OBJ_VAL(finished->moduleInstance));
          }
        }
        if (finished->function->isInitializer) {
          result = finished->receiver;
        }
        vm->stackTop = finished->slots;
        if (!finished->discardResult) {
          push(vm, result);
        }
        if (vm->frameCount == 0) {
          if (!finished->discardResult) {
            pop(vm);
          }
          return true;
        }
        frame = &vm->frames[vm->frameCount - 1];
        break;
      }
      case OP_BEGIN_SCOPE:
        vm->env = newEnv(vm, vm->env);
        break;
      case OP_END_SCOPE:
        if (vm->env && vm->env->enclosing) {
          vm->env = vm->env->enclosing;
        }
        break;
      case OP_CLASS: {
        ObjString* name = (ObjString*)AS_OBJ(READ_CONSTANT());
        uint16_t methodCount = READ_SHORT();
        ObjMap* methods = newMap(vm);
        for (int i = 0; i < methodCount; i++) {
          Value methodValue = pop(vm);
          ObjFunction* method = (ObjFunction*)AS_OBJ(methodValue);
          mapSet(methods, method->name, methodValue);
        }
        ObjClass* klass = newClass(vm, name, methods);
        if (!envAssignByName(vm->env, name, OBJ_VAL(klass))) {
          envDefine(vm->env, name, OBJ_VAL(klass));
        }
        break;
      }
      case OP_IMPORT: {
        uint8_t hasAlias = READ_BYTE();
        uint16_t aliasIndex = READ_SHORT();
        ObjString* alias = NULL;
        if (hasAlias) {
          alias = (ObjString*)AS_OBJ(frame->function->chunk->constants[aliasIndex]);
        }

        Value pathValue = pop(vm);
        if (!isString(pathValue)) {
          runtimeError(vm, currentToken(frame), "Import path must be a string.");
          return false;
        }

        ObjString* pathString = asString(pathValue);
        char* resolvedPath = resolveImportPath(
            vm->currentProgram ? vm->currentProgram->path : NULL,
            pathString->chars);
        if (!resolvedPath) {
          runtimeError(vm, currentToken(frame), "Failed to resolve import path.");
          return false;
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
            if (hasAlias) {
              envDefine(vm->env, alias, cached);
            }
            if (candidatePath != resolvedPath) free(candidatePath);
            free(resolvedPath);
            break;
          }
          if (IS_BOOL(cached) && AS_BOOL(cached)) {
            runtimeError(vm, currentToken(frame), "Circular import detected.");
            if (candidatePath != resolvedPath) free(candidatePath);
            free(resolvedPath);
            return false;
          }
        }

        ObjString* key = copyStringWithLength(vm, candidatePath, pathToken.length);
        mapSet(vm->modules, key, BOOL_VAL(true));

        Env* moduleEnv = newEnv(vm, vm->globals);
        Env* previousEnv = vm->env;
        vm->env = moduleEnv;
        ObjFunction* moduleFunction = loadModuleFunction(vm, currentToken(frame), candidatePath);
        vm->env = previousEnv;

        if (!moduleFunction) {
          mapSet(vm->modules, key, NULL_VAL);
          if (candidatePath != resolvedPath) free(candidatePath);
          free(resolvedPath);
          return false;
        }

        ObjClass* klass = newClass(vm, moduleNameFromPath(vm, candidatePath), newMap(vm));
        ObjInstance* moduleInstance = newInstanceWithFields(vm, klass, moduleEnv->values);

        push(vm, OBJ_VAL(moduleFunction));
        if (vm->frameCount == FRAMES_MAX) {
          Token token;
          memset(&token, 0, sizeof(Token));
          runtimeError(vm, token, "Stack overflow.");
          return false;
        }

        CallFrame* moduleFrame = &vm->frames[vm->frameCount++];
        moduleFrame->function = moduleFunction;
        moduleFrame->ip = moduleFunction->chunk->code;
        moduleFrame->slots = vm->stackTop - 1;
        moduleFrame->previousEnv = previousEnv;
        moduleFrame->previousProgram = vm->currentProgram;
        moduleFrame->receiver = NULL_VAL;
        moduleFrame->isModule = true;
        moduleFrame->discardResult = true;
        moduleFrame->moduleInstance = moduleInstance;
        moduleFrame->moduleAlias = alias;
        moduleFrame->moduleKey = key;
        moduleFrame->moduleHasAlias = hasAlias != 0;

        vm->env = moduleEnv;
        vm->currentProgram = moduleFunction->program;

        if (candidatePath != resolvedPath) free(candidatePath);
        free(resolvedPath);

        frame = &vm->frames[vm->frameCount - 1];
        break;
      }
      case OP_ARRAY: {
        uint16_t capacity = READ_SHORT();
        ObjArray* array = newArrayWithCapacity(vm, (int)capacity);
        push(vm, OBJ_VAL(array));
        break;
      }
      case OP_ARRAY_APPEND: {
        Value value = pop(vm);
        ObjArray* array = (ObjArray*)AS_OBJ(peek(vm, 0));
        arrayWrite(array, value);
        break;
      }
      case OP_MAP: {
        uint16_t capacity = READ_SHORT();
        ObjMap* map = newMapWithCapacity(vm, (int)capacity);
        push(vm, OBJ_VAL(map));
        break;
      }
      case OP_MAP_SET: {
        Value value = pop(vm);
        Value key = pop(vm);
        if (!isString(key)) {
          runtimeError(vm, currentToken(frame), "Map keys must be strings.");
          return false;
        }
        ObjMap* map = (ObjMap*)AS_OBJ(peek(vm, 0));
        mapSet(map, asString(key), value);
        break;
      }
      case OP_GC:
        gcMaybe(vm);
        break;
    }
  }

#undef READ_BYTE
#undef READ_SHORT
#undef READ_CONSTANT
}

static bool callScript(VM* vm, ObjFunction* function) {
  if (vm->frameCount == FRAMES_MAX) {
    Token token;
    memset(&token, 0, sizeof(Token));
    runtimeError(vm, token, "Stack overflow.");
    return false;
  }

  CallFrame* frame = &vm->frames[vm->frameCount++];
  frame->function = function;
  frame->ip = function->chunk->code;
  frame->slots = vm->stackTop - 1;
  frame->previousEnv = vm->env;
  frame->previousProgram = vm->currentProgram;
  frame->receiver = NULL_VAL;
  frame->isModule = false;
  frame->discardResult = false;
  frame->moduleInstance = NULL;
  frame->moduleAlias = NULL;
  frame->moduleKey = NULL;
  frame->moduleHasAlias = false;

  vm->currentProgram = function->program;
  return true;
}

bool interpret(VM* vm, Program* program) {
  vm->hadError = false;
  Program* previousProgram = vm->currentProgram;
  vm->currentProgram = program;
  programRunBegin(program);

  ObjFunction* function = compileProgram(vm, program);
  if (!function) {
    programRunEnd(vm, program);
    vm->currentProgram = previousProgram;
    return false;
  }

  resetStack(vm);
  push(vm, OBJ_VAL(function));
  if (!callScript(vm, function)) {
    programRunEnd(vm, program);
    vm->currentProgram = previousProgram;
    return false;
  }

  bool ok = run(vm);
  programRunEnd(vm, program);
  vm->currentProgram = previousProgram;
  return ok && !vm->hadError;
}

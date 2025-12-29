#include "interpreter_internal.h"
#include "singlepass.h"
#include "chunk.h"
#include "disasm.h"
#include "gc.h"
#include "program.h"
#include "diagnostics.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void resetStack(VM* vm) {
  vm->stackTop = vm->stack;
  vm->frameCount = 0;
  vm->tryCount = 0;
}

static void push(VM* vm, Value value) {
  *vm->stackTop = value;
  vm->stackTop++;
}

static Value pop(VM* vm) {
  vm->stackTop--;
  return *vm->stackTop;
}

static ObjString* stringifyValue(VM* vm, Value value);

static Value peek(VM* vm, int distance) {
  return vm->stackTop[-1 - distance];
}

static void popTryFramesForFrame(VM* vm, int frameIndex) {
  while (vm->tryCount > 0 &&
         vm->tryFrames[vm->tryCount - 1].frameIndex >= frameIndex) {
    vm->tryCount--;
  }
}

static bool isErrorValue(VM* vm, Value value) {
  if (!isObjType(value, OBJ_MAP)) return false;
  ObjMap* map = (ObjMap*)AS_OBJ(value);
  ObjString* key = copyString(vm, "_error");
  Value flag;
  if (!mapGet(map, key, &flag)) return false;
  return IS_BOOL(flag) && AS_BOOL(flag);
}

static ObjString* errorMessageForValue(VM* vm, Value value) {
  if (isObjType(value, OBJ_MAP)) {
    ObjMap* map = (ObjMap*)AS_OBJ(value);
    ObjString* key = copyString(vm, "message");
    Value message;
    if (mapGet(map, key, &message) && isString(message)) {
      return asString(message);
    }
  }
  if (isString(value)) {
    return asString(value);
  }
  return stringifyValue(vm, value);
}

static Value wrapErrorValue(VM* vm, Value value) {
  if (isErrorValue(vm, value)) {
    return value;
  }
  ObjMap* map = newMap(vm);
  ObjString* errorKey = copyString(vm, "_error");
  ObjString* messageKey = copyString(vm, "message");
  ObjString* valueKey = copyString(vm, "value");
  ObjString* traceKey = copyString(vm, "trace");
  mapSet(map, errorKey, BOOL_VAL(true));
  mapSet(map, valueKey, value);
  ObjString* message = errorMessageForValue(vm, value);
  mapSet(map, messageKey, OBJ_VAL(message));
  const char* displayPath = "<repl>";
  if (vm->currentProgram && vm->currentProgram->path) {
    displayPath = vm->currentProgram->path;
  }
  ObjArray* trace = captureStackTrace(vm, displayPath);
  mapSet(map, traceKey, OBJ_VAL(trace));
  return OBJ_VAL(map);
}

static bool unwindToHandler(VM* vm, CallFrame** frame, Value error) {
  while (vm->tryCount > 0) {
    TryFrame handler = vm->tryFrames[vm->tryCount - 1];
    if (handler.frameIndex < 0 || handler.frameIndex >= vm->frameCount) {
      vm->tryCount--;
      continue;
    }
    vm->tryCount--;
    vm->frameCount = handler.frameIndex + 1;
    vm->env = handler.env;
    vm->stackTop = handler.stackTop;
    *frame = &vm->frames[handler.frameIndex];
    vm->currentProgram = (*frame)->function->program;
    push(vm, error);
    (*frame)->ip = handler.handler;
    return true;
  }
  return false;
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

static void debugTraceInstruction(VM* vm, CallFrame* frame, uint8_t instruction) {
  if (!vm->debugTrace || !frame || !frame->function || !frame->function->chunk) return;
  Token token = currentToken(frame);
  if (token.line <= 0 || token.column <= 0) return;
  if (token.line == vm->debugTraceLine && token.column == vm->debugTraceColumn) return;
  vm->debugTraceLine = token.line;
  vm->debugTraceColumn = token.column;
  const char* path = "<repl>";
  if (frame->function->program && frame->function->program->path) {
    path = frame->function->program->path;
  }
  fprintf(stderr, "TRACE %s:%d:%d op=%u\n", path, token.line, token.column,
          (unsigned)instruction);
}

static bool updateBestSuggestionFromMap(ObjMap* map, const char* target, int targetLen,
                                        ObjString** best, int* bestDist) {
  if (!map || !target || targetLen <= 0) return false;
  for (int i = 0; i < map->capacity; i++) {
    ObjString* key = map->entries[i].key;
    if (!key) continue;
    int maxDist = *bestDist - 1;
    if (maxDist < 0) return true;
    if (maxDist > ERKAO_DIAG_MAX_DISTANCE) maxDist = ERKAO_DIAG_MAX_DISTANCE;
    int dist = diag_edit_distance_limited(target, targetLen, key->chars, key->length,
                                          maxDist);
    if (dist < *bestDist) {
      *bestDist = dist;
      *best = key;
      if (dist == 0) return true;
    }
  }
  return *best != NULL;
}

static bool suggestNameFromEnv(Env* env, const char* target, int targetLen,
                               char* out, size_t outSize) {
  if (!env || !out || outSize == 0) return false;
  ObjString* best = NULL;
  int bestDist = ERKAO_DIAG_MAX_DISTANCE + 1;
  for (Env* current = env; current != NULL; current = current->enclosing) {
    updateBestSuggestionFromMap(current->values, target, targetLen, &best, &bestDist);
  }
  if (!best || bestDist > ERKAO_DIAG_MAX_DISTANCE) return false;
  snprintf(out, outSize, "%.*s", best->length, best->chars);
  return true;
}

static bool suggestNameFromInstance(ObjInstance* instance, const char* target, int targetLen,
                                    char* out, size_t outSize) {
  if (!instance || !out || outSize == 0) return false;
  ObjString* best = NULL;
  int bestDist = ERKAO_DIAG_MAX_DISTANCE + 1;
  updateBestSuggestionFromMap(instance->fields, target, targetLen, &best, &bestDist);
  updateBestSuggestionFromMap(instance->klass ? instance->klass->methods : NULL,
                              target, targetLen, &best, &bestDist);
  if (!best || bestDist > ERKAO_DIAG_MAX_DISTANCE) return false;
  snprintf(out, outSize, "%.*s", best->length, best->chars);
  return true;
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

typedef struct {
  char* data;
  int length;
  int capacity;
} StringBuilder;

static void sbInit(StringBuilder* sb) {
  sb->data = NULL;
  sb->length = 0;
  sb->capacity = 0;
}

static void sbEnsure(StringBuilder* sb, int needed) {
  if (sb->capacity >= needed) return;
  int newCap = sb->capacity == 0 ? 64 : sb->capacity;
  while (newCap < needed) {
    newCap *= 2;
  }
  char* next = (char*)realloc(sb->data, (size_t)newCap);
  if (!next) {
    fprintf(stderr, "Out of memory.\n");
    exit(1);
  }
  sb->data = next;
  sb->capacity = newCap;
}

static void sbAppendN(StringBuilder* sb, const char* text, int length) {
  if (length <= 0) return;
  sbEnsure(sb, sb->length + length + 1);
  memcpy(sb->data + sb->length, text, (size_t)length);
  sb->length += length;
  sb->data[sb->length] = '\0';
}

static void sbAppendChar(StringBuilder* sb, char c) {
  sbEnsure(sb, sb->length + 2);
  sb->data[sb->length++] = c;
  sb->data[sb->length] = '\0';
}

static void appendValue(StringBuilder* sb, Value value);

static void appendArray(StringBuilder* sb, ObjArray* array) {
  sbAppendChar(sb, '[');
  for (int i = 0; i < array->count; i++) {
    if (i > 0) sbAppendN(sb, ", ", 2);
    appendValue(sb, array->items[i]);
  }
  sbAppendChar(sb, ']');
}

static void appendMap(StringBuilder* sb, ObjMap* map) {
  sbAppendChar(sb, '{');
  int printed = 0;
  for (int i = 0; i < map->capacity; i++) {
    if (!map->entries[i].key) continue;
    if (printed > 0) sbAppendN(sb, ", ", 2);
    sbAppendN(sb, map->entries[i].key->chars, map->entries[i].key->length);
    sbAppendN(sb, ": ", 2);
    appendValue(sb, map->entries[i].value);
    printed++;
  }
  sbAppendChar(sb, '}');
}

static void appendObject(StringBuilder* sb, Obj* obj) {
  switch (obj->type) {
    case OBJ_STRING: {
      ObjString* string = (ObjString*)obj;
      sbAppendN(sb, string->chars, string->length);
      break;
    }
    case OBJ_FUNCTION: {
      ObjFunction* function = (ObjFunction*)obj;
      if (function->name && function->name->chars) {
        sbAppendN(sb, "<fun ", 5);
        sbAppendN(sb, function->name->chars, function->name->length);
        sbAppendChar(sb, '>');
      } else {
        sbAppendN(sb, "<fun>", 5);
      }
      break;
    }
    case OBJ_NATIVE: {
      ObjNative* native = (ObjNative*)obj;
      if (native->name && native->name->chars) {
        sbAppendN(sb, "<native ", 8);
        sbAppendN(sb, native->name->chars, native->name->length);
        sbAppendChar(sb, '>');
      } else {
        sbAppendN(sb, "<native>", 8);
      }
      break;
    }
    case OBJ_ENUM_CTOR: {
      ObjEnumCtor* ctor = (ObjEnumCtor*)obj;
      sbAppendN(sb, "<enum ", 6);
      if (ctor->enumName && ctor->enumName->chars) {
        sbAppendN(sb, ctor->enumName->chars, ctor->enumName->length);
      } else {
        sbAppendN(sb, "enum", 4);
      }
      sbAppendChar(sb, '.');
      if (ctor->variantName && ctor->variantName->chars) {
        sbAppendN(sb, ctor->variantName->chars, ctor->variantName->length);
      } else {
        sbAppendN(sb, "variant", 7);
      }
      sbAppendChar(sb, '>');
      break;
    }
    case OBJ_CLASS: {
      ObjClass* klass = (ObjClass*)obj;
      sbAppendN(sb, "<class ", 7);
      sbAppendN(sb, klass->name->chars, klass->name->length);
      sbAppendChar(sb, '>');
      break;
    }
    case OBJ_INSTANCE: {
      ObjInstance* instance = (ObjInstance*)obj;
      sbAppendChar(sb, '<');
      sbAppendN(sb, instance->klass->name->chars, instance->klass->name->length);
      sbAppendN(sb, " instance>", 10);
      break;
    }
    case OBJ_ARRAY:
      appendArray(sb, (ObjArray*)obj);
      break;
    case OBJ_MAP:
      appendMap(sb, (ObjMap*)obj);
      break;
    case OBJ_BOUND_METHOD:
      sbAppendN(sb, "<bound method>", 14);
      break;
  }
}

static void appendValue(StringBuilder* sb, Value value) {
  switch (value.type) {
    case VAL_NULL:
      sbAppendN(sb, "null", 4);
      break;
    case VAL_BOOL:
      if (AS_BOOL(value)) {
        sbAppendN(sb, "true", 4);
      } else {
        sbAppendN(sb, "false", 5);
      }
      break;
    case VAL_NUMBER: {
      char buffer[64];
      int length = snprintf(buffer, sizeof(buffer), "%g", AS_NUMBER(value));
      if (length < 0) length = 0;
      if (length >= (int)sizeof(buffer)) {
        length = (int)sizeof(buffer) - 1;
      }
      sbAppendN(sb, buffer, length);
      break;
    }
    case VAL_OBJ:
      appendObject(sb, AS_OBJ(value));
      break;
  }
}

static ObjString* stringifyValue(VM* vm, Value value) {
  StringBuilder sb;
  sbInit(&sb);
  appendValue(&sb, value);
  return takeStringWithLength(vm, sb.data, sb.length);
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

static bool beginModuleImport(VM* vm, CallFrame** frame, ObjString* pathString,
                              ObjString* alias, bool hasAlias, bool pushResult) {
  char* resolvedPath = resolveImportPath(
      vm, vm->currentProgram ? vm->currentProgram->path : NULL,
      pathString->chars);
  if (!resolvedPath) {
    runtimeError(vm, currentToken(*frame), "Failed to resolve import path.");
    return false;
  }

  Token pathToken;
  memset(&pathToken, 0, sizeof(Token));
  pathToken.start = resolvedPath;
  pathToken.length = (int)strlen(resolvedPath);

  Value cached;
  if (mapGetByToken(vm->modules, pathToken, &cached)) {
    if (IS_OBJ(cached)) {
      if (pushResult) {
        push(vm, cached);
      }
      if (hasAlias && alias) {
        envDefine(vm->env, alias, cached);
      }
      free(resolvedPath);
      return true;
    }
  }

  ObjString* key = copyStringWithLength(vm, resolvedPath, pathToken.length);

  Env* moduleEnv = newEnv(vm, vm->globals);
  Env* previousEnv = vm->env;
  vm->env = moduleEnv;
  ObjFunction* moduleFunction =
      loadModuleFunction(vm, currentToken(*frame), resolvedPath);
  vm->env = previousEnv;

  if (!moduleFunction) {
    mapSet(vm->modules, key, NULL_VAL);
    free(resolvedPath);
    return false;
  }

  ObjClass* klass = newClass(vm, moduleNameFromPath(vm, resolvedPath), newMap(vm));
  ObjInstance* moduleInstance = newInstanceWithFields(vm, klass, newMap(vm));
  mapSet(vm->modules, key, OBJ_VAL(moduleInstance));

  push(vm, OBJ_VAL(moduleFunction));
  if (vm->frameCount == vm->maxFrames) {
    Token token;
    memset(&token, 0, sizeof(Token));
    runtimeError(vm, token, "Stack overflow.");
    free(resolvedPath);
    return false;
  }

  CallFrame* moduleFrame = &vm->frames[vm->frameCount++];
  moduleFrame->function = moduleFunction;
  moduleFrame->ip = moduleFunction->chunk->code;
  moduleFrame->slots = vm->stackTop - 1;
  moduleFrame->previousEnv = previousEnv;
  moduleFrame->previousProgram = vm->currentProgram;
  moduleFrame->receiver = NULL_VAL;
  moduleFrame->argCount = 0;
  moduleFrame->isModule = true;
  moduleFrame->discardResult = !pushResult;
  moduleFrame->moduleInstance = moduleInstance;
  moduleFrame->moduleAlias = alias;
  moduleFrame->moduleKey = key;
  moduleFrame->moduleHasAlias = hasAlias;
  moduleFrame->modulePushResult = pushResult;
  moduleFrame->modulePrivate = NULL;

  vm->env = moduleEnv;
  vm->currentProgram = moduleFunction->program;

  free(resolvedPath);
  *frame = moduleFrame;
  return true;
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
  if (argc < function->minArity || argc > function->arity) {
    Token token;
    memset(&token, 0, sizeof(Token));
    runtimeError(vm, token, "Wrong number of arguments.");
    return false;
  }

  if (vm->frameCount == vm->maxFrames) {
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
  frame->argCount = argc;
  frame->isModule = false;
  frame->discardResult = false;
  frame->moduleInstance = NULL;
  frame->moduleAlias = NULL;
  frame->moduleKey = NULL;
  frame->moduleHasAlias = false;
  frame->modulePushResult = false;
  frame->modulePrivate = NULL;

  Env* env = newEnv(vm, function->closure);
  if (hasReceiver) {
    ObjString* thisName = copyString(vm, "this");
    envDefine(env, thisName, receiver);
  }
  for (int i = 0; i < function->arity; i++) {
    Value arg = i < argc ? frame->slots[i + 1] : NULL_VAL;
    envDefine(env, function->params[i], arg);
  }

  vm->env = env;
  vm->currentProgram = function->program;
  return true;
}

static bool moduleNameIsPrivate(ObjMap* privateMap, ObjString* name) {
  if (!privateMap || !name) return false;
  Value ignored;
  return mapGet(privateMap, name, &ignored);
}

static ObjMap* buildPublicExports(VM* vm, ObjMap* source, ObjMap* privateMap) {
  if (!source || !privateMap || mapCount(privateMap) == 0) {
    return source;
  }
  ObjMap* filtered = newMap(vm);
  for (int i = 0; i < source->capacity; i++) {
    MapEntryValue* entry = &source->entries[i];
    if (!entry->key) continue;
    if (moduleNameIsPrivate(privateMap, entry->key)) continue;
    mapSet(filtered, entry->key, entry->value);
  }
  return filtered;
}

static bool returnFromFrame(VM* vm, CallFrame** frame, Value result, int targetFrameCount) {
  CallFrame* finished = *frame;
  Env* finishedEnv = vm->env;
  int finishedIndex = vm->frameCount - 1;
  popTryFramesForFrame(vm, finishedIndex);
  vm->frameCount--;
  vm->env = finished->previousEnv;
  vm->currentProgram = finished->previousProgram;
  if (finished->isModule && finished->moduleInstance && finished->moduleKey) {
    if (finishedEnv && mapCount(finished->moduleInstance->fields) == 0) {
      ObjMap* exports = buildPublicExports(vm, finishedEnv->values, finished->modulePrivate);
      finished->moduleInstance->fields = exports;
      gcWriteBarrier(vm, (Obj*)finished->moduleInstance, OBJ_VAL(exports));
    }
    mapSet(vm->modules, finished->moduleKey, OBJ_VAL(finished->moduleInstance));
    if (finished->moduleHasAlias && finished->moduleAlias) {
      envDefine(vm->env, finished->moduleAlias, OBJ_VAL(finished->moduleInstance));
    }
  }
  if (finished->isModule && finished->modulePushResult && finished->moduleInstance) {
    result = OBJ_VAL(finished->moduleInstance);
  }
  if (finished->function->isInitializer) {
    result = finished->receiver;
  }
  vm->stackTop = finished->slots;
  if (!finished->discardResult) {
    push(vm, result);
  }
  if (vm->frameCount <= targetFrameCount) {
    if (targetFrameCount == 0 && !finished->discardResult) {
      pop(vm);
    }
    return true;
  }
  *frame = &vm->frames[vm->frameCount - 1];
  return false;
}

static bool enumValueMatches(VM* vm, Value value, ObjString* enumName, ObjString* variantName) {
  if (!isObjType(value, OBJ_MAP)) return false;
  ObjMap* map = (ObjMap*)AS_OBJ(value);
  ObjString* enumKey = copyString(vm, "_enum");
  ObjString* tagKey = copyString(vm, "_tag");
  Value enumValue;
  if (!mapGet(map, enumKey, &enumValue) || !isObjType(enumValue, OBJ_STRING)) {
    return false;
  }
  if (!valuesEqual(enumValue, OBJ_VAL(enumName))) {
    return false;
  }
  Value tagValue;
  if (!mapGet(map, tagKey, &tagValue) || !isObjType(tagValue, OBJ_STRING)) {
    return false;
  }
  if (!valuesEqual(tagValue, OBJ_VAL(variantName))) {
    return false;
  }
  return true;
}

static bool enumUnwrap(VM* vm, ObjMap* map, const char* enumName,
                       const char* okTag, const char* errTag,
                       Value* out, bool* shouldReturn, bool* matched) {
  *matched = false;
  *shouldReturn = false;
  ObjString* enumKey = copyString(vm, "_enum");
  ObjString* tagKey = copyString(vm, "_tag");
  Value enumValue;
  if (!mapGet(map, enumKey, &enumValue) || !isObjType(enumValue, OBJ_STRING)) {
    return true;
  }
  ObjString* enumStr = (ObjString*)AS_OBJ(enumValue);
  if (strcmp(enumStr->chars, enumName) != 0) {
    return true;
  }
  *matched = true;
  Value tagValue;
  if (!mapGet(map, tagKey, &tagValue) || !isObjType(tagValue, OBJ_STRING)) {
    return false;
  }
  ObjString* tagStr = (ObjString*)AS_OBJ(tagValue);
  if (strcmp(tagStr->chars, errTag) == 0) {
    *shouldReturn = true;
    *out = OBJ_VAL(map);
    return true;
  }
  if (strcmp(tagStr->chars, okTag) == 0) {
    ObjString* valuesKey = copyString(vm, "_values");
    Value valuesValue;
    if (!mapGet(map, valuesKey, &valuesValue) || !isObjType(valuesValue, OBJ_ARRAY)) {
      return false;
    }
    ObjArray* values = (ObjArray*)AS_OBJ(valuesValue);
    if (values->count <= 0) {
      *out = NULL_VAL;
    } else {
      *out = values->items[0];
    }
    return true;
  }
  return false;
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

  if (isObjType(callee, OBJ_ENUM_CTOR)) {
    ObjEnumCtor* ctor = (ObjEnumCtor*)AS_OBJ(callee);
    if (ctor->arity >= 0 && argc != ctor->arity) {
      Token token;
      memset(&token, 0, sizeof(Token));
      runtimeError(vm, token, "Wrong number of arguments.");
      return false;
    }
    ObjMap* variant = newEnumVariant(vm, ctor->enumName, ctor->variantName, argc,
                                     vm->stackTop - argc);
    vm->stackTop -= argc + 1;
    push(vm, OBJ_VAL(variant));
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

static bool run(VM* vm);

static bool runWithTarget(VM* vm, int targetFrameCount);

bool vmCallValue(VM* vm, Value callee, int argc, Value* args, Value* out) {
  if (isObjType(callee, OBJ_NATIVE)) {
    ObjNative* native = (ObjNative*)AS_OBJ(callee);
    if (native->arity >= 0 && argc != native->arity) {
      Token token;
      memset(&token, 0, sizeof(Token));
      runtimeError(vm, token, "Wrong number of arguments.");
      return false;
    }
    Value result = native->function(vm, argc, args);
    if (vm->hadError) return false;
    *out = result;
    return true;
  }

  if (isObjType(callee, OBJ_ENUM_CTOR)) {
    ObjEnumCtor* ctor = (ObjEnumCtor*)AS_OBJ(callee);
    if (ctor->arity >= 0 && argc != ctor->arity) {
      Token token;
      memset(&token, 0, sizeof(Token));
      runtimeError(vm, token, "Wrong number of arguments.");
      return false;
    }
    ObjMap* variant = newEnumVariant(vm, ctor->enumName, ctor->variantName, argc, args);
    *out = OBJ_VAL(variant);
    return true;
  }

  int savedFrameCount = vm->frameCount;
  Value* savedStackTop = vm->stackTop;
  Env* savedEnv = vm->env;
  Program* savedProgram = vm->currentProgram;

  *vm->stackTop++ = callee;

  for (int i = 0; i < argc; i++) {
    *vm->stackTop++ = args[i];
  }

  if (!callValue(vm, callee, argc)) {
    vm->stackTop = savedStackTop;
    vm->env = savedEnv;
    vm->currentProgram = savedProgram;
    return false;
  }

  if (!runWithTarget(vm, savedFrameCount)) {
    vm->frameCount = savedFrameCount;
    vm->stackTop = savedStackTop;
    vm->env = savedEnv;
    vm->currentProgram = savedProgram;
    return false;
  }

  if (vm->stackTop > savedStackTop) {
    *out = *(vm->stackTop - 1);
    vm->stackTop = savedStackTop;
  } else {
    *out = NULL_VAL;
  }

  vm->env = savedEnv;
  vm->currentProgram = savedProgram;

  return true;
}

static bool run(VM* vm) {
  return runWithTarget(vm, 0);
}

static inline uint16_t readShort(CallFrame* frame) {
  frame->ip += 2;
  return (uint16_t)((frame->ip[-2] << 8) | frame->ip[-1]);
}

static inline Value readConstant(CallFrame* frame) {
  return frame->function->chunk->constants[readShort(frame)];
}

static bool runWithTarget(VM* vm, int targetFrameCount) {
  CallFrame* frame = &vm->frames[vm->frameCount - 1];

#define READ_BYTE() (*frame->ip++)
#define READ_SHORT() readShort(frame)
#define READ_CONSTANT() readConstant(frame)

  for (;;) {
    uint8_t instruction = READ_BYTE();
    debugTraceInstruction(vm, frame, instruction);
    vm->instructionCount++;
    if (vm->instructionBudget > 0 && vm->instructionCount > vm->instructionBudget) {
      runtimeError(vm, currentToken(frame), "Instruction budget exceeded.");
      return false;
    }
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
          char suggestion[64];
          char message[256];
          if (suggestNameFromEnv(vm->env, name->chars, name->length,
                                 suggestion, sizeof(suggestion))) {
            snprintf(message, sizeof(message),
                     "Undefined variable. Did you mean '%s'?", suggestion);
            runtimeError(vm, currentToken(frame), message);
          } else {
            runtimeError(vm, currentToken(frame), "Undefined variable.");
          }
          return false;
        }
        push(vm, value);
        break;
      }
      case OP_SET_VAR: {
        ObjString* name = (ObjString*)AS_OBJ(READ_CONSTANT());
        Value value = peek(vm, 0);
        if (envIsConst(vm->env, name)) {
          runtimeError(vm, currentToken(frame), "Cannot assign to const variable.");
          return false;
        }
        if (!envAssignByName(vm->env, name, value)) {
          char suggestion[64];
          char message[256];
          if (suggestNameFromEnv(vm->env, name->chars, name->length,
                                 suggestion, sizeof(suggestion))) {
            snprintf(message, sizeof(message),
                     "Undefined variable. Did you mean '%s'?", suggestion);
            runtimeError(vm, currentToken(frame), message);
          } else {
            runtimeError(vm, currentToken(frame), "Undefined variable.");
          }
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
      case OP_DEFINE_CONST: {
        ObjString* name = (ObjString*)AS_OBJ(READ_CONSTANT());
        Value value = pop(vm);
        envDefineConst(vm->env, name, value);
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

          {
            char suggestion[64];
            char message[256];
            if (suggestNameFromInstance(instance, name->chars, name->length,
                                        suggestion, sizeof(suggestion))) {
              snprintf(message, sizeof(message),
                       "Undefined property. Did you mean '%s'?", suggestion);
              runtimeError(vm, currentToken(frame), message);
            } else {
              runtimeError(vm, currentToken(frame), "Undefined property.");
            }
          }
          return false;
        }
        if (isObjType(object, OBJ_MAP)) {
          ObjMap* map = (ObjMap*)AS_OBJ(object);
          if (cache && cache->kind == IC_MAP && cache->map == map) {
            int entryIndex = cache->index;
            if (entryIndex >= 0 && entryIndex < map->capacity &&
                map->entries[entryIndex].key == name) {
              push(vm, map->entries[entryIndex].value);
              break;
            }
          }

          Value out;
          int entryIndex = -1;
          if (mapGetIndex(map, name, &out, &entryIndex)) {
            if (cache) {
              cache->kind = IC_MAP;
              cache->map = map;
              cache->key = name;
              cache->index = entryIndex;
              cache->klass = NULL;
              cache->method = NULL;
            }
            push(vm, out);
          } else {
            push(vm, NULL_VAL);
          }
          break;
        }
        runtimeError(vm, currentToken(frame), "Only instances have properties.");
        return false;
      }
      case OP_GET_PROPERTY_OPTIONAL: {
        ObjString* name = (ObjString*)AS_OBJ(READ_CONSTANT());
        Value object = pop(vm);
        if (IS_NULL(object)) {
          push(vm, NULL_VAL);
          break;
        }
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

          {
            char suggestion[64];
            char message[256];
            if (suggestNameFromInstance(instance, name->chars, name->length,
                                        suggestion, sizeof(suggestion))) {
              snprintf(message, sizeof(message),
                       "Undefined property. Did you mean '%s'?", suggestion);
              runtimeError(vm, currentToken(frame), message);
            } else {
              runtimeError(vm, currentToken(frame), "Undefined property.");
            }
          }
          return false;
        }
        if (isObjType(object, OBJ_MAP)) {
          ObjMap* map = (ObjMap*)AS_OBJ(object);
          if (cache && cache->kind == IC_MAP && cache->map == map) {
            int entryIndex = cache->index;
            if (entryIndex >= 0 && entryIndex < map->capacity &&
                map->entries[entryIndex].key == name) {
              push(vm, map->entries[entryIndex].value);
              break;
            }
          }

          Value out;
          int entryIndex = -1;
          if (mapGetIndex(map, name, &out, &entryIndex)) {
            if (cache) {
              cache->kind = IC_MAP;
              cache->map = map;
              cache->key = name;
              cache->index = entryIndex;
              cache->klass = NULL;
              cache->method = NULL;
            }
            push(vm, out);
          } else {
            push(vm, NULL_VAL);
          }
          break;
        }
        runtimeError(vm, currentToken(frame), "Only instances have properties.");
        return false;
      }
      case OP_SET_PROPERTY: {
        ObjString* name = (ObjString*)AS_OBJ(READ_CONSTANT());
        Value value = pop(vm);
        Value object = pop(vm);
        if (isObjType(object, OBJ_INSTANCE)) {
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
        if (isObjType(object, OBJ_MAP)) {
          ObjMap* map = (ObjMap*)AS_OBJ(object);
          int entryIndex = mapSetIndex(map, name, value);
          if (cache) {
            cache->kind = IC_MAP;
            cache->map = map;
            cache->key = name;
            cache->index = entryIndex;
            cache->klass = NULL;
            cache->method = NULL;
          }
          push(vm, value);
          break;
        }
        runtimeError(vm, currentToken(frame), "Only instances have fields.");
        return false;
      }
      case OP_GET_INDEX: {
        Value index = pop(vm);
        Value object = pop(vm);
        if (isObjType(object, OBJ_MAP) && isString(index)) {
          ObjMap* map = (ObjMap*)AS_OBJ(object);
          ObjString* key = asString(index);
          if (cache && cache->kind == IC_MAP && cache->map == map) {
            int entryIndex = cache->index;
            if (entryIndex >= 0 && entryIndex < map->capacity &&
                map->entries[entryIndex].key == key) {
              push(vm, map->entries[entryIndex].value);
              break;
            }
          }

          Value out;
          int entryIndex = -1;
          if (mapGetIndex(map, key, &out, &entryIndex)) {
            if (cache) {
              cache->kind = IC_MAP;
              cache->map = map;
              cache->key = key;
              cache->index = entryIndex;
              cache->klass = NULL;
              cache->method = NULL;
            }
            push(vm, out);
          } else {
            push(vm, NULL_VAL);
          }
          break;
        }
        Value result = evaluateIndex(vm, currentToken(frame), object, index);
        if (vm->hadError) return false;
        push(vm, result);
        break;
      }
      case OP_GET_INDEX_OPTIONAL: {
        Value index = pop(vm);
        Value object = pop(vm);
        if (IS_NULL(object)) {
          push(vm, NULL_VAL);
          break;
        }
        if (isObjType(object, OBJ_MAP) && isString(index)) {
          ObjMap* map = (ObjMap*)AS_OBJ(object);
          ObjString* key = asString(index);
          if (cache && cache->kind == IC_MAP && cache->map == map) {
            int entryIndex = cache->index;
            if (entryIndex >= 0 && entryIndex < map->capacity &&
                map->entries[entryIndex].key == key) {
              push(vm, map->entries[entryIndex].value);
              break;
            }
          }

          Value out;
          int entryIndex = -1;
          if (mapGetIndex(map, key, &out, &entryIndex)) {
            if (cache) {
              cache->kind = IC_MAP;
              cache->map = map;
              cache->key = key;
              cache->index = entryIndex;
              cache->klass = NULL;
              cache->method = NULL;
            }
            push(vm, out);
          } else {
            push(vm, NULL_VAL);
          }
          break;
        }
        Value result = evaluateIndex(vm, currentToken(frame), object, index);
        if (vm->hadError) return false;
        push(vm, result);
        break;
      }
      case OP_SET_INDEX: {
        Value value = pop(vm);
        Value index = pop(vm);
        Value object = pop(vm);
        if (isObjType(object, OBJ_MAP) && isString(index)) {
          ObjMap* map = (ObjMap*)AS_OBJ(object);
          ObjString* key = asString(index);
          if (cache && cache->kind == IC_MAP && cache->map == map) {
            int entryIndex = cache->index;
            if (entryIndex >= 0 && entryIndex < map->capacity &&
                map->entries[entryIndex].key == key) {
              map->entries[entryIndex].value = value;
              gcWriteBarrier(vm, (Obj*)map, value);
              push(vm, value);
              break;
            }
          }

          int entryIndex = mapSetIndex(map, key, value);
          if (cache) {
            cache->kind = IC_MAP;
            cache->map = map;
            cache->key = key;
            cache->index = entryIndex;
            cache->klass = NULL;
            cache->method = NULL;
          }
          push(vm, value);
          break;
        }
        Value result = evaluateSetIndex(vm, currentToken(frame), object, index, value);
        if (vm->hadError) return false;
        push(vm, result);
        break;
      }
      case OP_MATCH_ENUM: {
        ObjString* enumName = (ObjString*)AS_OBJ(READ_CONSTANT());
        ObjString* variantName = (ObjString*)AS_OBJ(READ_CONSTANT());
        Value value = pop(vm);
        push(vm, BOOL_VAL(enumValueMatches(vm, value, enumName, variantName)));
        break;
      }
      case OP_IS_ARRAY: {
        Value value = pop(vm);
        push(vm, BOOL_VAL(isObjType(value, OBJ_ARRAY)));
        break;
      }
      case OP_IS_MAP: {
        Value value = pop(vm);
        push(vm, BOOL_VAL(isObjType(value, OBJ_MAP)));
        break;
      }
      case OP_LEN: {
        Value value = pop(vm);
        if (isObjType(value, OBJ_STRING)) {
          ObjString* string = (ObjString*)AS_OBJ(value);
          push(vm, NUMBER_VAL(string->length));
          break;
        }
        if (isObjType(value, OBJ_ARRAY)) {
          ObjArray* array = (ObjArray*)AS_OBJ(value);
          push(vm, NUMBER_VAL(array->count));
          break;
        }
        if (isObjType(value, OBJ_MAP)) {
          ObjMap* map = (ObjMap*)AS_OBJ(value);
          push(vm, NUMBER_VAL(mapCount(map)));
          break;
        }
        runtimeError(vm, currentToken(frame), "len() expects a string, array, or map.");
        return false;
      }
      case OP_MAP_HAS: {
        Value key = pop(vm);
        Value object = pop(vm);
        if (isObjType(object, OBJ_MAP) && isString(key)) {
          ObjMap* map = (ObjMap*)AS_OBJ(object);
          Value ignored;
          push(vm, BOOL_VAL(mapGet(map, asString(key), &ignored)));
          break;
        }
        push(vm, BOOL_VAL(false));
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
      case OP_STRINGIFY: {
        Value value = pop(vm);
        ObjString* string = stringifyValue(vm, value);
        push(vm, OBJ_VAL(string));
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
      case OP_TRY: {
        uint16_t offset = READ_SHORT();
        if (vm->tryCount >= TRY_MAX) {
          runtimeError(vm, currentToken(frame), "Too many nested try blocks.");
          return false;
        }
        TryFrame* tryFrame = &vm->tryFrames[vm->tryCount++];
        tryFrame->frameIndex = vm->frameCount - 1;
        tryFrame->handler = frame->ip + offset;
        tryFrame->stackTop = vm->stackTop;
        tryFrame->env = vm->env;
        break;
      }
      case OP_END_TRY: {
        if (vm->tryCount > 0 &&
            vm->tryFrames[vm->tryCount - 1].frameIndex == vm->frameCount - 1) {
          vm->tryCount--;
        }
        break;
      }
      case OP_THROW: {
        Value thrown = pop(vm);
        push(vm, thrown);
        Value errorValue = wrapErrorValue(vm, thrown);
        pop(vm);
        if (unwindToHandler(vm, &frame, errorValue)) {
          break;
        }
        Token token = currentToken(frame);
        push(vm, errorValue);
        ObjString* message = errorMessageForValue(vm, errorValue);
        char buffer[256];
        snprintf(buffer, sizeof(buffer), "Uncaught throw: %s", message->chars);
        pop(vm);
        runtimeError(vm, token, buffer);
        return false;
      }
      case OP_TRY_UNWRAP: {
        Value value = pop(vm);
        if (!isObjType(value, OBJ_MAP)) {
          runtimeError(vm, currentToken(frame), "Cannot use '?' on this value.");
          return false;
        }
        ObjMap* map = (ObjMap*)AS_OBJ(value);
        Value out = NULL_VAL;
        bool shouldReturn = false;
        bool matched = false;
        bool ok = enumUnwrap(vm, map, "Result", "Ok", "Err", &out, &shouldReturn, &matched);
        if (!matched) {
          ok = enumUnwrap(vm, map, "Option", "Some", "None", &out, &shouldReturn, &matched);
        }
        if (!matched) {
          runtimeError(vm, currentToken(frame), "Cannot use '?' on this value.");
          return false;
        }
        if (!ok) {
          runtimeError(vm, currentToken(frame), "Invalid value for '?' unwrap.");
          return false;
        }
        if (shouldReturn) {
          if (returnFromFrame(vm, &frame, out, targetFrameCount)) {
            return true;
          }
          break;
        }
        push(vm, out);
        break;
      }
      case OP_CALL: {
        int argCount = READ_BYTE();
        Value callee = peek(vm, argCount);
        if (!callValue(vm, callee, argCount)) return false;
        frame = &vm->frames[vm->frameCount - 1];
        break;
      }
      case OP_CALL_OPTIONAL: {
        int argCount = READ_BYTE();
        Value callee = peek(vm, argCount);
        if (IS_NULL(callee)) {
          vm->stackTop -= argCount + 1;
          push(vm, NULL_VAL);
          break;
        }
        if (!callValue(vm, callee, argCount)) return false;
        frame = &vm->frames[vm->frameCount - 1];
        break;
      }
      case OP_INVOKE: {
        ObjString* name = (ObjString*)AS_OBJ(READ_CONSTANT());
        int argCount = READ_BYTE();
        Value receiver = peek(vm, argCount);
        if (isObjType(receiver, OBJ_MAP)) {
          ObjMap* map = (ObjMap*)AS_OBJ(receiver);
          if (cache && cache->kind == IC_MAP && cache->map == map) {
            int entryIndex = cache->index;
            if (entryIndex >= 0 && entryIndex < map->capacity &&
                map->entries[entryIndex].key == name) {
              Value callee = map->entries[entryIndex].value;
              vm->stackTop[-argCount - 1] = callee;
              if (!callValue(vm, callee, argCount)) return false;
              frame = &vm->frames[vm->frameCount - 1];
              break;
            }
          }

          Value value;
          int entryIndex = -1;
          if (mapGetIndex(map, name, &value, &entryIndex)) {
            if (cache) {
              cache->kind = IC_MAP;
              cache->map = map;
              cache->key = name;
              cache->index = entryIndex;
              cache->klass = NULL;
              cache->method = NULL;
            }
            vm->stackTop[-argCount - 1] = value;
            if (!callValue(vm, value, argCount)) return false;
            frame = &vm->frames[vm->frameCount - 1];
            break;
          }
          runtimeError(vm, currentToken(frame), "Undefined property.");
          return false;
        }
        if (!isObjType(receiver, OBJ_INSTANCE)) {
          runtimeError(vm, currentToken(frame), "Only instances have properties.");
          return false;
        }

        ObjInstance* instance = (ObjInstance*)AS_OBJ(receiver);
        ObjMap* fields = instance->fields;
        if (cache && cache->kind == IC_FIELD && cache->map == fields) {
          int index = cache->index;
          if (index >= 0 && index < fields->capacity &&
              fields->entries[index].key == name) {
            Value callee = fields->entries[index].value;
            vm->stackTop[-argCount - 1] = callee;
            if (!callValue(vm, callee, argCount)) return false;
            frame = &vm->frames[vm->frameCount - 1];
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
          vm->stackTop[-argCount - 1] = value;
          if (!callValue(vm, value, argCount)) return false;
          frame = &vm->frames[vm->frameCount - 1];
          break;
        }

        if (cache && cache->kind == IC_METHOD &&
            cache->klass == instance->klass &&
            cache->key == name && cache->method) {
          ObjFunction* method = cache->method;
          vm->stackTop[-argCount - 1] = OBJ_VAL(method);
          if (!callFunction(vm, method, receiver, true, argCount)) return false;
          frame = &vm->frames[vm->frameCount - 1];
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
          vm->stackTop[-argCount - 1] = OBJ_VAL(method);
          if (!callFunction(vm, method, receiver, true, argCount)) return false;
          frame = &vm->frames[vm->frameCount - 1];
          break;
        }

        {
          char suggestion[64];
          char message[256];
          if (suggestNameFromInstance(instance, name->chars, name->length,
                                      suggestion, sizeof(suggestion))) {
            snprintf(message, sizeof(message),
                     "Undefined property. Did you mean '%s'?", suggestion);
            runtimeError(vm, currentToken(frame), message);
          } else {
            runtimeError(vm, currentToken(frame), "Undefined property.");
          }
        }
        return false;
      }
      case OP_ARG_COUNT:
        push(vm, NUMBER_VAL((double)frame->argCount));
        break;
      case OP_CLOSURE: {
        ObjFunction* proto = (ObjFunction*)AS_OBJ(READ_CONSTANT());
        ObjFunction* function = cloneFunction(vm, proto, vm->env);
        push(vm, OBJ_VAL(function));
        break;
      }
      case OP_RETURN: {
        Value result = pop(vm);
        if (returnFromFrame(vm, &frame, result, targetFrameCount)) {
          return true;
        }
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
        if (!beginModuleImport(vm, &frame, pathString, alias, hasAlias != 0, false)) {
          return false;
        }
        break;
      }
      case OP_IMPORT_MODULE: {
        Value pathValue = pop(vm);
        if (!isString(pathValue)) {
          runtimeError(vm, currentToken(frame), "Import path must be a string.");
          return false;
        }
        ObjString* pathString = asString(pathValue);
        if (!beginModuleImport(vm, &frame, pathString, NULL, false, true)) {
          return false;
        }
        break;
      }
      case OP_EXPORT: {
        ObjString* name = (ObjString*)AS_OBJ(READ_CONSTANT());
        if (!frame->isModule || !frame->moduleInstance) {
          break;
        }
        Value value;
        if (!envGetByName(vm->env, name, &value)) {
          runtimeError(vm, currentToken(frame), "Cannot export undefined name.");
          return false;
        }
        mapSet(frame->moduleInstance->fields, name, value);
        break;
      }
      case OP_PRIVATE: {
        ObjString* name = (ObjString*)AS_OBJ(READ_CONSTANT());
        if (!frame->isModule) {
          break;
        }
        if (!frame->modulePrivate) {
          frame->modulePrivate = newMap(vm);
        }
        mapSet(frame->modulePrivate, name, BOOL_VAL(true));
        break;
      }
      case OP_EXPORT_VALUE: {
        ObjString* name = (ObjString*)AS_OBJ(READ_CONSTANT());
        Value value = pop(vm);
        if (!frame->isModule || !frame->moduleInstance) {
          break;
        }
        mapSet(frame->moduleInstance->fields, name, value);
        break;
      }
      case OP_EXPORT_FROM: {
        uint16_t count = READ_SHORT();
        Value moduleValue = pop(vm);
        if (!frame->isModule || !frame->moduleInstance) {
          break;
        }
        if (!isObjType(moduleValue, OBJ_INSTANCE)) {
          runtimeError(vm, currentToken(frame), "Export source must be a module.");
          return false;
        }
        ObjInstance* moduleInstance = (ObjInstance*)AS_OBJ(moduleValue);
        ObjMap* fields = moduleInstance->fields;
        if (count == 0) {
          for (int i = 0; i < fields->capacity; i++) {
            MapEntryValue* entry = &fields->entries[i];
            if (!entry->key) continue;
            mapSet(frame->moduleInstance->fields, entry->key, entry->value);
          }
          break;
        }
        for (uint16_t i = 0; i < count; i++) {
          ObjString* from = (ObjString*)AS_OBJ(READ_CONSTANT());
          ObjString* to = (ObjString*)AS_OBJ(READ_CONSTANT());
          Value value;
          if (!mapGet(fields, from, &value)) {
            runtimeError(vm, currentToken(frame), "Cannot re-export missing name.");
            return false;
          }
          mapSet(frame->moduleInstance->fields, to, value);
        }
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

    if (vm->hadError) return false;
    if (vm->maxStackSlots > 0) {
      size_t stackUsed = (size_t)(vm->stackTop - vm->stack);
      if (stackUsed > (size_t)vm->maxStackSlots) {
        runtimeError(vm, currentToken(frame), "Stack limit exceeded.");
        return false;
      }
    }
    if (vm->maxHeapBytes > 0) {
      size_t heapUsed = gcTotalHeapBytes(vm);
      if (heapUsed > vm->maxHeapBytes) {
        gcCollect(vm);
        heapUsed = gcTotalHeapBytes(vm);
        if (heapUsed > vm->maxHeapBytes) {
          runtimeError(vm, currentToken(frame), "Heap limit exceeded.");
          return false;
        }
      }
    }
  }

#undef READ_BYTE
#undef READ_SHORT
#undef READ_CONSTANT
}

static bool callScript(VM* vm, ObjFunction* function) {
  if (vm->frameCount == vm->maxFrames) {
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
  frame->argCount = 0;
  frame->isModule = false;
  frame->discardResult = false;
  frame->moduleInstance = NULL;
  frame->moduleAlias = NULL;
  frame->moduleKey = NULL;
  frame->moduleHasAlias = false;
  frame->modulePushResult = false;
  frame->modulePrivate = NULL;

  vm->currentProgram = function->program;
  return true;
}

bool interpret(VM* vm, Program* program) {
  vm->hadError = false;
  vm->instructionCount = 0;
  Program* previousProgram = vm->currentProgram;
  vm->currentProgram = program;
  programRunBegin(program);

  ObjFunction* function = program->function;
  if (!function) {
    programRunEnd(vm, program);
    vm->currentProgram = previousProgram;
    return false;
  }
  if (vm->debugBytecode) {
    disassembleFunction(function);
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

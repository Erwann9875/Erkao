#include "singlepass_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static Pattern* parseArrayPattern(Compiler* c);
static Pattern* parseMapPattern(Compiler* c);
static Pattern* parseEnumPattern(Compiler* c);

bool tokenIsUnderscore(Token token) {
  return token.length == 1 && token.start[0] == '_';
}

bool tokensEqual(Token a, Token b) {
  if (a.length != b.length) return false;
  return memcmp(a.start, b.start, (size_t)a.length) == 0;
}

Pattern* newPattern(PatternKind kind, Token token) {
  Pattern* pattern = (Pattern*)malloc(sizeof(Pattern));
  if (!pattern) {
    fprintf(stderr, "Out of memory.\n");
    exit(1);
  }
  memset(pattern, 0, sizeof(Pattern));
  pattern->kind = kind;
  pattern->token = token;
  return pattern;
}

void patternListAppend(PatternList* list, Pattern* item) {
  if (list->capacity < list->count + 1) {
    int oldCap = list->capacity;
    list->capacity = GROW_CAPACITY(oldCap);
    list->items = GROW_ARRAY(Pattern*, list->items, oldCap, list->capacity);
  }
  list->items[list->count++] = item;
}

void patternMapAppend(PatternMap* map, Token key, bool keyIsString, Pattern* value) {
  if (map->capacity < map->count + 1) {
    int oldCap = map->capacity;
    map->capacity = GROW_CAPACITY(oldCap);
    map->entries = GROW_ARRAY(PatternMapEntry, map->entries, oldCap, map->capacity);
  }
  PatternMapEntry* entry = &map->entries[map->count++];
  entry->key = key;
  entry->keyIsString = keyIsString;
  entry->value = value;
}

void patternEnumAppend(PatternEnum* patternEnum, Pattern* arg) {
  if (patternEnum->argCapacity < patternEnum->argCount + 1) {
    int oldCap = patternEnum->argCapacity;
    patternEnum->argCapacity = GROW_CAPACITY(oldCap);
    patternEnum->args = GROW_ARRAY(Pattern*, patternEnum->args, oldCap,
                                   patternEnum->argCapacity);
  }
  patternEnum->args[patternEnum->argCount++] = arg;
}

void freePattern(Pattern* pattern) {
  if (!pattern) return;
  switch (pattern->kind) {
    case PATTERN_ARRAY:
      for (int i = 0; i < pattern->as.array.count; i++) {
        freePattern(pattern->as.array.items[i]);
      }
      FREE_ARRAY(Pattern*, pattern->as.array.items, pattern->as.array.capacity);
      break;
    case PATTERN_MAP:
      for (int i = 0; i < pattern->as.map.count; i++) {
        freePattern(pattern->as.map.entries[i].value);
      }
      FREE_ARRAY(PatternMapEntry, pattern->as.map.entries, pattern->as.map.capacity);
      break;
    case PATTERN_ENUM:
      for (int i = 0; i < pattern->as.enumPattern.argCount; i++) {
        freePattern(pattern->as.enumPattern.args[i]);
      }
      FREE_ARRAY(Pattern*, pattern->as.enumPattern.args, pattern->as.enumPattern.argCapacity);
      break;
    default:
      break;
  }
  free(pattern);
}

void patternPathInit(PatternPath* path) {
  path->steps = NULL;
  path->count = 0;
  path->capacity = 0;
}

void patternPathPushIndex(PatternPath* path, int index) {
  if (path->capacity < path->count + 1) {
    int oldCap = path->capacity;
    path->capacity = GROW_CAPACITY(oldCap);
    path->steps = GROW_ARRAY(PatternPathStep, path->steps, oldCap, path->capacity);
  }
  PatternPathStep* step = &path->steps[path->count++];
  step->kind = PATH_INDEX;
  step->index = index;
  memset(&step->key, 0, sizeof(Token));
  step->keyIsString = false;
}

void patternPathPushKey(PatternPath* path, Token key, bool keyIsString) {
  if (path->capacity < path->count + 1) {
    int oldCap = path->capacity;
    path->capacity = GROW_CAPACITY(oldCap);
    path->steps = GROW_ARRAY(PatternPathStep, path->steps, oldCap, path->capacity);
  }
  PatternPathStep* step = &path->steps[path->count++];
  step->kind = PATH_KEY;
  step->index = 0;
  step->key = key;
  step->keyIsString = keyIsString;
}

void patternPathPop(PatternPath* path) {
  if (path->count > 0) {
    path->count--;
  }
}

void patternPathFree(PatternPath* path) {
  FREE_ARRAY(PatternPathStep, path->steps, path->capacity);
  patternPathInit(path);
}

void patternBindingListInit(PatternBindingList* list) {
  list->entries = NULL;
  list->count = 0;
  list->capacity = 0;
}

void patternBindingListFree(PatternBindingList* list) {
  for (int i = 0; i < list->count; i++) {
    FREE_ARRAY(PatternPathStep, list->entries[i].steps, list->entries[i].stepCount);
    FREE_ARRAY(PatternRestKey, list->entries[i].restKeys, list->entries[i].restKeyCount);
  }
  FREE_ARRAY(PatternBinding, list->entries, list->capacity);
  list->entries = NULL;
  list->count = 0;
  list->capacity = 0;
}

PatternBinding* patternBindingFind(PatternBindingList* list, Token name) {
  for (int i = 0; i < list->count; i++) {
    if (tokensEqual(list->entries[i].name, name)) {
      return &list->entries[i];
    }
  }
  return NULL;
}

void patternBindingAdd(PatternBindingList* list, Token name, PatternPath* path) {
  if (list->capacity < list->count + 1) {
    int oldCap = list->capacity;
    list->capacity = GROW_CAPACITY(oldCap);
    list->entries = GROW_ARRAY(PatternBinding, list->entries, oldCap, list->capacity);
  }
  PatternBinding* binding = &list->entries[list->count++];
  binding->name = name;
  binding->stepCount = path->count;
  binding->steps = NULL;
  binding->kind = PATTERN_BIND_PATH;
  binding->restIndex = 0;
  binding->restKeys = NULL;
  binding->restKeyCount = 0;
  if (path->count > 0) {
    binding->steps = GROW_ARRAY(PatternPathStep, binding->steps, 0, path->count);
    memcpy(binding->steps, path->steps, sizeof(PatternPathStep) * (size_t)path->count);
  }
}

static void patternBindingAddArrayRest(PatternBindingList* list, Token name,
                                       PatternPath* path, int restIndex) {
  if (list->capacity < list->count + 1) {
    int oldCap = list->capacity;
    list->capacity = GROW_CAPACITY(oldCap);
    list->entries = GROW_ARRAY(PatternBinding, list->entries, oldCap, list->capacity);
  }
  PatternBinding* binding = &list->entries[list->count++];
  binding->name = name;
  binding->stepCount = path->count;
  binding->steps = NULL;
  binding->kind = PATTERN_BIND_ARRAY_REST;
  binding->restIndex = restIndex;
  binding->restKeys = NULL;
  binding->restKeyCount = 0;
  if (path->count > 0) {
    binding->steps = GROW_ARRAY(PatternPathStep, binding->steps, 0, path->count);
    memcpy(binding->steps, path->steps, sizeof(PatternPathStep) * (size_t)path->count);
  }
}

static void patternBindingAddMapRest(PatternBindingList* list, Token name,
                                     PatternPath* path, PatternMapEntry* entries,
                                     int entryCount) {
  if (list->capacity < list->count + 1) {
    int oldCap = list->capacity;
    list->capacity = GROW_CAPACITY(oldCap);
    list->entries = GROW_ARRAY(PatternBinding, list->entries, oldCap, list->capacity);
  }
  PatternBinding* binding = &list->entries[list->count++];
  binding->name = name;
  binding->stepCount = path->count;
  binding->steps = NULL;
  binding->kind = PATTERN_BIND_MAP_REST;
  binding->restIndex = 0;
  binding->restKeys = NULL;
  binding->restKeyCount = entryCount;
  if (path->count > 0) {
    binding->steps = GROW_ARRAY(PatternPathStep, binding->steps, 0, path->count);
    memcpy(binding->steps, path->steps, sizeof(PatternPathStep) * (size_t)path->count);
  }
  if (entryCount > 0) {
    binding->restKeys = GROW_ARRAY(PatternRestKey, binding->restKeys, 0, entryCount);
    for (int i = 0; i < entryCount; i++) {
      binding->restKeys[i].key = entries[i].key;
      binding->restKeys[i].keyIsString = entries[i].keyIsString;
    }
  }
}

void patternFailureListInit(PatternFailureList* list) {
  list->entries = NULL;
  list->count = 0;
  list->capacity = 0;
}

void patternFailureListFree(PatternFailureList* list) {
  if (!list) return;
  for (int i = 0; i < list->count; i++) {
    FREE_ARRAY(PatternPathStep, list->entries[i].steps, list->entries[i].stepCount);
  }
  FREE_ARRAY(PatternFailure, list->entries, list->capacity);
  list->entries = NULL;
  list->count = 0;
  list->capacity = 0;
}

static void patternFailureListAdd(PatternFailureList* list, PatternPath* path,
                                  int jump, Token token) {
  if (list->capacity < list->count + 1) {
    int oldCap = list->capacity;
    list->capacity = GROW_CAPACITY(oldCap);
    list->entries = GROW_ARRAY(PatternFailure, list->entries, oldCap, list->capacity);
  }
  PatternFailure* failure = &list->entries[list->count++];
  failure->jump = jump;
  failure->token = token;
  failure->stepCount = path->count;
  failure->steps = NULL;
  if (path->count > 0) {
    failure->steps = GROW_ARRAY(PatternPathStep, failure->steps, 0, path->count);
    memcpy(failure->steps, path->steps, sizeof(PatternPathStep) * (size_t)path->count);
  }
}

void patternPathBufferEnsure(char** buffer, size_t* capacity, size_t needed) {
  if (*capacity >= needed) return;
  size_t newCap = *capacity < 32 ? 32 : *capacity;
  while (newCap < needed) {
    newCap = newCap * 2;
  }
  char* next = (char*)realloc(*buffer, newCap);
  if (!next) {
    fprintf(stderr, "Out of memory.\n");
    exit(1);
  }
  *buffer = next;
  *capacity = newCap;
}

static void patternPathBufferAppend(char** buffer, size_t* length, size_t* capacity,
                                    const char* text, size_t textLen) {
  if (textLen == 0) return;
  size_t needed = *length + textLen + 1;
  patternPathBufferEnsure(buffer, capacity, needed);
  memcpy(*buffer + *length, text, textLen);
  *length += textLen;
  (*buffer)[*length] = '\0';
}

static void patternPathBufferAppendChar(char** buffer, size_t* length, size_t* capacity,
                                        char c) {
  patternPathBufferEnsure(buffer, capacity, *length + 2);
  (*buffer)[(*length)++] = c;
  (*buffer)[*length] = '\0';
}

static void patternPathAppendEscaped(char** buffer, size_t* length, size_t* capacity,
                                     const char* text, int textLen) {
  for (int i = 0; i < textLen; i++) {
    unsigned char c = (unsigned char)text[i];
    if (c == '"' || c == '\\') {
      patternPathBufferAppendChar(buffer, length, capacity, '\\');
      patternPathBufferAppendChar(buffer, length, capacity, (char)c);
      continue;
    }
    if (c == '\n') {
      patternPathBufferAppend(buffer, length, capacity, "\\n", 2);
      continue;
    }
    if (c == '\r') {
      patternPathBufferAppend(buffer, length, capacity, "\\r", 2);
      continue;
    }
    if (c == '\t') {
      patternPathBufferAppend(buffer, length, capacity, "\\t", 2);
      continue;
    }
    if (c < 32) {
      char hex[5];
      snprintf(hex, sizeof(hex), "\\x%02x", c);
      patternPathBufferAppend(buffer, length, capacity, hex, 4);
      continue;
    }
    patternPathBufferAppendChar(buffer, length, capacity, (char)c);
  }
}

ObjString* patternPathString(Compiler* c, PatternPathStep* steps, int stepCount) {
  size_t length = 0;
  size_t capacity = 0;
  char* buffer = NULL;
  patternPathBufferAppend(&buffer, &length, &capacity, "$", 1);
  for (int i = 0; i < stepCount; i++) {
    PatternPathStep step = steps[i];
    if (step.kind == PATH_KEY) {
      if (step.keyIsString) {
        char* keyName = parseStringLiteral(step.key);
        patternPathBufferAppend(&buffer, &length, &capacity, "[\"", 2);
        patternPathAppendEscaped(&buffer, &length, &capacity, keyName,
                                 (int)strlen(keyName));
        patternPathBufferAppend(&buffer, &length, &capacity, "\"]", 2);
        free(keyName);
      } else {
        patternPathBufferAppendChar(&buffer, &length, &capacity, '.');
        patternPathBufferAppend(&buffer, &length, &capacity,
                                step.key.start, (size_t)step.key.length);
      }
    } else {
      char indexBuffer[32];
      int indexLen = snprintf(indexBuffer, sizeof(indexBuffer), "[%d]", step.index);
      if (indexLen < 0) indexLen = 0;
      patternPathBufferAppend(&buffer, &length, &capacity, indexBuffer, (size_t)indexLen);
    }
  }
  return takeStringWithLength(c->vm, buffer, (int)length);
}

ObjString* patternFailureMessage(Compiler* c, ObjString* path) {
  const char* prefix = "Pattern match failed at ";
  const char* suffix = ".";
  int prefixLen = (int)strlen(prefix);
  int suffixLen = (int)strlen(suffix);
  int total = prefixLen + path->length + suffixLen;
  char* buffer = (char*)malloc((size_t)total + 1);
  if (!buffer) {
    fprintf(stderr, "Out of memory.\n");
    exit(1);
  }
  memcpy(buffer, prefix, (size_t)prefixLen);
  memcpy(buffer + prefixLen, path->chars, (size_t)path->length);
  memcpy(buffer + prefixLen + path->length, suffix, (size_t)suffixLen);
  buffer[total] = '\0';
  return takeStringWithLength(c->vm, buffer, total);
}

bool isEnumPatternStart(Compiler* c) {
  if (!check(c, TOKEN_IDENTIFIER) || !checkNext(c, TOKEN_DOT)) return false;
  int lookahead = c->current + 2;
  if (lookahead >= c->tokens->count) return false;
  Token variant = c->tokens->tokens[lookahead];
  if (variant.type != TOKEN_IDENTIFIER) return false;
  int afterIndex = lookahead + 1;
  if (afterIndex >= c->tokens->count) return false;
  ErkaoTokenType afterType = c->tokens->tokens[afterIndex].type;
  return afterType == TOKEN_LEFT_PAREN ||
         afterType == TOKEN_COLON ||
         afterType == TOKEN_COMMA ||
         afterType == TOKEN_RIGHT_PAREN ||
         afterType == TOKEN_RIGHT_BRACE ||
         afterType == TOKEN_RIGHT_BRACKET ||
         afterType == TOKEN_EQUAL ||
         afterType == TOKEN_IF ||
         afterType == TOKEN_SEMICOLON;
}


Pattern* parsePattern(Compiler* c) {
  if (isEnumPatternStart(c)) {
    return parseEnumPattern(c);
  }
  if (match(c, TOKEN_LEFT_BRACKET)) {
    return parseArrayPattern(c);
  }
  if (match(c, TOKEN_LEFT_BRACE)) {
    return parseMapPattern(c);
  }
  if (match(c, TOKEN_CARET)) {
    Token name = consume(c, TOKEN_IDENTIFIER, "Expect name after '^'.");
    if (tokenIsUnderscore(name)) {
      errorAt(c, name, "Cannot pin '_'.");
    }
    return newPattern(PATTERN_PIN, name);
  }
  if (match(c, TOKEN_NUMBER) || match(c, TOKEN_STRING) ||
      match(c, TOKEN_TRUE) || match(c, TOKEN_FALSE) || match(c, TOKEN_NULL)) {
    return newPattern(PATTERN_LITERAL, previous(c));
  }
  if (match(c, TOKEN_IDENTIFIER) || match(c, TOKEN_TYPE_KW)) {
    Token name = previous(c);
    if (tokenIsUnderscore(name)) {
      return newPattern(PATTERN_WILDCARD, name);
    }
    return newPattern(PATTERN_BINDING, name);
  }
  errorAtCurrent(c, "Expect pattern.");
  return newPattern(PATTERN_WILDCARD, previous(c));
}

Pattern* parseArrayPattern(Compiler* c) {
  Token open = previous(c);
  Pattern* pattern = newPattern(PATTERN_ARRAY, open);
  if (!check(c, TOKEN_RIGHT_BRACKET)) {
    do {
      if (match(c, TOKEN_ELLIPSIS)) {
        Token restName = consume(c, TOKEN_IDENTIFIER, "Expect rest binding name after '...'.");
        if (pattern->as.array.hasRest) {
          errorAt(c, restName, "Array pattern can only have one rest binding.");
        } else {
          pattern->as.array.hasRest = true;
          pattern->as.array.restName = restName;
        }
        if (match(c, TOKEN_COMMA) && !check(c, TOKEN_RIGHT_BRACKET)) {
          errorAtCurrent(c, "Array rest pattern must be last.");
        }
        break;
      }
      Pattern* item = parsePattern(c);
      patternListAppend(&pattern->as.array, item);
    } while (match(c, TOKEN_COMMA));
  }
  consumeClosing(c, TOKEN_RIGHT_BRACKET, "Expect ']' after array pattern.", open);
  return pattern;
}

Pattern* parseMapPattern(Compiler* c) {
  Token open = previous(c);
  Pattern* pattern = newPattern(PATTERN_MAP, open);
  if (!check(c, TOKEN_RIGHT_BRACE)) {
    do {
      if (match(c, TOKEN_DOT_DOT)) {
        Token restName = consume(c, TOKEN_IDENTIFIER, "Expect rest binding name after '..'.");
        if (pattern->as.map.hasRest) {
          errorAt(c, restName, "Map pattern can only have one rest binding.");
        } else {
          pattern->as.map.hasRest = true;
          pattern->as.map.restName = restName;
        }
        if (match(c, TOKEN_COMMA) && !check(c, TOKEN_RIGHT_BRACE)) {
          errorAtCurrent(c, "Map rest pattern must be last.");
        }
        break;
      }
      Token key;
      bool keyIsString = false;
      if (match(c, TOKEN_IDENTIFIER) || match(c, TOKEN_TYPE_KW)) {
        key = previous(c);
      } else if (match(c, TOKEN_STRING)) {
        key = previous(c);
        keyIsString = true;
      } else {
        errorAtCurrent(c, "Map pattern keys must be identifiers or strings.");
        break;
      }
      Pattern* value = NULL;
      if (match(c, TOKEN_COLON)) {
        value = parsePattern(c);
      } else {
        if (keyIsString) {
          errorAt(c, key, "String map keys require ':' and a value pattern.");
          value = newPattern(PATTERN_WILDCARD, key);
        } else {
          value = newPattern(PATTERN_BINDING, key);
        }
      }
      patternMapAppend(&pattern->as.map, key, keyIsString, value);
    } while (match(c, TOKEN_COMMA));
  }
  consumeClosing(c, TOKEN_RIGHT_BRACE, "Expect '}' after map pattern.", open);
  return pattern;
}

Pattern* parseEnumPattern(Compiler* c) {
  Token enumToken = consume(c, TOKEN_IDENTIFIER, "Expect enum name.");
  consume(c, TOKEN_DOT, "Expect '.' in enum pattern.");
  Token variantToken = consume(c, TOKEN_IDENTIFIER, "Expect enum variant name.");
  Pattern* pattern = newPattern(PATTERN_ENUM, enumToken);
  pattern->as.enumPattern.enumToken = enumToken;
  pattern->as.enumPattern.variantToken = variantToken;
  if (match(c, TOKEN_LEFT_PAREN)) {
    Token open = previous(c);
    if (!check(c, TOKEN_RIGHT_PAREN)) {
      do {
        Pattern* arg = parsePattern(c);
        patternEnumAppend(&pattern->as.enumPattern, arg);
      } while (match(c, TOKEN_COMMA));
    }
    consumeClosing(c, TOKEN_RIGHT_PAREN, "Expect ')' after enum pattern.", open);
  }
  return pattern;
}

void emitPatternKeyConstant(Compiler* c, Token key, bool keyIsString, Token token) {
  if (keyIsString) {
    char* keyName = parseStringLiteral(key);
    ObjString* keyStr = takeStringWithLength(c->vm, keyName, (int)strlen(keyName));
    emitConstant(c, OBJ_VAL(keyStr), token);
  } else {
    ObjString* keyStr = stringFromToken(c->vm, key);
    emitConstant(c, OBJ_VAL(keyStr), token);
  }
}

static void emitPatternValueSteps(Compiler* c, int switchValue,
                                  PatternPathStep* steps, int stepCount,
                                  Token token) {
  emitGetVarConstant(c, switchValue);
  for (int i = 0; i < stepCount; i++) {
    PatternPathStep step = steps[i];
    if (step.kind == PATH_KEY) {
      emitPatternKeyConstant(c, step.key, step.keyIsString, token);
      emitByte(c, OP_GET_INDEX, token);
    } else {
      emitConstant(c, NUMBER_VAL((double)step.index), token);
      emitByte(c, OP_GET_INDEX, token);
    }
  }
}

void emitPatternValue(Compiler* c, int switchValue, PatternPath* path, Token token) {
  emitPatternValueSteps(c, switchValue, path->steps, path->count, token);
}

void emitPatternLiteral(Compiler* c, Pattern* pattern) {
  Token token = pattern->token;
  switch (token.type) {
    case TOKEN_NUMBER: {
      double value = parseNumberToken(token);
      emitConstant(c, NUMBER_VAL(value), token);
      break;
    }
    case TOKEN_STRING: {
      char* value = parseStringLiteral(token);
      ObjString* str = takeStringWithLength(c->vm, value, (int)strlen(value));
      emitConstant(c, OBJ_VAL(str), token);
      break;
    }
    case TOKEN_TRUE:
      emitByte(c, OP_TRUE, token);
      break;
    case TOKEN_FALSE:
      emitByte(c, OP_FALSE, token);
      break;
    case TOKEN_NULL:
      emitByte(c, OP_NULL, token);
      break;
    default:
      emitByte(c, OP_NULL, token);
      break;
  }
}

void emitPatternCheckJump(Compiler* c, JumpList* failJumps, Token token) {
  int jump = emitJump(c, OP_JUMP_IF_FALSE, token);
  writeJumpList(failJumps, jump);
  emitByte(c, OP_POP, noToken());
}

static void emitPatternCheckJumpDetailed(Compiler* c, PatternFailureList* failures,
                                         PatternPath* path, Token token) {
  int jump = emitJump(c, OP_JUMP_IF_FALSE, token);
  patternFailureListAdd(failures, path, jump, token);
  emitByte(c, OP_POP, noToken());
}

bool patternPinnedDefined(Compiler* c, Token name) {
  if (!typecheckEnabled(c)) return true;
  ObjString* nameStr = stringFromToken(c->vm, name);
  return typeLookupEntry(c->typecheck, nameStr) != NULL;
}

bool patternIsCatchAll(Pattern* pattern) {
  if (!pattern) return false;
  return pattern->kind == PATTERN_BINDING || pattern->kind == PATTERN_WILDCARD;
}

bool patternConstValue(Pattern* pattern, ConstValue* out) {
  if (!pattern || pattern->kind != PATTERN_LITERAL) return false;
  Token token = pattern->token;
  out->ownsString = false;
  switch (token.type) {
    case TOKEN_NUMBER:
      out->type = CONST_NUMBER;
      out->as.number = parseNumberToken(token);
      return true;
    case TOKEN_STRING: {
      char* value = parseStringLiteral(token);
      out->type = CONST_STRING;
      out->as.string.chars = value;
      out->as.string.length = (int)strlen(value);
      out->ownsString = true;
      return true;
    }
    case TOKEN_TRUE:
      out->type = CONST_BOOL;
      out->as.boolean = true;
      return true;
    case TOKEN_FALSE:
      out->type = CONST_BOOL;
      out->as.boolean = false;
      return true;
    case TOKEN_NULL:
      out->type = CONST_NULL;
      return true;
    default:
      break;
  }
  return false;
}

bool constValueListContains(ConstValue* values, int count, ConstValue* value) {
  for (int i = 0; i < count; i++) {
    if (constValueEquals(&values[i], value)) return true;
  }
  return false;
}

void constValueListAdd(ConstValue** values, int* count, int* capacity,
                              ConstValue* value) {
  if (*capacity < *count + 1) {
    int oldCap = *capacity;
    *capacity = GROW_CAPACITY(oldCap);
    *values = GROW_ARRAY(ConstValue, *values, oldCap, *capacity);
    if (!*values) {
      fprintf(stderr, "Out of memory.\n");
      exit(1);
    }
  }
  (*values)[*count] = *value;
  (*count)++;
  value->ownsString = false;
}

void constValueListFree(ConstValue* values, int count, int capacity) {
  for (int i = 0; i < count; i++) {
    constValueFree(&values[i]);
  }
  FREE_ARRAY(ConstValue, values, capacity);
}

Type* typeFromPattern(Compiler* c, Pattern* pattern) {
  if (!pattern || !typecheckEnabled(c)) return typeAny();
  switch (pattern->kind) {
    case PATTERN_LITERAL: {
      Token token = pattern->token;
      switch (token.type) {
        case TOKEN_NUMBER:
          return typeNumber();
        case TOKEN_STRING:
          return typeString();
        case TOKEN_TRUE:
        case TOKEN_FALSE:
          return typeBool();
        case TOKEN_NULL:
          return typeNull();
        default:
          return typeAny();
      }
    }
    case PATTERN_ARRAY:
      return typeArray(c->typecheck, typeAny());
    case PATTERN_MAP:
      return typeMap(c->typecheck, typeString(), typeAny());
    case PATTERN_ENUM:
      return typeNamed(c->typecheck,
                       stringFromToken(c->vm, pattern->as.enumPattern.enumToken));
    case PATTERN_PIN:
      return typeLookup(c, pattern->token);
    case PATTERN_BINDING:
    case PATTERN_WILDCARD:
    default:
      return typeAny();
  }
}

Type* typeIntersect(TypeChecker* tc, Type* left, Type* right) {
  if (!tc || !left || !right) return typeAny();
  if (typeIsAny(left)) return right;
  if (typeIsAny(right)) return left;
  if (typeEquals(left, right)) return left;
  if (left->kind == TYPE_UNION) {
    Type* merged = NULL;
    for (int i = 0; i < left->unionCount; i++) {
      Type* candidate = typeIntersect(tc, left->unionTypes[i], right);
      if (!candidate) continue;
      merged = typeMerge(tc, merged, candidate);
    }
    return merged;
  }
  if (right->kind == TYPE_UNION) {
    return typeIntersect(tc, right, left);
  }
  if (typeAssignable(left, right)) return right;
  if (typeAssignable(right, left)) return left;
  return NULL;
}

Type* typeNarrowByPattern(Compiler* c, Type* valueType, Pattern* pattern) {
  if (!typecheckEnabled(c) || !valueType) return valueType;
  Type* patternType = typeFromPattern(c, pattern);
  if (!patternType || typeIsAny(patternType)) return valueType;
  Type* narrowed = typeIntersect(c->typecheck, valueType, patternType);
  return narrowed ? narrowed : valueType;
}

static void emitPatternChecks(Compiler* c, int switchValue, Pattern* pattern,
                              PatternPath* path, JumpList* failJumps,
                              PatternBindingList* bindings) {
  if (!pattern) return;
  switch (pattern->kind) {
    case PATTERN_WILDCARD:
      return;
    case PATTERN_BINDING: {
      if (tokenIsUnderscore(pattern->token)) return;
      PatternBinding* existing = patternBindingFind(bindings, pattern->token);
      if (!existing) {
        patternBindingAdd(bindings, pattern->token, path);
        return;
      }
      emitPatternValueSteps(c, switchValue, existing->steps, existing->stepCount, pattern->token);
      emitPatternValue(c, switchValue, path, pattern->token);
      emitByte(c, OP_EQUAL, pattern->token);
      emitPatternCheckJump(c, failJumps, pattern->token);
      return;
    }
    case PATTERN_PIN: {
      if (!patternPinnedDefined(c, pattern->token)) {
        errorAt(c, pattern->token, "Pinned variable must be defined.");
        return;
      }
      emitPatternValue(c, switchValue, path, pattern->token);
      int nameIdx = emitStringConstant(c, pattern->token);
      emitByte(c, OP_GET_VAR, pattern->token);
      emitShort(c, (uint16_t)nameIdx, pattern->token);
      emitByte(c, OP_EQUAL, pattern->token);
      emitPatternCheckJump(c, failJumps, pattern->token);
      return;
    }
    case PATTERN_LITERAL:
      emitPatternValue(c, switchValue, path, pattern->token);
      emitPatternLiteral(c, pattern);
      emitByte(c, OP_EQUAL, pattern->token);
      emitPatternCheckJump(c, failJumps, pattern->token);
      return;
    case PATTERN_ARRAY: {
      emitPatternValue(c, switchValue, path, pattern->token);
      emitByte(c, OP_IS_ARRAY, pattern->token);
      emitPatternCheckJump(c, failJumps, pattern->token);

      emitPatternValue(c, switchValue, path, pattern->token);
      emitByte(c, OP_LEN, pattern->token);
      emitConstant(c, NUMBER_VAL((double)pattern->as.array.count), pattern->token);
      emitByte(c, pattern->as.array.hasRest ? OP_GREATER_EQUAL : OP_EQUAL, pattern->token);
      emitPatternCheckJump(c, failJumps, pattern->token);

      for (int i = 0; i < pattern->as.array.count; i++) {
        patternPathPushIndex(path, i);
        emitPatternChecks(c, switchValue, pattern->as.array.items[i], path,
                          failJumps, bindings);
        patternPathPop(path);
      }
      if (pattern->as.array.hasRest && !tokenIsUnderscore(pattern->as.array.restName)) {
        if (patternBindingFind(bindings, pattern->as.array.restName)) {
          errorAt(c, pattern->as.array.restName, "Duplicate pattern binding.");
        } else {
          patternBindingAddArrayRest(bindings, pattern->as.array.restName, path,
                                     pattern->as.array.count);
        }
      }
      return;
    }
    case PATTERN_MAP: {
      emitPatternValue(c, switchValue, path, pattern->token);
      emitByte(c, OP_IS_MAP, pattern->token);
      emitPatternCheckJump(c, failJumps, pattern->token);

      for (int i = 0; i < pattern->as.map.count; i++) {
        PatternMapEntry* entry = &pattern->as.map.entries[i];
        emitPatternValue(c, switchValue, path, entry->key);
        emitPatternKeyConstant(c, entry->key, entry->keyIsString, entry->key);
        emitByte(c, OP_MAP_HAS, entry->key);
        emitPatternCheckJump(c, failJumps, entry->key);

        patternPathPushKey(path, entry->key, entry->keyIsString);
        emitPatternChecks(c, switchValue, entry->value, path, failJumps, bindings);
        patternPathPop(path);
      }
      if (pattern->as.map.hasRest && !tokenIsUnderscore(pattern->as.map.restName)) {
        if (patternBindingFind(bindings, pattern->as.map.restName)) {
          errorAt(c, pattern->as.map.restName, "Duplicate pattern binding.");
        } else {
          patternBindingAddMapRest(bindings, pattern->as.map.restName, path,
                                   pattern->as.map.entries, pattern->as.map.count);
        }
      }
      return;
    }
    case PATTERN_ENUM: {
      EnumInfo* info = findEnumInfo(c, pattern->as.enumPattern.enumToken);
      if (!info) {
        errorAt(c, pattern->as.enumPattern.enumToken, "Unknown enum in match pattern.");
      } else if (!info->isAdt) {
        errorAt(c, pattern->as.enumPattern.enumToken, "Enum does not support payload patterns.");
      }
      EnumVariantInfo* variantInfo = info ?
          findEnumVariant(info, pattern->as.enumPattern.variantToken) : NULL;
      if (!variantInfo && info) {
        errorAt(c, pattern->as.enumPattern.variantToken, "Unknown enum variant.");
      } else if (variantInfo && variantInfo->arity != pattern->as.enumPattern.argCount) {
        char patternMessage[96];
        snprintf(patternMessage, sizeof(patternMessage),
                 "Pattern expects %d bindings but got %d.",
                 variantInfo->arity, pattern->as.enumPattern.argCount);
        errorAt(c, pattern->as.enumPattern.variantToken, patternMessage);
      }

      emitPatternValue(c, switchValue, path, pattern->token);
      int enumIdx = emitStringConstant(c, pattern->as.enumPattern.enumToken);
      int variantIdx = emitStringConstant(c, pattern->as.enumPattern.variantToken);
      emitByte(c, OP_MATCH_ENUM, pattern->token);
      emitShort(c, (uint16_t)enumIdx, pattern->token);
      emitShort(c, (uint16_t)variantIdx, pattern->token);
      emitPatternCheckJump(c, failJumps, pattern->token);

      Token valuesToken = syntheticToken("_values");
      for (int i = 0; i < pattern->as.enumPattern.argCount; i++) {
        patternPathPushKey(path, valuesToken, false);
        patternPathPushIndex(path, i);
        emitPatternChecks(c, switchValue, pattern->as.enumPattern.args[i], path,
                          failJumps, bindings);
        patternPathPop(path);
        patternPathPop(path);
      }
      return;
    }
    default:
      return;
  }
}

static void emitPatternChecksDetailed(Compiler* c, int switchValue, Pattern* pattern,
                                      PatternPath* path, PatternFailureList* failures,
                                      PatternBindingList* bindings) {
  if (!pattern) return;
  switch (pattern->kind) {
    case PATTERN_WILDCARD:
      return;
    case PATTERN_BINDING: {
      if (tokenIsUnderscore(pattern->token)) return;
      PatternBinding* existing = patternBindingFind(bindings, pattern->token);
      if (!existing) {
        patternBindingAdd(bindings, pattern->token, path);
        return;
      }
      emitPatternValueSteps(c, switchValue, existing->steps, existing->stepCount, pattern->token);
      emitPatternValue(c, switchValue, path, pattern->token);
      emitByte(c, OP_EQUAL, pattern->token);
      emitPatternCheckJumpDetailed(c, failures, path, pattern->token);
      return;
    }
    case PATTERN_PIN: {
      if (!patternPinnedDefined(c, pattern->token)) {
        errorAt(c, pattern->token, "Pinned variable must be defined.");
        return;
      }
      emitPatternValue(c, switchValue, path, pattern->token);
      int nameIdx = emitStringConstant(c, pattern->token);
      emitByte(c, OP_GET_VAR, pattern->token);
      emitShort(c, (uint16_t)nameIdx, pattern->token);
      emitByte(c, OP_EQUAL, pattern->token);
      emitPatternCheckJumpDetailed(c, failures, path, pattern->token);
      return;
    }
    case PATTERN_LITERAL:
      emitPatternValue(c, switchValue, path, pattern->token);
      emitPatternLiteral(c, pattern);
      emitByte(c, OP_EQUAL, pattern->token);
      emitPatternCheckJumpDetailed(c, failures, path, pattern->token);
      return;
    case PATTERN_ARRAY: {
      emitPatternValue(c, switchValue, path, pattern->token);
      emitByte(c, OP_IS_ARRAY, pattern->token);
      emitPatternCheckJumpDetailed(c, failures, path, pattern->token);

      emitPatternValue(c, switchValue, path, pattern->token);
      emitByte(c, OP_LEN, pattern->token);
      emitConstant(c, NUMBER_VAL((double)pattern->as.array.count), pattern->token);
      emitByte(c, pattern->as.array.hasRest ? OP_GREATER_EQUAL : OP_EQUAL, pattern->token);
      emitPatternCheckJumpDetailed(c, failures, path, pattern->token);

      for (int i = 0; i < pattern->as.array.count; i++) {
        patternPathPushIndex(path, i);
        emitPatternChecksDetailed(c, switchValue, pattern->as.array.items[i], path,
                                  failures, bindings);
        patternPathPop(path);
      }
      if (pattern->as.array.hasRest && !tokenIsUnderscore(pattern->as.array.restName)) {
        if (patternBindingFind(bindings, pattern->as.array.restName)) {
          errorAt(c, pattern->as.array.restName, "Duplicate pattern binding.");
        } else {
          patternBindingAddArrayRest(bindings, pattern->as.array.restName, path,
                                     pattern->as.array.count);
        }
      }
      return;
    }
    case PATTERN_MAP: {
      emitPatternValue(c, switchValue, path, pattern->token);
      emitByte(c, OP_IS_MAP, pattern->token);
      emitPatternCheckJumpDetailed(c, failures, path, pattern->token);

      for (int i = 0; i < pattern->as.map.count; i++) {
        PatternMapEntry* entry = &pattern->as.map.entries[i];
        emitPatternValue(c, switchValue, path, entry->key);
        emitPatternKeyConstant(c, entry->key, entry->keyIsString, entry->key);
        emitByte(c, OP_MAP_HAS, entry->key);
        patternPathPushKey(path, entry->key, entry->keyIsString);
        emitPatternCheckJumpDetailed(c, failures, path, entry->key);
        patternPathPop(path);

        patternPathPushKey(path, entry->key, entry->keyIsString);
        emitPatternChecksDetailed(c, switchValue, entry->value, path, failures, bindings);
        patternPathPop(path);
      }
      if (pattern->as.map.hasRest && !tokenIsUnderscore(pattern->as.map.restName)) {
        if (patternBindingFind(bindings, pattern->as.map.restName)) {
          errorAt(c, pattern->as.map.restName, "Duplicate pattern binding.");
        } else {
          patternBindingAddMapRest(bindings, pattern->as.map.restName, path,
                                   pattern->as.map.entries, pattern->as.map.count);
        }
      }
      return;
    }
    case PATTERN_ENUM: {
      EnumInfo* info = findEnumInfo(c, pattern->as.enumPattern.enumToken);
      if (!info) {
        errorAt(c, pattern->as.enumPattern.enumToken, "Unknown enum in match pattern.");
      } else if (!info->isAdt) {
        errorAt(c, pattern->as.enumPattern.enumToken, "Enum does not support payload patterns.");
      }
      EnumVariantInfo* variantInfo = info ?
          findEnumVariant(info, pattern->as.enumPattern.variantToken) : NULL;
      if (!variantInfo && info) {
        errorAt(c, pattern->as.enumPattern.variantToken, "Unknown enum variant.");
      } else if (variantInfo && variantInfo->arity != pattern->as.enumPattern.argCount) {
        char patternMessage[96];
        snprintf(patternMessage, sizeof(patternMessage),
                 "Pattern expects %d bindings but got %d.",
                 variantInfo->arity, pattern->as.enumPattern.argCount);
        errorAt(c, pattern->as.enumPattern.variantToken, patternMessage);
      }

      emitPatternValue(c, switchValue, path, pattern->token);
      int enumIdx = emitStringConstant(c, pattern->as.enumPattern.enumToken);
      int variantIdx = emitStringConstant(c, pattern->as.enumPattern.variantToken);
      emitByte(c, OP_MATCH_ENUM, pattern->token);
      emitShort(c, (uint16_t)enumIdx, pattern->token);
      emitShort(c, (uint16_t)variantIdx, pattern->token);
      emitPatternCheckJumpDetailed(c, failures, path, pattern->token);

      Token valuesToken = syntheticToken("_values");
      for (int i = 0; i < pattern->as.enumPattern.argCount; i++) {
        patternPathPushKey(path, valuesToken, false);
        patternPathPushIndex(path, i);
        emitPatternChecksDetailed(c, switchValue, pattern->as.enumPattern.args[i], path,
                                  failures, bindings);
        patternPathPop(path);
        patternPathPop(path);
      }
      return;
    }
    default:
      return;
  }
}

void emitPatternMatchValue(Compiler* c, int switchValue, Pattern* pattern,
                                  PatternBindingList* bindings) {
  JumpList failJumps;
  initJumpList(&failJumps);
  PatternPath path;
  patternPathInit(&path);
  emitPatternChecks(c, switchValue, pattern, &path, &failJumps, bindings);
  patternPathFree(&path);

  if (failJumps.count == 0) {
    emitByte(c, OP_TRUE, pattern ? pattern->token : noToken());
    freeJumpList(&failJumps);
    return;
  }

  emitByte(c, OP_TRUE, pattern ? pattern->token : noToken());
  int endJump = emitJump(c, OP_JUMP, pattern ? pattern->token : noToken());
  int failTarget = c->chunk->count;
  patchJumpList(c, &failJumps, failTarget, pattern ? pattern->token : noToken());
  emitByte(c, OP_POP, noToken());
  emitByte(c, OP_FALSE, pattern ? pattern->token : noToken());
  patchJump(c, endJump, pattern ? pattern->token : noToken());
  freeJumpList(&failJumps);
}

void emitPatternFailureThrow(Compiler* c, int switchValue, PatternFailure* failure) {
  Token token = failure->token;
  emitByte(c, OP_POP, noToken());

  ObjString* pathStr = patternPathString(c, failure->steps, failure->stepCount);
  ObjString* messageStr = patternFailureMessage(c, pathStr);

  emitByte(c, OP_MAP, token);
  emitShort(c, 3, token);

  ObjString* messageKey = copyStringWithLength(c->vm, "message", 7);
  emitConstant(c, OBJ_VAL(messageKey), token);
  emitConstant(c, OBJ_VAL(messageStr), token);
  emitByte(c, OP_MAP_SET, token);

  ObjString* pathKey = copyStringWithLength(c->vm, "path", 4);
  emitConstant(c, OBJ_VAL(pathKey), token);
  emitConstant(c, OBJ_VAL(pathStr), token);
  emitByte(c, OP_MAP_SET, token);

  ObjString* valueKey = copyStringWithLength(c->vm, "value", 5);
  emitConstant(c, OBJ_VAL(valueKey), token);
  emitPatternValueSteps(c, switchValue, failure->steps, failure->stepCount, token);
  emitByte(c, OP_MAP_SET, token);

  emitByte(c, OP_THROW, token);
}

void emitPatternMatchOrThrow(Compiler* c, int switchValue, Pattern* pattern,
                                    PatternBindingList* bindings) {
  PatternFailureList failures;
  patternFailureListInit(&failures);
  PatternPath path;
  patternPathInit(&path);
  emitPatternChecksDetailed(c, switchValue, pattern, &path, &failures, bindings);
  patternPathFree(&path);

  if (failures.count == 0) {
    patternFailureListFree(&failures);
    return;
  }

  int endJump = emitJump(c, OP_JUMP, pattern ? pattern->token : noToken());
  for (int i = 0; i < failures.count; i++) {
    PatternFailure* failure = &failures.entries[i];
    patchJump(c, failure->jump, failure->token);
    emitPatternFailureThrow(c, switchValue, failure);
  }
  patchJump(c, endJump, pattern ? pattern->token : noToken());
  patternFailureListFree(&failures);
}

static Type* typeForPatternPath(TypeChecker* tc, Type* root,
                                PatternPathStep* steps, int stepCount) {
  if (!tc) return typeAny();
  if (!root) return typeAny();
  if (root->kind == TYPE_UNION) {
    Type* merged = NULL;
    for (int i = 0; i < root->unionCount; i++) {
      Type* candidate = typeForPatternPath(tc, root->unionTypes[i], steps, stepCount);
      if (!candidate) continue;
      merged = typeMerge(tc, merged, candidate);
    }
    return merged ? merged : typeAny();
  }
  if (typeIsAny(root)) return typeAny();
  Type* current = root;
  for (int i = 0; i < stepCount; i++) {
    if (!current || typeIsAny(current)) return typeAny();
    PatternPathStep step = steps[i];
    if (step.kind == PATH_INDEX) {
      if (current->kind != TYPE_ARRAY) return NULL;
      current = current->elem ? current->elem : typeAny();
      continue;
    }
    if (step.kind == PATH_KEY) {
      if (current->kind != TYPE_MAP) return NULL;
      current = current->value ? current->value : typeAny();
      continue;
    }
  }
  return current ? current : typeAny();
}

Type* typeForArrayRest(TypeChecker* tc, Type* root, PatternBinding* binding) {
  Type* container = typeForPatternPath(tc, root, binding->steps, binding->stepCount);
  if (!container) return typeArray(tc, typeAny());
  if (container->kind == TYPE_UNION) {
    Type* elemType = NULL;
    for (int i = 0; i < container->unionCount; i++) {
      Type* member = container->unionTypes[i];
      if (member && member->kind == TYPE_ARRAY) {
        elemType = typeMerge(tc, elemType, member->elem ? member->elem : typeAny());
      }
    }
    if (!elemType) elemType = typeAny();
    return typeArray(tc, elemType);
  }
  if (typeIsAny(container) || container->kind != TYPE_ARRAY) {
    return typeArray(tc, typeAny());
  }
  return typeArray(tc, container->elem ? container->elem : typeAny());
}

Type* typeForMapRest(TypeChecker* tc, Type* root, PatternBinding* binding) {
  Type* container = typeForPatternPath(tc, root, binding->steps, binding->stepCount);
  if (!container) return typeMap(tc, typeString(), typeAny());
  if (container->kind == TYPE_UNION) {
    Type* valueType = NULL;
    for (int i = 0; i < container->unionCount; i++) {
      Type* member = container->unionTypes[i];
      if (member && member->kind == TYPE_MAP) {
        valueType = typeMerge(tc, valueType, member->value ? member->value : typeAny());
      }
    }
    if (!valueType) valueType = typeAny();
    return typeMap(tc, typeString(), valueType);
  }
  if (typeIsAny(container) || container->kind != TYPE_MAP) {
    return typeMap(tc, typeString(), typeAny());
  }
  return typeMap(tc, typeString(), container->value ? container->value : typeAny());
}

void emitPatternRestKeyArray(Compiler* c, PatternBinding* binding) {
  int count = binding->restKeyCount;
  emitByte(c, OP_ARRAY, binding->name);
  emitShort(c, (uint16_t)count, binding->name);
  for (int i = 0; i < count; i++) {
    PatternRestKey* key = &binding->restKeys[i];
    emitPatternKeyConstant(c, key->key, key->keyIsString, binding->name);
    emitByte(c, OP_ARRAY_APPEND, binding->name);
  }
}

void emitPatternBindings(Compiler* c, int switchValue, PatternBindingList* bindings,
                                uint8_t defineOp, Type* matchType) {
  for (int i = 0; i < bindings->count; i++) {
    PatternBinding* binding = &bindings->entries[i];
    Type* bindingType = typeAny();
    if (typecheckEnabled(c)) {
      switch (binding->kind) {
        case PATTERN_BIND_PATH:
          bindingType = typeForPatternPath(c->typecheck, matchType,
                                           binding->steps, binding->stepCount);
          if (!bindingType) bindingType = typeAny();
          break;
        case PATTERN_BIND_ARRAY_REST:
          bindingType = typeForArrayRest(c->typecheck, matchType, binding);
          break;
        case PATTERN_BIND_MAP_REST:
          bindingType = typeForMapRest(c->typecheck, matchType, binding);
          break;
      }
    }
    switch (binding->kind) {
      case PATTERN_BIND_PATH:
        emitPatternValueSteps(c, switchValue, binding->steps, binding->stepCount, binding->name);
        break;
      case PATTERN_BIND_ARRAY_REST: {
        emitPatternValueSteps(c, switchValue, binding->steps, binding->stepCount, binding->name);
        int arrayTemp = emitTempNameConstant(c, "rest_arr");
        emitDefineVarConstant(c, arrayTemp);
        int restFn = emitStringConstantFromChars(c, "arrayRest", 9);
        emitGetVarConstant(c, restFn);
        emitGetVarConstant(c, arrayTemp);
        emitConstant(c, NUMBER_VAL((double)binding->restIndex), binding->name);
        emitByte(c, OP_CALL, binding->name);
        emitByte(c, 2, binding->name);
        break;
      }
      case PATTERN_BIND_MAP_REST: {
        emitPatternValueSteps(c, switchValue, binding->steps, binding->stepCount, binding->name);
        int mapTemp = emitTempNameConstant(c, "rest_map");
        emitDefineVarConstant(c, mapTemp);
        emitPatternRestKeyArray(c, binding);
        int keysTemp = emitTempNameConstant(c, "rest_keys");
        emitDefineVarConstant(c, keysTemp);
        int restFn = emitStringConstantFromChars(c, "mapRest", 7);
        emitGetVarConstant(c, restFn);
        emitGetVarConstant(c, mapTemp);
        emitGetVarConstant(c, keysTemp);
        emitByte(c, OP_CALL, binding->name);
        emitByte(c, 2, binding->name);
        break;
      }
    }
    int nameIdx = emitStringConstant(c, binding->name);
    emitByte(c, defineOp, binding->name);
    emitShort(c, (uint16_t)nameIdx, binding->name);
    if (defineOp == OP_SET_VAR) {
      emitByte(c, OP_POP, binding->name);
    }
    if (typecheckEnabled(c)) {
      if (defineOp == OP_SET_VAR) {
        typeAssign(c, binding->name, bindingType);
      } else {
        typeDefine(c, binding->name, bindingType ? bindingType : typeAny(), true);
      }
    }
  }
}


#include "singlepass.h"
#include "chunk.h"
#include "common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  int* offsets;
  int count;
  int capacity;
} JumpList;

typedef enum {
  BREAK_LOOP,
  BREAK_SWITCH
} BreakContextType;

typedef struct BreakContext {
  BreakContextType type;
  struct BreakContext* enclosing;
  int scopeDepth;
  JumpList breaks;
  JumpList continues;
} BreakContext;

typedef struct Compiler {
  VM* vm;
  const TokenArray* tokens;
  const char* source;
  const char* path;
  int current;
  bool panicMode;
  bool hadError;
  Chunk* chunk;
  int scopeDepth;
  int tempIndex;
  BreakContext* breakContext;
  struct Compiler* enclosing;
} Compiler;

typedef enum {
  CONST_NULL,
  CONST_BOOL,
  CONST_NUMBER,
  CONST_STRING
} ConstType;

typedef struct {
  ConstType type;
  bool ownsString;
  union {
    bool boolean;
    double number;
    struct { const char* chars; int length; } string;
  } as;
} ConstValue;

static void constValueFree(ConstValue* v) {
  if (v->type == CONST_STRING && v->ownsString) {
    free((void*)v->as.string.chars);
    v->ownsString = false;
  }
}

static bool constValueIsTruthy(const ConstValue* v) {
  if (v->type == CONST_NULL) return false;
  if (v->type == CONST_BOOL) return v->as.boolean;
  return true;
}

static bool isAtEnd(Compiler* c) {
  return c->tokens->tokens[c->current].type == TOKEN_EOF;
}

static Token peek(Compiler* c) {
  return c->tokens->tokens[c->current];
}

static Token previous(Compiler* c) {
  return c->tokens->tokens[c->current - 1];
}

static Token advance(Compiler* c) {
  if (!isAtEnd(c)) c->current++;
  return previous(c);
}

static bool check(Compiler* c, ErkaoTokenType type) {
  if (isAtEnd(c)) return false;
  return peek(c).type == type;
}

static bool match(Compiler* c, ErkaoTokenType type) {
  if (!check(c, type)) return false;
  advance(c);
  return true;
}

static Token noToken(void) {
  Token t;
  memset(&t, 0, sizeof(Token));
  return t;
}

static void errorAt(Compiler* c, Token token, const char* message) {
  if (c->panicMode) return;
  c->panicMode = true;
  c->hadError = true;
  const char* path = c->path ? c->path : "<repl>";
  fprintf(stderr, "%s:%d:%d: Error", path, token.line, token.column);
  if (token.type == TOKEN_EOF) {
    fprintf(stderr, " at end");
  } else if (token.type != TOKEN_ERROR) {
    fprintf(stderr, " at '%.*s'", token.length, token.start);
  }
  fprintf(stderr, ": %s\n", message);
  printErrorContext(c->source, token.line, token.column,
                    token.length > 0 ? token.length : 1);
}

static void errorAtCurrent(Compiler* c, const char* message) {
  errorAt(c, peek(c), message);
}

static Token consume(Compiler* c, ErkaoTokenType type, const char* message) {
  if (check(c, type)) return advance(c);
  if (type == TOKEN_SEMICOLON && c->current > 0) {
    Token token = previous(c);
    if (token.length > 0) token.column += token.length;
    errorAt(c, token, message);
  } else {
    errorAtCurrent(c, message);
  }
  return peek(c);
}

static void synchronize(Compiler* c) {
  c->panicMode = false;
  while (!isAtEnd(c)) {
    if (previous(c).type == TOKEN_SEMICOLON) return;
    switch (peek(c).type) {
      case TOKEN_CLASS: case TOKEN_FUN: case TOKEN_LET: case TOKEN_IMPORT:
      case TOKEN_FROM: case TOKEN_IF: case TOKEN_WHILE: case TOKEN_FOR:
      case TOKEN_FOREACH: case TOKEN_SWITCH: case TOKEN_RETURN:
      case TOKEN_BREAK: case TOKEN_CONTINUE:
        return;
      default:
        break;
    }
    advance(c);
  }
}

static void emitByte(Compiler* c, uint8_t byte, Token token) {
  writeChunk(c->chunk, byte, token);
}

static void emitBytes(Compiler* c, uint8_t a, uint8_t b, Token token) {
  emitByte(c, a, token);
  emitByte(c, b, token);
}

static void emitShort(Compiler* c, uint16_t value, Token token) {
  emitByte(c, (uint8_t)((value >> 8) & 0xff), token);
  emitByte(c, (uint8_t)(value & 0xff), token);
}

static int makeConstant(Compiler* c, Value value, Token token) {
  int index = addConstant(c->chunk, value);
  if (index > UINT16_MAX) {
    errorAt(c, token, "Too many constants in chunk.");
    return 0;
  }
  return index;
}

static void emitConstant(Compiler* c, Value value, Token token) {
  int constant = makeConstant(c, value, token);
  emitByte(c, OP_CONSTANT, token);
  emitShort(c, (uint16_t)constant, token);
}

static int emitJump(Compiler* c, uint8_t instruction, Token token) {
  emitByte(c, instruction, token);
  emitByte(c, 0xff, token);
  emitByte(c, 0xff, token);
  return c->chunk->count - 2;
}

static void patchJump(Compiler* c, int offset, Token token) {
  int jump = c->chunk->count - offset - 2;
  if (jump > UINT16_MAX) {
    errorAt(c, token, "Too much code to jump over.");
    return;
  }
  c->chunk->code[offset] = (uint8_t)((jump >> 8) & 0xff);
  c->chunk->code[offset + 1] = (uint8_t)(jump & 0xff);
}

static void emitLoop(Compiler* c, int loopStart, Token token) {
  emitByte(c, OP_LOOP, token);
  int offset = c->chunk->count - loopStart + 2;
  if (offset > UINT16_MAX) {
    errorAt(c, token, "Loop body too large.");
    return;
  }
  emitShort(c, (uint16_t)offset, token);
}

static int emitStringConstant(Compiler* c, Token token) {
  ObjString* name = stringFromToken(c->vm, token);
  return makeConstant(c, OBJ_VAL(name), token);
}

static int emitStringConstantFromChars(Compiler* c, const char* chars, int len) {
  ObjString* name = copyStringWithLength(c->vm, chars, len);
  return makeConstant(c, OBJ_VAL(name), noToken());
}

static int emitTempNameConstant(Compiler* c, const char* prefix) {
  char buffer[64];
  int length = snprintf(buffer, sizeof(buffer), "__%s%d", prefix, c->tempIndex++);
  if (length < 0) length = 0;
  if (length >= (int)sizeof(buffer)) length = (int)sizeof(buffer) - 1;
  return emitStringConstantFromChars(c, buffer, length);
}

static void emitGetVarConstant(Compiler* c, int idx) {
  emitByte(c, OP_GET_VAR, noToken());
  emitShort(c, (uint16_t)idx, noToken());
}

static void emitSetVarConstant(Compiler* c, int idx) {
  emitByte(c, OP_SET_VAR, noToken());
  emitShort(c, (uint16_t)idx, noToken());
}

static void emitDefineVarConstant(Compiler* c, int idx) {
  emitByte(c, OP_DEFINE_VAR, noToken());
  emitShort(c, (uint16_t)idx, noToken());
}

static void emitGc(Compiler* c) {
  emitByte(c, OP_GC, noToken());
}

static void initJumpList(JumpList* list) {
  list->offsets = NULL;
  list->count = 0;
  list->capacity = 0;
}

static void writeJumpList(JumpList* list, int offset) {
  if (list->capacity < list->count + 1) {
    int oldCap = list->capacity;
    list->capacity = GROW_CAPACITY(oldCap);
    list->offsets = GROW_ARRAY(int, list->offsets, oldCap, list->capacity);
  }
  list->offsets[list->count++] = offset;
}

static void freeJumpList(JumpList* list) {
  FREE_ARRAY(int, list->offsets, list->capacity);
  initJumpList(list);
}

static void patchJumpTo(Compiler* c, int offset, int target, Token token) {
  int jump = target - offset - 2;
  if (jump < 0 || jump > UINT16_MAX) {
    errorAt(c, token, "Too much code to jump over.");
    return;
  }
  c->chunk->code[offset] = (uint8_t)((jump >> 8) & 0xff);
  c->chunk->code[offset + 1] = (uint8_t)(jump & 0xff);
}

static void patchJumpList(Compiler* c, JumpList* list, int target, Token token) {
  for (int i = 0; i < list->count; i++) {
    patchJumpTo(c, list->offsets[i], target, token);
  }
}

static void emitScopeExits(Compiler* c, int targetDepth) {
  for (int depth = c->scopeDepth; depth > targetDepth; depth--) {
    emitByte(c, OP_END_SCOPE, noToken());
  }
}

static BreakContext* findLoopContext(Compiler* c) {
  for (BreakContext* ctx = c->breakContext; ctx; ctx = ctx->enclosing) {
    if (ctx->type == BREAK_LOOP) return ctx;
  }
  return NULL;
}

static char* copyTokenLexeme(Token token) {
  char* buffer = (char*)malloc((size_t)token.length + 1);
  if (!buffer) { fprintf(stderr, "Out of memory.\n"); exit(1); }
  memcpy(buffer, token.start, (size_t)token.length);
  buffer[token.length] = '\0';
  return buffer;
}

static char* parseStringLiteral(Token token) {
  int length = token.length - 2;
  if (length < 0) length = 0;
  const char* src = token.start + 1;
  char* buffer = (char*)malloc((size_t)length + 1);
  if (!buffer) { fprintf(stderr, "Out of memory.\n"); exit(1); }
  int out = 0;
  for (int i = 0; i < length; i++) {
    char ch = src[i];
    if (ch == '\\' && i + 1 < length) {
      char next = src[++i];
      switch (next) {
        case 'n': buffer[out++] = '\n'; break;
        case 't': buffer[out++] = '\t'; break;
        case 'r': buffer[out++] = '\r'; break;
        case '"': buffer[out++] = '"'; break;
        case '\\': buffer[out++] = '\\'; break;
        default: buffer[out++] = next; break;
      }
    } else {
      buffer[out++] = ch;
    }
  }
  buffer[out] = '\0';
  return buffer;
}

static void expression(Compiler* c);
static void declaration(Compiler* c);
static void statement(Compiler* c);
static void block(Compiler* c);

typedef enum {
  PREC_NONE,
  PREC_ASSIGNMENT,
  PREC_OR,
  PREC_AND,
  PREC_EQUALITY,
  PREC_COMPARISON,
  PREC_TERM,
  PREC_FACTOR,
  PREC_UNARY,
  PREC_CALL,
  PREC_PRIMARY
} Precedence;

typedef void (*ParseFn)(Compiler* c, bool canAssign);

typedef struct {
  ParseFn prefix;
  ParseFn infix;
  Precedence precedence;
} ParseRule;

static void parsePrecedence(Compiler* c, Precedence prec);
static ParseRule* getRule(ErkaoTokenType type);

static void number(Compiler* c, bool canAssign) {
  (void)canAssign;
  Token token = previous(c);
  char* temp = copyTokenLexeme(token);
  double value = strtod(temp, NULL);
  free(temp);
  emitConstant(c, NUMBER_VAL(value), token);
}

static void string(Compiler* c, bool canAssign) {
  (void)canAssign;
  Token token = previous(c);
  char* value = parseStringLiteral(token);
  ObjString* str = takeStringWithLength(c->vm, value, (int)strlen(value));
  emitConstant(c, OBJ_VAL(str), token);
}

static void literal(Compiler* c, bool canAssign) {
  (void)canAssign;
  Token token = previous(c);
  switch (token.type) {
    case TOKEN_FALSE: emitByte(c, OP_FALSE, token); break;
    case TOKEN_TRUE: emitByte(c, OP_TRUE, token); break;
    case TOKEN_NULL: emitByte(c, OP_NULL, token); break;
    default: break;
  }
}

static void variable(Compiler* c, bool canAssign) {
  Token name = previous(c);
  int nameIdx = emitStringConstant(c, name);
  if (canAssign && match(c, TOKEN_EQUAL)) {
    expression(c);
    emitByte(c, OP_SET_VAR, name);
    emitShort(c, (uint16_t)nameIdx, name);
  } else {
    emitByte(c, OP_GET_VAR, name);
    emitShort(c, (uint16_t)nameIdx, name);
  }
}

static void thisExpr(Compiler* c, bool canAssign) {
  (void)canAssign;
  Token token = previous(c);
  int name = emitStringConstant(c, token);
  emitByte(c, OP_GET_THIS, token);
  emitShort(c, (uint16_t)name, token);
}

static void grouping(Compiler* c, bool canAssign) {
  (void)canAssign;
  expression(c);
  consume(c, TOKEN_RIGHT_PAREN, "Expect ')' after expression.");
}

static void unary(Compiler* c, bool canAssign) {
  (void)canAssign;
  Token op = previous(c);
  parsePrecedence(c, PREC_UNARY);
  switch (op.type) {
    case TOKEN_MINUS: emitByte(c, OP_NEGATE, op); break;
    case TOKEN_BANG: emitByte(c, OP_NOT, op); break;
    default: break;
  }
}

static void binary(Compiler* c, bool canAssign) {
  (void)canAssign;
  Token op = previous(c);
  ParseRule* rule = getRule(op.type);
  parsePrecedence(c, (Precedence)(rule->precedence + 1));
  switch (op.type) {
    case TOKEN_PLUS: emitByte(c, OP_ADD, op); break;
    case TOKEN_MINUS: emitByte(c, OP_SUBTRACT, op); break;
    case TOKEN_STAR: emitByte(c, OP_MULTIPLY, op); break;
    case TOKEN_SLASH: emitByte(c, OP_DIVIDE, op); break;
    case TOKEN_GREATER: emitByte(c, OP_GREATER, op); break;
    case TOKEN_GREATER_EQUAL: emitByte(c, OP_GREATER_EQUAL, op); break;
    case TOKEN_LESS: emitByte(c, OP_LESS, op); break;
    case TOKEN_LESS_EQUAL: emitByte(c, OP_LESS_EQUAL, op); break;
    case TOKEN_BANG_EQUAL: emitBytes(c, OP_EQUAL, OP_NOT, op); break;
    case TOKEN_EQUAL_EQUAL: emitByte(c, OP_EQUAL, op); break;
    default: break;
  }
}

static void andExpr(Compiler* c, bool canAssign) {
  (void)canAssign;
  Token op = previous(c);
  int jumpIfFalse = emitJump(c, OP_JUMP_IF_FALSE, op);
  emitByte(c, OP_POP, noToken());
  parsePrecedence(c, PREC_AND);
  patchJump(c, jumpIfFalse, op);
}

static void orExpr(Compiler* c, bool canAssign) {
  (void)canAssign;
  Token op = previous(c);
  int jumpIfFalse = emitJump(c, OP_JUMP_IF_FALSE, op);
  int jumpToEnd = emitJump(c, OP_JUMP, op);
  patchJump(c, jumpIfFalse, op);
  emitByte(c, OP_POP, noToken());
  parsePrecedence(c, PREC_OR);
  patchJump(c, jumpToEnd, op);
}

static void call(Compiler* c, bool canAssign) {
  (void)canAssign;
  Token paren = previous(c);
  int argc = 0;
  if (!check(c, TOKEN_RIGHT_PAREN)) {
    do {
      if (argc >= ERK_MAX_ARGS) {
        errorAtCurrent(c, "Too many arguments.");
      }
      expression(c);
      argc++;
    } while (match(c, TOKEN_COMMA));
  }
  consume(c, TOKEN_RIGHT_PAREN, "Expect ')' after arguments.");
  emitByte(c, OP_CALL, paren);
  emitByte(c, (uint8_t)argc, paren);
}

static void dot(Compiler* c, bool canAssign) {
  Token name = consume(c, TOKEN_IDENTIFIER, "Expect property name after '.'.");
  int nameIdx = emitStringConstant(c, name);
  if (canAssign && match(c, TOKEN_EQUAL)) {
    expression(c);
    emitByte(c, OP_SET_PROPERTY, name);
    emitShort(c, (uint16_t)nameIdx, name);
  } else {
    emitByte(c, OP_GET_PROPERTY, name);
    emitShort(c, (uint16_t)nameIdx, name);
  }
}

static void index_(Compiler* c, bool canAssign) {
  Token bracket = previous(c);
  expression(c);
  consume(c, TOKEN_RIGHT_BRACKET, "Expect ']' after index.");
  if (canAssign && match(c, TOKEN_EQUAL)) {
    expression(c);
    emitByte(c, OP_SET_INDEX, bracket);
  } else {
    emitByte(c, OP_GET_INDEX, bracket);
  }
}

static void array(Compiler* c, bool canAssign) {
  (void)canAssign;
  int count = 0;
  emitByte(c, OP_ARRAY, noToken());
  emitShort(c, 0, noToken());
  int sizeOffset = c->chunk->count - 2;
  if (!check(c, TOKEN_RIGHT_BRACKET)) {
    do {
      expression(c);
      emitByte(c, OP_ARRAY_APPEND, noToken());
      count++;
    } while (match(c, TOKEN_COMMA));
  }
  consume(c, TOKEN_RIGHT_BRACKET, "Expect ']' after array literal.");
  c->chunk->code[sizeOffset] = (uint8_t)((count >> 8) & 0xff);
  c->chunk->code[sizeOffset + 1] = (uint8_t)(count & 0xff);
}

static void map(Compiler* c, bool canAssign) {
  (void)canAssign;
  int count = 0;
  emitByte(c, OP_MAP, noToken());
  emitShort(c, 0, noToken());
  int sizeOffset = c->chunk->count - 2;
  if (!check(c, TOKEN_RIGHT_BRACE)) {
    do {
      if (match(c, TOKEN_IDENTIFIER)) {
        Token key = previous(c);
        char* keyName = copyTokenLexeme(key);
        ObjString* keyStr = takeStringWithLength(c->vm, keyName, key.length);
        emitConstant(c, OBJ_VAL(keyStr), key);
      } else if (match(c, TOKEN_STRING)) {
        Token key = previous(c);
        char* keyName = parseStringLiteral(key);
        ObjString* keyStr = takeStringWithLength(c->vm, keyName, (int)strlen(keyName));
        emitConstant(c, OBJ_VAL(keyStr), key);
      } else {
        errorAtCurrent(c, "Map keys must be identifiers or strings.");
        break;
      }
      consume(c, TOKEN_COLON, "Expect ':' after map key.");
      expression(c);
      emitByte(c, OP_MAP_SET, noToken());
      count++;
    } while (match(c, TOKEN_COMMA));
  }
  consume(c, TOKEN_RIGHT_BRACE, "Expect '}' after map literal.");
  c->chunk->code[sizeOffset] = (uint8_t)((count >> 8) & 0xff);
  c->chunk->code[sizeOffset + 1] = (uint8_t)(count & 0xff);
}

static ParseRule rules[TOKEN_EOF + 1];
static bool rulesInitialized = false;

static void initRules(void) {
  if (rulesInitialized) return;
  rulesInitialized = true;
  for (int i = 0; i <= TOKEN_EOF; i++) {
    rules[i] = (ParseRule){NULL, NULL, PREC_NONE};
  }
  rules[TOKEN_LEFT_PAREN] = (ParseRule){grouping, call, PREC_CALL};
  rules[TOKEN_LEFT_BRACKET] = (ParseRule){array, index_, PREC_CALL};
  rules[TOKEN_LEFT_BRACE] = (ParseRule){map, NULL, PREC_NONE};
  rules[TOKEN_DOT] = (ParseRule){NULL, dot, PREC_CALL};
  rules[TOKEN_MINUS] = (ParseRule){unary, binary, PREC_TERM};
  rules[TOKEN_PLUS] = (ParseRule){NULL, binary, PREC_TERM};
  rules[TOKEN_SLASH] = (ParseRule){NULL, binary, PREC_FACTOR};
  rules[TOKEN_STAR] = (ParseRule){NULL, binary, PREC_FACTOR};
  rules[TOKEN_BANG] = (ParseRule){unary, NULL, PREC_NONE};
  rules[TOKEN_BANG_EQUAL] = (ParseRule){NULL, binary, PREC_EQUALITY};
  rules[TOKEN_EQUAL_EQUAL] = (ParseRule){NULL, binary, PREC_EQUALITY};
  rules[TOKEN_GREATER] = (ParseRule){NULL, binary, PREC_COMPARISON};
  rules[TOKEN_GREATER_EQUAL] = (ParseRule){NULL, binary, PREC_COMPARISON};
  rules[TOKEN_LESS] = (ParseRule){NULL, binary, PREC_COMPARISON};
  rules[TOKEN_LESS_EQUAL] = (ParseRule){NULL, binary, PREC_COMPARISON};
  rules[TOKEN_IDENTIFIER] = (ParseRule){variable, NULL, PREC_NONE};
  rules[TOKEN_STRING] = (ParseRule){string, NULL, PREC_NONE};
  rules[TOKEN_NUMBER] = (ParseRule){number, NULL, PREC_NONE};
  rules[TOKEN_AND] = (ParseRule){NULL, andExpr, PREC_AND};
  rules[TOKEN_OR] = (ParseRule){NULL, orExpr, PREC_OR};
  rules[TOKEN_FALSE] = (ParseRule){literal, NULL, PREC_NONE};
  rules[TOKEN_TRUE] = (ParseRule){literal, NULL, PREC_NONE};
  rules[TOKEN_NULL] = (ParseRule){literal, NULL, PREC_NONE};
  rules[TOKEN_THIS] = (ParseRule){thisExpr, NULL, PREC_NONE};
}

static ParseRule* getRule(ErkaoTokenType type) {
  return &rules[type];
}

static void parsePrecedence(Compiler* c, Precedence prec) {
  advance(c);
  ParseFn prefixRule = getRule(previous(c).type)->prefix;
  if (prefixRule == NULL) {
    errorAt(c, previous(c), "Expect expression.");
    return;
  }
  bool canAssign = prec <= PREC_ASSIGNMENT;
  prefixRule(c, canAssign);

  while (prec <= getRule(peek(c).type)->precedence) {
    advance(c);
    ParseFn infixRule = getRule(previous(c).type)->infix;
    if (infixRule != NULL) {
      infixRule(c, canAssign);
    }
  }

  if (canAssign && match(c, TOKEN_EQUAL)) {
    errorAt(c, previous(c), "Invalid assignment target.");
  }
}

static void expression(Compiler* c) {
  parsePrecedence(c, PREC_ASSIGNMENT);
}

static void expressionStatement(Compiler* c) {
  expression(c);
  consume(c, TOKEN_SEMICOLON, "Expect ';' after expression.");
  emitByte(c, OP_POP, noToken());
  emitGc(c);
}

static void varDeclaration(Compiler* c) {
  Token name = consume(c, TOKEN_IDENTIFIER, "Expect variable name.");
  if (match(c, TOKEN_EQUAL)) {
    expression(c);
  } else {
    emitByte(c, OP_NULL, noToken());
  }
  consume(c, TOKEN_SEMICOLON, "Expect ';' after variable declaration.");
  int nameIdx = emitStringConstant(c, name);
  emitByte(c, OP_DEFINE_VAR, name);
  emitShort(c, (uint16_t)nameIdx, name);
  emitGc(c);
}

static void block(Compiler* c) {
  while (!check(c, TOKEN_RIGHT_BRACE) && !isAtEnd(c)) {
    declaration(c);
  }
  consume(c, TOKEN_RIGHT_BRACE, "Expect '}' after block.");
}

static void blockStatement(Compiler* c) {
  emitByte(c, OP_BEGIN_SCOPE, noToken());
  c->scopeDepth++;
  block(c);
  emitByte(c, OP_END_SCOPE, noToken());
  c->scopeDepth--;
  emitGc(c);
}

static void ifStatement(Compiler* c) {
  Token keyword = previous(c);
  consume(c, TOKEN_LEFT_PAREN, "Expect '(' after 'if'.");
  expression(c);
  consume(c, TOKEN_RIGHT_PAREN, "Expect ')' after if condition.");
  int thenJump = emitJump(c, OP_JUMP_IF_FALSE, keyword);
  emitByte(c, OP_POP, noToken());
  statement(c);
  if (match(c, TOKEN_ELSE)) {
    int elseJump = emitJump(c, OP_JUMP, keyword);
    patchJump(c, thenJump, keyword);
    emitByte(c, OP_POP, noToken());
    statement(c);
    patchJump(c, elseJump, keyword);
  } else {
    patchJump(c, thenJump, keyword);
    emitByte(c, OP_POP, noToken());
  }
  emitGc(c);
}

static void whileStatement(Compiler* c) {
  Token keyword = previous(c);
  int loopStart = c->chunk->count;
  consume(c, TOKEN_LEFT_PAREN, "Expect '(' after 'while'.");
  expression(c);
  consume(c, TOKEN_RIGHT_PAREN, "Expect ')' after condition.");
  int exitJump = emitJump(c, OP_JUMP_IF_FALSE, keyword);
  emitByte(c, OP_POP, noToken());

  BreakContext loop;
  loop.type = BREAK_LOOP;
  loop.enclosing = c->breakContext;
  loop.scopeDepth = c->scopeDepth;
  initJumpList(&loop.breaks);
  initJumpList(&loop.continues);
  c->breakContext = &loop;

  statement(c);
  int continueTarget = c->chunk->count;
  emitGc(c);
  emitLoop(c, loopStart, keyword);
  c->breakContext = loop.enclosing;

  patchJump(c, exitJump, keyword);
  emitByte(c, OP_POP, noToken());
  emitGc(c);
  int loopEnd = c->chunk->count;
  patchJumpList(c, &loop.breaks, loopEnd, keyword);
  patchJumpList(c, &loop.continues, continueTarget, keyword);
  freeJumpList(&loop.breaks);
  freeJumpList(&loop.continues);
}

static void forStatement(Compiler* c) {
  Token keyword = previous(c);
  emitByte(c, OP_BEGIN_SCOPE, noToken());
  c->scopeDepth++;
  consume(c, TOKEN_LEFT_PAREN, "Expect '(' after 'for'.");

  if (match(c, TOKEN_SEMICOLON)) {
  } else if (match(c, TOKEN_LET)) {
    varDeclaration(c);
  } else {
    expression(c);
    consume(c, TOKEN_SEMICOLON, "Expect ';' after loop initializer.");
    emitByte(c, OP_POP, noToken());
  }

  int loopStart = c->chunk->count;
  int exitJump = -1;
  if (!check(c, TOKEN_SEMICOLON)) {
    expression(c);
    exitJump = emitJump(c, OP_JUMP_IF_FALSE, keyword);
    emitByte(c, OP_POP, noToken());
  }
  consume(c, TOKEN_SEMICOLON, "Expect ';' after loop condition.");

  int incrementOffset = -1;
  bool hasIncrement = !check(c, TOKEN_RIGHT_PAREN);
  if (hasIncrement) {
    int bodyJump = emitJump(c, OP_JUMP, keyword);
    incrementOffset = c->chunk->count;
    expression(c);
    emitByte(c, OP_POP, noToken());
    emitLoop(c, loopStart, keyword);
    loopStart = incrementOffset;
    patchJump(c, bodyJump, keyword);
  }
  consume(c, TOKEN_RIGHT_PAREN, "Expect ')' after for clauses.");

  BreakContext loop;
  loop.type = BREAK_LOOP;
  loop.enclosing = c->breakContext;
  loop.scopeDepth = c->scopeDepth;
  initJumpList(&loop.breaks);
  initJumpList(&loop.continues);
  c->breakContext = &loop;

  statement(c);
  int continueTarget = hasIncrement ? incrementOffset : c->chunk->count;
  emitGc(c);
  emitLoop(c, loopStart, keyword);
  c->breakContext = loop.enclosing;

  if (exitJump != -1) {
    patchJump(c, exitJump, keyword);
    emitByte(c, OP_POP, noToken());
  }
  emitGc(c);
  int loopEnd = c->chunk->count;
  patchJumpList(c, &loop.breaks, loopEnd, keyword);
  patchJumpList(c, &loop.continues, continueTarget, keyword);
  freeJumpList(&loop.breaks);
  freeJumpList(&loop.continues);

  emitByte(c, OP_END_SCOPE, noToken());
  c->scopeDepth--;
  emitGc(c);
}

static void foreachStatement(Compiler* c) {
  Token keyword = previous(c);
  emitByte(c, OP_BEGIN_SCOPE, noToken());
  c->scopeDepth++;
  consume(c, TOKEN_LEFT_PAREN, "Expect '(' after 'foreach'.");

  Token first = consume(c, TOKEN_IDENTIFIER, "Expect loop variable.");
  Token keyToken; Token valueToken;
  memset(&keyToken, 0, sizeof(Token));
  valueToken = first;
  bool hasKey = false;
  if (match(c, TOKEN_COMMA)) {
    keyToken = first;
    valueToken = consume(c, TOKEN_IDENTIFIER, "Expect value name after ','.");
    hasKey = true;
  }
  consume(c, TOKEN_IN, "Expect 'in' after foreach variable.");
  expression(c);
  consume(c, TOKEN_RIGHT_PAREN, "Expect ')' after foreach iterable.");

  int iterName = emitTempNameConstant(c, "iter");
  emitDefineVarConstant(c, iterName);

  int collectionName = iterName;
  if (hasKey) {
    int keysFn = emitStringConstantFromChars(c, "keys", 4);
    emitGetVarConstant(c, keysFn);
    emitGetVarConstant(c, iterName);
    emitByte(c, OP_CALL, noToken());
    emitByte(c, 1, noToken());
    int keysName = emitTempNameConstant(c, "keys");
    emitDefineVarConstant(c, keysName);
    collectionName = keysName;
  }

  int indexName = emitTempNameConstant(c, "i");
  emitConstant(c, NUMBER_VAL(0), noToken());
  emitDefineVarConstant(c, indexName);

  int lenFn = emitStringConstantFromChars(c, "len", 3);
  int loopStart = c->chunk->count;
  emitGetVarConstant(c, indexName);
  emitGetVarConstant(c, lenFn);
  emitGetVarConstant(c, collectionName);
  emitByte(c, OP_CALL, noToken());
  emitByte(c, 1, noToken());
  emitByte(c, OP_LESS, keyword);
  int exitJump = emitJump(c, OP_JUMP_IF_FALSE, keyword);
  emitByte(c, OP_POP, noToken());

  BreakContext loop;
  loop.type = BREAK_LOOP;
  loop.enclosing = c->breakContext;
  loop.scopeDepth = c->scopeDepth;
  initJumpList(&loop.breaks);
  initJumpList(&loop.continues);
  c->breakContext = &loop;

  if (hasKey) {
    int keyName = emitStringConstant(c, keyToken);
    int valueName = emitStringConstant(c, valueToken);
    emitGetVarConstant(c, collectionName);
    emitGetVarConstant(c, indexName);
    emitByte(c, OP_GET_INDEX, keyToken);
    emitByte(c, OP_DEFINE_VAR, keyToken);
    emitShort(c, (uint16_t)keyName, keyToken);
    emitGetVarConstant(c, iterName);
    emitByte(c, OP_GET_VAR, keyToken);
    emitShort(c, (uint16_t)keyName, keyToken);
    emitByte(c, OP_GET_INDEX, valueToken);
    emitByte(c, OP_DEFINE_VAR, valueToken);
    emitShort(c, (uint16_t)valueName, valueToken);
  } else {
    int valueName = emitStringConstant(c, valueToken);
    emitGetVarConstant(c, iterName);
    emitGetVarConstant(c, indexName);
    emitByte(c, OP_GET_INDEX, valueToken);
    emitByte(c, OP_DEFINE_VAR, valueToken);
    emitShort(c, (uint16_t)valueName, valueToken);
  }

  statement(c);
  int continueTarget = c->chunk->count;
  emitGetVarConstant(c, indexName);
  emitConstant(c, NUMBER_VAL(1), noToken());
  emitByte(c, OP_ADD, noToken());
  emitSetVarConstant(c, indexName);
  emitByte(c, OP_POP, noToken());
  emitGc(c);
  emitLoop(c, loopStart, keyword);
  c->breakContext = loop.enclosing;

  patchJump(c, exitJump, keyword);
  emitByte(c, OP_POP, noToken());
  emitGc(c);
  int loopEnd = c->chunk->count;
  patchJumpList(c, &loop.breaks, loopEnd, keyword);
  patchJumpList(c, &loop.continues, continueTarget, keyword);
  freeJumpList(&loop.breaks);
  freeJumpList(&loop.continues);

  emitByte(c, OP_END_SCOPE, noToken());
  c->scopeDepth--;
  emitGc(c);
}

static void switchStatement(Compiler* c) {
  Token keyword = previous(c);
  emitByte(c, OP_BEGIN_SCOPE, noToken());
  c->scopeDepth++;
  consume(c, TOKEN_LEFT_PAREN, "Expect '(' after 'switch'.");
  expression(c);
  consume(c, TOKEN_RIGHT_PAREN, "Expect ')' after switch value.");
  consume(c, TOKEN_LEFT_BRACE, "Expect '{' after switch value.");

  int switchValue = emitTempNameConstant(c, "switch");
  emitDefineVarConstant(c, switchValue);

  BreakContext ctx;
  ctx.type = BREAK_SWITCH;
  ctx.enclosing = c->breakContext;
  ctx.scopeDepth = c->scopeDepth;
  initJumpList(&ctx.breaks);
  initJumpList(&ctx.continues);
  c->breakContext = &ctx;

  JumpList endJumps;
  initJumpList(&endJumps);
  int previousJump = -1;

  while (!check(c, TOKEN_RIGHT_BRACE) && !isAtEnd(c)) {
    if (match(c, TOKEN_CASE)) {
      if (previousJump != -1) {
        patchJump(c, previousJump, keyword);
        emitByte(c, OP_POP, noToken());
      }
      emitGetVarConstant(c, switchValue);
      expression(c);
      consume(c, TOKEN_COLON, "Expect ':' after case value.");
      emitByte(c, OP_EQUAL, keyword);
      previousJump = emitJump(c, OP_JUMP_IF_FALSE, keyword);
      emitByte(c, OP_POP, noToken());

      while (!check(c, TOKEN_CASE) && !check(c, TOKEN_DEFAULT) &&
             !check(c, TOKEN_RIGHT_BRACE) && !isAtEnd(c)) {
        declaration(c);
      }
      int endJump = emitJump(c, OP_JUMP, keyword);
      writeJumpList(&endJumps, endJump);
    } else if (match(c, TOKEN_DEFAULT)) {
      if (previousJump != -1) {
        patchJump(c, previousJump, keyword);
        emitByte(c, OP_POP, noToken());
        previousJump = -1;
      }
      consume(c, TOKEN_COLON, "Expect ':' after default.");
      while (!check(c, TOKEN_CASE) && !check(c, TOKEN_DEFAULT) &&
             !check(c, TOKEN_RIGHT_BRACE) && !isAtEnd(c)) {
        declaration(c);
      }
    } else {
      errorAtCurrent(c, "Expect 'case' or 'default' in switch.");
      synchronize(c);
      break;
    }
  }

  if (previousJump != -1) {
    patchJump(c, previousJump, keyword);
    emitByte(c, OP_POP, noToken());
  }

  consume(c, TOKEN_RIGHT_BRACE, "Expect '}' after switch cases.");
  c->breakContext = ctx.enclosing;
  int switchEnd = c->chunk->count;
  patchJumpList(c, &endJumps, switchEnd, keyword);
  patchJumpList(c, &ctx.breaks, switchEnd, keyword);
  freeJumpList(&endJumps);
  freeJumpList(&ctx.breaks);
  freeJumpList(&ctx.continues);

  emitByte(c, OP_END_SCOPE, noToken());
  c->scopeDepth--;
  emitGc(c);
}

static void breakStatement(Compiler* c) {
  Token keyword = previous(c);
  consume(c, TOKEN_SEMICOLON, "Expect ';' after 'break'.");
  if (!c->breakContext) {
    errorAt(c, keyword, "Cannot use 'break' outside of a loop or switch.");
    return;
  }
  emitScopeExits(c, c->breakContext->scopeDepth);
  int jump = emitJump(c, OP_JUMP, keyword);
  writeJumpList(&c->breakContext->breaks, jump);
}

static void continueStatement(Compiler* c) {
  Token keyword = previous(c);
  consume(c, TOKEN_SEMICOLON, "Expect ';' after 'continue'.");
  BreakContext* loop = findLoopContext(c);
  if (!loop) {
    errorAt(c, keyword, "Cannot use 'continue' outside of a loop.");
    return;
  }
  emitScopeExits(c, loop->scopeDepth);
  int jump = emitJump(c, OP_JUMP, keyword);
  writeJumpList(&loop->continues, jump);
}

static void returnStatement(Compiler* c) {
  Token keyword = previous(c);
  if (!check(c, TOKEN_SEMICOLON)) {
    expression(c);
  } else {
    emitByte(c, OP_NULL, noToken());
  }
  consume(c, TOKEN_SEMICOLON, "Expect ';' after return value.");
  emitByte(c, OP_RETURN, keyword);
}

static void importStatement(Compiler* c) {
  Token keyword = previous(c);
  expression(c);
  Token alias; memset(&alias, 0, sizeof(Token));
  bool hasAlias = false;
  if (match(c, TOKEN_AS)) {
    alias = consume(c, TOKEN_IDENTIFIER, "Expect name after 'as'.");
    hasAlias = true;
  }
  consume(c, TOKEN_SEMICOLON, "Expect ';' after import.");
  emitByte(c, OP_IMPORT, keyword);
  emitByte(c, hasAlias ? 1 : 0, keyword);
  uint16_t aliasIdx = 0;
  if (hasAlias) {
    aliasIdx = (uint16_t)emitStringConstant(c, alias);
  }
  emitShort(c, aliasIdx, keyword);
  emitGc(c);
}

static void fromImportStatement(Compiler* c) {
  Token keyword = previous(c);
  expression(c);
  consume(c, TOKEN_IMPORT, "Expect 'import' after module path.");
  Token alias = consume(c, TOKEN_IDENTIFIER, "Expect name after 'import'.");
  consume(c, TOKEN_SEMICOLON, "Expect ';' after import.");
  emitByte(c, OP_IMPORT, keyword);
  emitByte(c, 1, keyword);
  uint16_t aliasIdx = (uint16_t)emitStringConstant(c, alias);
  emitShort(c, aliasIdx, keyword);
  emitGc(c);
}

static ObjFunction* compileFunction(Compiler* c, Token name, bool isMethod);

static void functionDeclaration(Compiler* c) {
  Token name = consume(c, TOKEN_IDENTIFIER, "Expect function name.");
  ObjFunction* function = compileFunction(c, name, false);
  if (!function) return;
  int constant = makeConstant(c, OBJ_VAL(function), name);
  emitByte(c, OP_CLOSURE, name);
  emitShort(c, (uint16_t)constant, name);
  int nameIdx = emitStringConstant(c, name);
  emitByte(c, OP_DEFINE_VAR, name);
  emitShort(c, (uint16_t)nameIdx, name);
  emitGc(c);
}

static void classDeclaration(Compiler* c) {
  Token name = consume(c, TOKEN_IDENTIFIER, "Expect class name.");
  consume(c, TOKEN_LEFT_BRACE, "Expect '{' before class body.");

  int nameConst = emitStringConstant(c, name);
  emitByte(c, OP_NULL, noToken());
  emitByte(c, OP_DEFINE_VAR, name);
  emitShort(c, (uint16_t)nameConst, name);

  int methodCount = 0;
  while (!check(c, TOKEN_RIGHT_BRACE) && !isAtEnd(c)) {
    if (!match(c, TOKEN_FUN)) {
      errorAtCurrent(c, "Expect 'fun' before method declaration.");
      synchronize(c);
      break;
    }
    Token methodName = consume(c, TOKEN_IDENTIFIER, "Expect method name.");
    bool isInit = methodName.length == 4 && memcmp(methodName.start, "init", 4) == 0;
    ObjFunction* method = compileFunction(c, methodName, isInit);
    if (!method) return;
    int constant = makeConstant(c, OBJ_VAL(method), methodName);
    emitByte(c, OP_CLOSURE, methodName);
    emitShort(c, (uint16_t)constant, methodName);
    methodCount++;
  }
  consume(c, TOKEN_RIGHT_BRACE, "Expect '}' after class body.");

  emitByte(c, OP_CLASS, name);
  emitShort(c, (uint16_t)nameConst, name);
  emitShort(c, (uint16_t)methodCount, name);
  emitGc(c);
}

static void declaration(Compiler* c) {
  if (match(c, TOKEN_CLASS)) {
    classDeclaration(c);
  } else if (match(c, TOKEN_FUN)) {
    functionDeclaration(c);
  } else if (match(c, TOKEN_LET)) {
    varDeclaration(c);
  } else if (match(c, TOKEN_IMPORT)) {
    importStatement(c);
  } else if (match(c, TOKEN_FROM)) {
    fromImportStatement(c);
  } else {
    statement(c);
  }
  if (c->panicMode) synchronize(c);
}

static void statement(Compiler* c) {
  if (match(c, TOKEN_IF)) {
    ifStatement(c);
  } else if (match(c, TOKEN_WHILE)) {
    whileStatement(c);
  } else if (match(c, TOKEN_FOR)) {
    forStatement(c);
  } else if (match(c, TOKEN_FOREACH)) {
    foreachStatement(c);
  } else if (match(c, TOKEN_SWITCH)) {
    switchStatement(c);
  } else if (match(c, TOKEN_RETURN)) {
    returnStatement(c);
  } else if (match(c, TOKEN_BREAK)) {
    breakStatement(c);
  } else if (match(c, TOKEN_CONTINUE)) {
    continueStatement(c);
  } else if (match(c, TOKEN_LEFT_BRACE)) {
    blockStatement(c);
  } else {
    expressionStatement(c);
  }
}

static ObjFunction* compileFunction(Compiler* c, Token name, bool isInitializer) {
  consume(c, TOKEN_LEFT_PAREN, "Expect '(' after function name.");

  int arity = 0;
  int minArity = 0;
  ObjString** params = NULL;
  Token* paramTokens = NULL;
  int* defaultStarts = NULL;
  int* defaultEnds = NULL;
  bool sawDefault = false;

  int savedStart = c->current;
  int tempArity = 0;
  if (!check(c, TOKEN_RIGHT_PAREN)) {
    do {
      if (!check(c, TOKEN_IDENTIFIER)) {
        errorAtCurrent(c, "Expect parameter name.");
        break;
      }
      advance(c);
      tempArity++;
      if (match(c, TOKEN_EQUAL)) {
        sawDefault = true;
        int depth = 0;
        while (!isAtEnd(c)) {
          if (check(c, TOKEN_COMMA) && depth == 0) break;
          if (check(c, TOKEN_RIGHT_PAREN) && depth == 0) break;
          if (check(c, TOKEN_LEFT_PAREN) || check(c, TOKEN_LEFT_BRACKET) || check(c, TOKEN_LEFT_BRACE)) depth++;
          if (check(c, TOKEN_RIGHT_PAREN) || check(c, TOKEN_RIGHT_BRACKET) || check(c, TOKEN_RIGHT_BRACE)) depth--;
          advance(c);
        }
      }
    } while (match(c, TOKEN_COMMA));
  }
  consume(c, TOKEN_RIGHT_PAREN, "Expect ')' after parameters.");
  consume(c, TOKEN_LEFT_BRACE, "Expect '{' before function body.");
  int bodyStart = c->current;

  arity = tempArity;
  if (arity > 0) {
    params = (ObjString**)malloc(sizeof(ObjString*) * (size_t)arity);
    paramTokens = (Token*)malloc(sizeof(Token) * (size_t)arity);
    defaultStarts = (int*)malloc(sizeof(int) * (size_t)arity);
    defaultEnds = (int*)malloc(sizeof(int) * (size_t)arity);
    if (!params || !paramTokens || !defaultStarts || !defaultEnds) {
      fprintf(stderr, "Out of memory.\n");
      exit(1);
    }
    for (int i = 0; i < arity; i++) {
      defaultStarts[i] = -1;
      defaultEnds[i] = -1;
    }
  }

  c->current = savedStart;
  sawDefault = false;
  minArity = arity;
  int paramIdx = 0;
  if (!check(c, TOKEN_RIGHT_PAREN)) {
    do {
      if (paramIdx >= arity) break;
      Token paramName = consume(c, TOKEN_IDENTIFIER, "Expect parameter name.");
      params[paramIdx] = stringFromToken(c->vm, paramName);
      paramTokens[paramIdx] = paramName;
      if (match(c, TOKEN_EQUAL)) {
        if (!sawDefault) minArity = paramIdx;
        sawDefault = true;
        defaultStarts[paramIdx] = c->current;
        int depth = 0;
        while (!isAtEnd(c)) {
          if (check(c, TOKEN_COMMA) && depth == 0) break;
          if (check(c, TOKEN_RIGHT_PAREN) && depth == 0) break;
          if (check(c, TOKEN_LEFT_PAREN) || check(c, TOKEN_LEFT_BRACKET) || check(c, TOKEN_LEFT_BRACE)) depth++;
          if (check(c, TOKEN_RIGHT_PAREN) || check(c, TOKEN_RIGHT_BRACKET) || check(c, TOKEN_RIGHT_BRACE)) depth--;
          advance(c);
        }
        defaultEnds[paramIdx] = c->current;
      } else if (sawDefault) {
        errorAt(c, paramName, "Parameters with defaults must be last.");
      }
      paramIdx++;
    } while (match(c, TOKEN_COMMA));
  }
  consume(c, TOKEN_RIGHT_PAREN, "Expect ')' after parameters.");
  consume(c, TOKEN_LEFT_BRACE, "Expect '{' before function body.");

  Chunk* chunk = (Chunk*)malloc(sizeof(Chunk));
  if (!chunk) { fprintf(stderr, "Out of memory.\n"); exit(1); }
  initChunk(chunk);

  ObjString* fnName = stringFromToken(c->vm, name);
  ObjFunction* function = newFunction(c->vm, fnName, arity, minArity, isInitializer, params, chunk, NULL, NULL);

  Compiler fnCompiler;
  fnCompiler.vm = c->vm;
  fnCompiler.tokens = c->tokens;
  fnCompiler.source = c->source;
  fnCompiler.path = c->path;
  fnCompiler.current = bodyStart;
  fnCompiler.panicMode = false;
  fnCompiler.hadError = false;
  fnCompiler.chunk = chunk;
  fnCompiler.scopeDepth = 0;
  fnCompiler.tempIndex = 0;
  fnCompiler.breakContext = NULL;
  fnCompiler.enclosing = c;

  for (int i = 0; i < arity; i++) {
    if (defaultStarts[i] < 0) continue;

    Token ptoken = paramTokens[i];
    emitByte(&fnCompiler, OP_ARG_COUNT, ptoken);
    emitConstant(&fnCompiler, NUMBER_VAL((double)(i + 1)), ptoken);
    emitByte(&fnCompiler, OP_LESS, ptoken);
    int skipJump = emitJump(&fnCompiler, OP_JUMP_IF_FALSE, ptoken);
    emitByte(&fnCompiler, OP_POP, noToken());

    int savedCurrent = fnCompiler.current;
    fnCompiler.current = defaultStarts[i];
    expression(&fnCompiler);
    fnCompiler.current = savedCurrent;

    int nameIndex = emitStringConstant(&fnCompiler, ptoken);
    emitByte(&fnCompiler, OP_SET_VAR, ptoken);
    emitShort(&fnCompiler, (uint16_t)nameIndex, ptoken);
    emitByte(&fnCompiler, OP_POP, noToken());

    int endJump = emitJump(&fnCompiler, OP_JUMP, ptoken);
    patchJump(&fnCompiler, skipJump, ptoken);
    emitByte(&fnCompiler, OP_POP, noToken());
    patchJump(&fnCompiler, endJump, ptoken);
    emitGc(&fnCompiler);
  }

  fnCompiler.current = bodyStart;
  while (!check(&fnCompiler, TOKEN_RIGHT_BRACE) && !isAtEnd(&fnCompiler)) {
    declaration(&fnCompiler);
  }
  consume(&fnCompiler, TOKEN_RIGHT_BRACE, "Expect '}' after function body.");

  emitByte(&fnCompiler, OP_NULL, noToken());
  emitByte(&fnCompiler, OP_RETURN, noToken());

  c->current = fnCompiler.current;

  free(paramTokens);
  free(defaultStarts);
  free(defaultEnds);

  if (fnCompiler.hadError) {
    c->hadError = true;
    return NULL;
  }

  return function;
}

ObjFunction* compile(VM* vm, const TokenArray* tokens, const char* source,
                     const char* path, bool* hadError) {
  initRules();

  Chunk* chunk = (Chunk*)malloc(sizeof(Chunk));
  if (!chunk) { fprintf(stderr, "Out of memory.\n"); exit(1); }
  initChunk(chunk);

  ObjFunction* function = newFunction(vm, NULL, 0, 0, false, NULL, chunk, NULL, NULL);

  Compiler c;
  c.vm = vm;
  c.tokens = tokens;
  c.source = source;
  c.path = path;
  c.current = 0;
  c.panicMode = false;
  c.hadError = false;
  c.chunk = chunk;
  c.scopeDepth = 0;
  c.tempIndex = 0;
  c.breakContext = NULL;
  c.enclosing = NULL;

  while (!isAtEnd(&c)) {
    declaration(&c);
  }

  emitByte(&c, OP_NULL, noToken());
  emitByte(&c, OP_RETURN, noToken());

  *hadError = c.hadError;
  if (c.hadError) {
    return NULL;
  }
  return function;
}

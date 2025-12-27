#include "singlepass.h"
#include "chunk.h"
#include "common.h"
#include "interpreter.h"
#include "diagnostics.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>

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

static bool constValueFromValue(Value value, ConstValue* out) {
  out->ownsString = false;
  if (IS_NULL(value)) {
    out->type = CONST_NULL;
    return true;
  }
  if (IS_BOOL(value)) {
    out->type = CONST_BOOL;
    out->as.boolean = AS_BOOL(value);
    return true;
  }
  if (IS_NUMBER(value)) {
    out->type = CONST_NUMBER;
    out->as.number = AS_NUMBER(value);
    return true;
  }
  if (isObjType(value, OBJ_STRING)) {
    ObjString* str = (ObjString*)AS_OBJ(value);
    out->type = CONST_STRING;
    out->as.string.chars = str->chars;
    out->as.string.length = str->length;
    return true;
  }
  return false;
}

static bool constValueEquals(const ConstValue* a, const ConstValue* b) {
  if (a->type != b->type) return false;
  switch (a->type) {
    case CONST_NULL:
      return true;
    case CONST_BOOL:
      return a->as.boolean == b->as.boolean;
    case CONST_NUMBER:
      return a->as.number == b->as.number;
    case CONST_STRING:
      if (a->as.string.length != b->as.string.length) return false;
      return memcmp(a->as.string.chars, b->as.string.chars,
                    (size_t)a->as.string.length) == 0;
  }
  return false;
}

static bool constValueConcat(const ConstValue* a, const ConstValue* b, ConstValue* out) {
  if (a->type != CONST_STRING || b->type != CONST_STRING) return false;
  int length = a->as.string.length + b->as.string.length;
  char* buffer = (char*)malloc((size_t)length + 1);
  if (!buffer) {
    fprintf(stderr, "Out of memory.\n");
    exit(1);
  }
  memcpy(buffer, a->as.string.chars, (size_t)a->as.string.length);
  memcpy(buffer + a->as.string.length, b->as.string.chars, (size_t)b->as.string.length);
  buffer[length] = '\0';
  out->type = CONST_STRING;
  out->ownsString = true;
  out->as.string.chars = buffer;
  out->as.string.length = length;
  return true;
}

static bool constValueStringify(const ConstValue* input, ConstValue* out) {
  if (input->type == CONST_STRING) {
    *out = *input;
    out->ownsString = false;
    return true;
  }

  char buffer[128];
  const char* text = NULL;
  int length = 0;
  switch (input->type) {
    case CONST_NULL:
      text = "null";
      length = 4;
      break;
    case CONST_BOOL:
      if (input->as.boolean) {
        text = "true";
        length = 4;
      } else {
        text = "false";
        length = 5;
      }
      break;
    case CONST_NUMBER:
      length = snprintf(buffer, sizeof(buffer), "%g", input->as.number);
      if (length < 0) length = 0;
      if (length >= (int)sizeof(buffer)) {
        length = (int)sizeof(buffer) - 1;
      }
      text = buffer;
      break;
    case CONST_STRING:
      break;
  }

  if (!text) return false;
  char* copy = (char*)malloc((size_t)length + 1);
  if (!copy) {
    fprintf(stderr, "Out of memory.\n");
    exit(1);
  }
  memcpy(copy, text, (size_t)length);
  copy[length] = '\0';
  out->type = CONST_STRING;
  out->ownsString = true;
  out->as.string.chars = copy;
  out->as.string.length = length;
  return true;
}

typedef struct {
  uint8_t op;
  int offset;
  int length;
  Token token;
} InstrInfo;

typedef struct {
  uint8_t* code;
  Token* tokens;
  InlineCache* caches;
  int count;
  int capacity;
} CodeBuilder;

static void codeBuilderInit(CodeBuilder* out) {
  out->code = NULL;
  out->tokens = NULL;
  out->caches = NULL;
  out->count = 0;
  out->capacity = 0;
}

static void codeBuilderFree(CodeBuilder* out) {
  FREE_ARRAY(uint8_t, out->code, out->capacity);
  FREE_ARRAY(Token, out->tokens, out->capacity);
  FREE_ARRAY(InlineCache, out->caches, out->capacity);
  codeBuilderInit(out);
}

static void codeBuilderEnsure(CodeBuilder* out, int needed) {
  if (out->capacity >= needed) return;
  int oldCapacity = out->capacity;
  out->capacity = GROW_CAPACITY(oldCapacity);
  while (out->capacity < needed) {
    out->capacity = GROW_CAPACITY(out->capacity);
  }
  out->code = GROW_ARRAY(uint8_t, out->code, oldCapacity, out->capacity);
  out->tokens = GROW_ARRAY(Token, out->tokens, oldCapacity, out->capacity);
  out->caches = GROW_ARRAY(InlineCache, out->caches, oldCapacity, out->capacity);
  if (out->caches) {
    memset(out->caches + oldCapacity, 0,
           sizeof(InlineCache) * (size_t)(out->capacity - oldCapacity));
  }
}

static void codeEmitByte(CodeBuilder* out, uint8_t byte, Token token) {
  codeBuilderEnsure(out, out->count + 1);
  out->code[out->count] = byte;
  out->tokens[out->count] = token;
  if (out->caches) {
    memset(&out->caches[out->count], 0, sizeof(InlineCache));
  }
  out->count++;
}

static void codeEmitShort(CodeBuilder* out, uint16_t value, Token token) {
  codeEmitByte(out, (uint8_t)((value >> 8) & 0xff), token);
  codeEmitByte(out, (uint8_t)(value & 0xff), token);
}

static int instructionLength(const Chunk* chunk, int offset) {
  uint8_t op = chunk->code[offset];
  switch (op) {
    case OP_CONSTANT:
    case OP_GET_VAR:
    case OP_SET_VAR:
    case OP_DEFINE_VAR:
    case OP_DEFINE_CONST:
    case OP_GET_PROPERTY:
    case OP_GET_PROPERTY_OPTIONAL:
    case OP_SET_PROPERTY:
    case OP_GET_THIS:
    case OP_CLOSURE:
    case OP_EXPORT:
    case OP_EXPORT_VALUE:
    case OP_JUMP:
    case OP_JUMP_IF_FALSE:
    case OP_LOOP:
    case OP_ARRAY:
    case OP_MAP:
      return 3;
    case OP_EXPORT_FROM: {
      if (offset + 3 > chunk->count) return 1;
      uint16_t count = (uint16_t)((chunk->code[offset + 1] << 8) | chunk->code[offset + 2]);
      return 3 + (int)count * 4;
    }
    case OP_CALL:
    case OP_CALL_OPTIONAL:
      return 2;
    case OP_INVOKE:
      return 4;
    case OP_CLASS:
      return 5;
    case OP_IMPORT:
      return 4;
    case OP_IMPORT_MODULE:
      return 1;
    default:
      return 1;
  }
}

static bool instrPushesConst(const Chunk* chunk, const InstrInfo* instr, ConstValue* out) {
  switch (instr->op) {
    case OP_TRUE:
      out->type = CONST_BOOL;
      out->ownsString = false;
      out->as.boolean = true;
      return true;
    case OP_FALSE:
      out->type = CONST_BOOL;
      out->ownsString = false;
      out->as.boolean = false;
      return true;
    case OP_NULL:
      out->type = CONST_NULL;
      out->ownsString = false;
      return true;
    case OP_CONSTANT: {
      uint16_t index = (uint16_t)((chunk->code[instr->offset + 1] << 8) |
                                  chunk->code[instr->offset + 2]);
      if (index >= (uint16_t)chunk->constantsCount) return false;
      return constValueFromValue(chunk->constants[index], out);
    }
    default:
      return false;
  }
}

static bool emitConstValue(VM* vm, Chunk* chunk, CodeBuilder* out,
                           const ConstValue* value, Token token) {
  switch (value->type) {
    case CONST_NULL:
      codeEmitByte(out, OP_NULL, token);
      return true;
    case CONST_BOOL:
      codeEmitByte(out, value->as.boolean ? OP_TRUE : OP_FALSE, token);
      return true;
    case CONST_NUMBER: {
      int constant = addConstant(chunk, NUMBER_VAL(value->as.number));
      if (constant > UINT16_MAX) return false;
      codeEmitByte(out, OP_CONSTANT, token);
      codeEmitShort(out, (uint16_t)constant, token);
      return true;
    }
    case CONST_STRING: {
      ObjString* str = copyStringWithLength(vm, value->as.string.chars,
                                            value->as.string.length);
      int constant = addConstant(chunk, OBJ_VAL(str));
      if (constant > UINT16_MAX) return false;
      codeEmitByte(out, OP_CONSTANT, token);
      codeEmitShort(out, (uint16_t)constant, token);
      return true;
    }
  }
  return false;
}

static void emitInstructionRaw(CodeBuilder* out, const Chunk* chunk,
                               const InstrInfo* instr) {
  for (int i = 0; i < instr->length; i++) {
    codeEmitByte(out, chunk->code[instr->offset + i],
                 chunk->tokens[instr->offset + i]);
  }
}

static void optimizeChunk(VM* vm, Chunk* chunk) {
  if (!chunk || chunk->count == 0) return;
  int capacity = 64;
  int instrCount = 0;
  InstrInfo* instrs = (InstrInfo*)malloc(sizeof(InstrInfo) * (size_t)capacity);
  if (!instrs) {
    fprintf(stderr, "Out of memory.\n");
    exit(1);
  }

  int offset = 0;
  while (offset < chunk->count) {
    if (instrCount >= capacity) {
      int oldCap = capacity;
      capacity = GROW_CAPACITY(oldCap);
      instrs = GROW_ARRAY(InstrInfo, instrs, oldCap, capacity);
      if (!instrs) {
        fprintf(stderr, "Out of memory.\n");
        exit(1);
      }
    }
    uint8_t op = chunk->code[offset];
    int length = instructionLength(chunk, offset);
    if (offset + length > chunk->count) {
      length = 1;
    }
    instrs[instrCount].op = op;
    instrs[instrCount].offset = offset;
    instrs[instrCount].length = length;
    instrs[instrCount].token = chunk->tokens[offset];
    instrCount++;
    offset += length;
  }

  CodeBuilder out;
  codeBuilderInit(&out);

  for (int i = 0; i < instrCount; ) {
    ConstValue a;
    ConstValue b;
    ConstValue result;

    if (i + 1 < instrCount &&
        instrPushesConst(chunk, &instrs[i], &a)) {
      uint8_t op = instrs[i + 1].op;
      if (op == OP_NEGATE && a.type == CONST_NUMBER) {
        result.type = CONST_NUMBER;
        result.ownsString = false;
        result.as.number = -a.as.number;
        if (emitConstValue(vm, chunk, &out, &result, instrs[i + 1].token)) {
          i += 2;
          continue;
        }
      }
      if (op == OP_NOT) {
        result.type = CONST_BOOL;
        result.ownsString = false;
        result.as.boolean = !constValueIsTruthy(&a);
        if (emitConstValue(vm, chunk, &out, &result, instrs[i + 1].token)) {
          i += 2;
          continue;
        }
      }
      if (op == OP_STRINGIFY) {
        if (constValueStringify(&a, &result)) {
          bool emitted = emitConstValue(vm, chunk, &out, &result, instrs[i + 1].token);
          constValueFree(&result);
          if (emitted) {
            i += 2;
            continue;
          }
        }
      }
    }

    if (i + 2 < instrCount &&
        instrPushesConst(chunk, &instrs[i], &a) &&
        instrPushesConst(chunk, &instrs[i + 1], &b)) {
      uint8_t op = instrs[i + 2].op;
      bool folded = false;
      switch (op) {
        case OP_ADD:
          if (a.type == CONST_NUMBER && b.type == CONST_NUMBER) {
            result.type = CONST_NUMBER;
            result.ownsString = false;
            result.as.number = a.as.number + b.as.number;
            folded = true;
          } else if (constValueConcat(&a, &b, &result)) {
            folded = true;
          }
          break;
        case OP_SUBTRACT:
          if (a.type == CONST_NUMBER && b.type == CONST_NUMBER) {
            result.type = CONST_NUMBER;
            result.ownsString = false;
            result.as.number = a.as.number - b.as.number;
            folded = true;
          }
          break;
        case OP_MULTIPLY:
          if (a.type == CONST_NUMBER && b.type == CONST_NUMBER) {
            result.type = CONST_NUMBER;
            result.ownsString = false;
            result.as.number = a.as.number * b.as.number;
            folded = true;
          }
          break;
        case OP_DIVIDE:
          if (a.type == CONST_NUMBER && b.type == CONST_NUMBER) {
            result.type = CONST_NUMBER;
            result.ownsString = false;
            result.as.number = a.as.number / b.as.number;
            folded = true;
          }
          break;
        case OP_EQUAL:
          result.type = CONST_BOOL;
          result.ownsString = false;
          result.as.boolean = constValueEquals(&a, &b);
          folded = true;
          break;
        case OP_GREATER:
          if (a.type == CONST_NUMBER && b.type == CONST_NUMBER) {
            result.type = CONST_BOOL;
            result.ownsString = false;
            result.as.boolean = a.as.number > b.as.number;
            folded = true;
          }
          break;
        case OP_GREATER_EQUAL:
          if (a.type == CONST_NUMBER && b.type == CONST_NUMBER) {
            result.type = CONST_BOOL;
            result.ownsString = false;
            result.as.boolean = a.as.number >= b.as.number;
            folded = true;
          }
          break;
        case OP_LESS:
          if (a.type == CONST_NUMBER && b.type == CONST_NUMBER) {
            result.type = CONST_BOOL;
            result.ownsString = false;
            result.as.boolean = a.as.number < b.as.number;
            folded = true;
          }
          break;
        case OP_LESS_EQUAL:
          if (a.type == CONST_NUMBER && b.type == CONST_NUMBER) {
            result.type = CONST_BOOL;
            result.ownsString = false;
            result.as.boolean = a.as.number <= b.as.number;
            folded = true;
          }
          break;
        default:
          break;
      }

      if (folded) {
        if (emitConstValue(vm, chunk, &out, &result, instrs[i + 2].token)) {
          constValueFree(&result);
          i += 3;
          continue;
        }
        constValueFree(&result);
      }
    }

    emitInstructionRaw(&out, chunk, &instrs[i]);
    i++;
  }

  free(instrs);

  FREE_ARRAY(uint8_t, chunk->code, chunk->capacity);
  FREE_ARRAY(Token, chunk->tokens, chunk->capacity);
  FREE_ARRAY(InlineCache, chunk->caches, chunk->capacity);

  chunk->code = out.code;
  chunk->tokens = out.tokens;
  chunk->caches = out.caches;
  chunk->count = out.count;
  chunk->capacity = out.capacity;
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

static bool checkNext(Compiler* c, ErkaoTokenType type) {
  if (c->current + 1 >= c->tokens->count) return false;
  return c->tokens->tokens[c->current + 1].type == type;
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

typedef struct {
  ErkaoTokenType type;
  const char* lexeme;
} KeywordEntry;

static const KeywordEntry keywordEntries[] = {
  {TOKEN_AND, "and"},
  {TOKEN_AS, "as"},
  {TOKEN_BREAK, "break"},
  {TOKEN_CASE, "case"},
  {TOKEN_CLASS, "class"},
  {TOKEN_CONST, "const"},
  {TOKEN_CONTINUE, "continue"},
  {TOKEN_DEFAULT, "default"},
  {TOKEN_ELSE, "else"},
  {TOKEN_ENUM, "enum"},
  {TOKEN_EXPORT, "export"},
  {TOKEN_FALSE, "false"},
  {TOKEN_FOR, "for"},
  {TOKEN_FOREACH, "foreach"},
  {TOKEN_FROM, "from"},
  {TOKEN_FUN, "fun"},
  {TOKEN_IF, "if"},
  {TOKEN_IMPORT, "import"},
  {TOKEN_IN, "in"},
  {TOKEN_LET, "let"},
  {TOKEN_MATCH, "match"},
  {TOKEN_NULL, "null"},
  {TOKEN_OR, "or"},
  {TOKEN_RETURN, "return"},
  {TOKEN_SWITCH, "switch"},
  {TOKEN_THIS, "this"},
  {TOKEN_TRUE, "true"},
  {TOKEN_WHILE, "while"},
  {TOKEN_ERROR, NULL}
};

static const char* keywordLexeme(ErkaoTokenType type) {
  for (int i = 0; keywordEntries[i].lexeme != NULL; i++) {
    if (keywordEntries[i].type == type) return keywordEntries[i].lexeme;
  }
  return NULL;
}

static const char* tokenDescription(ErkaoTokenType type) {
  switch (type) {
    case TOKEN_LEFT_PAREN: return "'('";
    case TOKEN_RIGHT_PAREN: return "')'";
    case TOKEN_LEFT_BRACE: return "'{'";
    case TOKEN_RIGHT_BRACE: return "'}'";
    case TOKEN_LEFT_BRACKET: return "'['";
    case TOKEN_RIGHT_BRACKET: return "']'";
    case TOKEN_COMMA: return "','";
    case TOKEN_DOT: return "'.'";
    case TOKEN_QUESTION_DOT: return "'?.'";
    case TOKEN_MINUS: return "'-'";
    case TOKEN_PLUS: return "'+'";
    case TOKEN_SEMICOLON: return "';'";
    case TOKEN_SLASH: return "'/'";
    case TOKEN_STAR: return "'*'";
    case TOKEN_COLON: return "':'";
    case TOKEN_BANG: return "'!'";
    case TOKEN_BANG_EQUAL: return "'!='";
    case TOKEN_EQUAL: return "'='";
    case TOKEN_EQUAL_EQUAL: return "'=='";
    case TOKEN_GREATER: return "'>'";
    case TOKEN_GREATER_EQUAL: return "'>='";
    case TOKEN_LESS: return "'<'";
    case TOKEN_LESS_EQUAL: return "'<='";
    case TOKEN_IDENTIFIER: return "identifier";
    case TOKEN_STRING: return "string";
    case TOKEN_STRING_SEGMENT: return "string segment";
    case TOKEN_NUMBER: return "number";
    case TOKEN_INTERP_START: return "'${'";
    case TOKEN_INTERP_END: return "interpolation end";
    case TOKEN_AND: return "'and'";
    case TOKEN_AS: return "'as'";
    case TOKEN_BREAK: return "'break'";
    case TOKEN_CASE: return "'case'";
    case TOKEN_CLASS: return "'class'";
    case TOKEN_CONST: return "'const'";
    case TOKEN_CONTINUE: return "'continue'";
    case TOKEN_DEFAULT: return "'default'";
    case TOKEN_ELSE: return "'else'";
    case TOKEN_ENUM: return "'enum'";
    case TOKEN_EXPORT: return "'export'";
    case TOKEN_FALSE: return "'false'";
    case TOKEN_FOR: return "'for'";
    case TOKEN_FOREACH: return "'foreach'";
    case TOKEN_FROM: return "'from'";
    case TOKEN_FUN: return "'fun'";
    case TOKEN_IF: return "'if'";
    case TOKEN_IMPORT: return "'import'";
    case TOKEN_IN: return "'in'";
    case TOKEN_LET: return "'let'";
    case TOKEN_MATCH: return "'match'";
    case TOKEN_NULL: return "'null'";
    case TOKEN_OR: return "'or'";
    case TOKEN_RETURN: return "'return'";
    case TOKEN_SWITCH: return "'switch'";
    case TOKEN_THIS: return "'this'";
    case TOKEN_TRUE: return "'true'";
    case TOKEN_WHILE: return "'while'";
    case TOKEN_EOF: return "end of file";
    case TOKEN_ERROR: return "invalid token";
    default:
      return "token";
  }
}

static bool keywordSuggestion(Token found, ErkaoTokenType expected,
                              char* out, size_t outSize) {
  if (found.type != TOKEN_IDENTIFIER || !out || outSize == 0) return false;
  const char* expectedLexeme = keywordLexeme(expected);
  if (!expectedLexeme) return false;
  int expectedLen = (int)strlen(expectedLexeme);
  int dist = diag_edit_distance_limited(found.start, found.length,
                                        expectedLexeme, expectedLen,
                                        ERKAO_DIAG_MAX_DISTANCE);
  if (dist > ERKAO_DIAG_MAX_DISTANCE) return false;
  snprintf(out, outSize, "%s", expectedLexeme);
  return true;
}

static void synchronize(Compiler* c);

static void appendMessage(char* buffer, size_t size, const char* format, ...) {
  size_t length = strlen(buffer);
  if (length >= size) return;
  va_list args;
  va_start(args, format);
  vsnprintf(buffer + length, size - length, format, args);
  va_end(args);
}

static void noteAt(Compiler* c, Token token, const char* message) {
  if (!message || message[0] == '\0') return;
  if (token.line <= 0 || token.column <= 0) return;
  const char* path = c->path ? c->path : "<repl>";
  fprintf(stderr, "%s:%d:%d: Note: %s\n", path, token.line, token.column, message);
  printErrorContext(c->source, token.line, token.column,
                    token.length > 0 ? token.length : 1);
}

static void synchronizeExpression(Compiler* c) {
  while (!isAtEnd(c)) {
    switch (peek(c).type) {
      case TOKEN_COMMA:
      case TOKEN_SEMICOLON:
      case TOKEN_RIGHT_PAREN:
      case TOKEN_RIGHT_BRACKET:
      case TOKEN_RIGHT_BRACE:
      case TOKEN_INTERP_END:
        return;
      default:
        break;
    }
    advance(c);
  }
}

static void buildConsumeErrorMessage(ErkaoTokenType expected, Token found,
                                     const char* message,
                                     char* out, size_t outSize) {
  const char* foundDesc = tokenDescription(found.type);
  if (message && message[0] != '\0') {
    snprintf(out, outSize, "%s Found %s.", message, foundDesc);
  } else {
    const char* expectedDesc = tokenDescription(expected);
    snprintf(out, outSize, "Expected %s. Found %s.", expectedDesc, foundDesc);
  }

  if ((expected == TOKEN_RIGHT_PAREN || expected == TOKEN_RIGHT_BRACKET ||
       expected == TOKEN_RIGHT_BRACE) &&
      (found.type == TOKEN_RIGHT_PAREN || found.type == TOKEN_RIGHT_BRACKET ||
       found.type == TOKEN_RIGHT_BRACE) &&
      expected != found.type) {
    appendMessage(out, outSize, " Mismatched closing %s.", foundDesc);
  }

  if (expected == TOKEN_IDENTIFIER) {
    const char* lexeme = keywordLexeme(found.type);
    if (lexeme) {
      appendMessage(out, outSize, " '%s' is a keyword.", lexeme);
    }
  }

  char suggestion[32];
  if (keywordSuggestion(found, expected, suggestion, sizeof(suggestion))) {
    appendMessage(out, outSize, " Did you mean '%s'?", suggestion);
  }
}

static void emitConsumeError(Compiler* c, ErkaoTokenType expected, Token found,
                             const char* message) {
  char full[256];
  buildConsumeErrorMessage(expected, found, message, full, sizeof(full));
  if (expected == TOKEN_SEMICOLON && c->current > 0) {
    Token token = previous(c);
    if (token.length > 0) token.column += token.length;
    errorAt(c, token, full);
  } else {
    errorAtCurrent(c, full);
  }
}

static void recoverAfterConsumeError(Compiler* c, ErkaoTokenType expected) {
  if (expected == TOKEN_SEMICOLON) {
    synchronize(c);
    return;
  }
  if (expected == TOKEN_RIGHT_PAREN || expected == TOKEN_RIGHT_BRACKET ||
      expected == TOKEN_RIGHT_BRACE || expected == TOKEN_INTERP_END) {
    synchronizeExpression(c);
    c->panicMode = false;
  }
}

static bool openMatchesClose(ErkaoTokenType open, ErkaoTokenType close) {
  if (open == TOKEN_LEFT_PAREN && close == TOKEN_RIGHT_PAREN) return true;
  if (open == TOKEN_LEFT_BRACKET && close == TOKEN_RIGHT_BRACKET) return true;
  if (open == TOKEN_LEFT_BRACE && close == TOKEN_RIGHT_BRACE) return true;
  if (open == TOKEN_INTERP_START && close == TOKEN_INTERP_END) return true;
  return false;
}

static void noteUnclosedDelimiter(Compiler* c, Token open, ErkaoTokenType close) {
  if (!openMatchesClose(open.type, close)) return;
  const char* desc = tokenDescription(open.type);
  char message[96];
  snprintf(message, sizeof(message), "Opening %s is here.", desc);
  noteAt(c, open, message);
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
  Token found = peek(c);
  if (c->panicMode) return found;
  emitConsumeError(c, type, found, message);
  recoverAfterConsumeError(c, type);
  return found;
}

static Token consumeClosing(Compiler* c, ErkaoTokenType type, const char* message,
                            Token open) {
  if (check(c, type)) return advance(c);
  Token found = peek(c);
  if (c->panicMode) return found;
  emitConsumeError(c, type, found, message);
  noteUnclosedDelimiter(c, open, type);
  recoverAfterConsumeError(c, type);
  return found;
}

static void synchronize(Compiler* c) {
  c->panicMode = false;
  while (!isAtEnd(c)) {
    if (previous(c).type == TOKEN_SEMICOLON) return;
    switch (peek(c).type) {
      case TOKEN_CLASS: case TOKEN_FUN: case TOKEN_LET: case TOKEN_CONST:
      case TOKEN_ENUM: case TOKEN_EXPORT: case TOKEN_IMPORT: case TOKEN_FROM:
      case TOKEN_IF: case TOKEN_WHILE: case TOKEN_FOR: case TOKEN_FOREACH:
      case TOKEN_SWITCH: case TOKEN_MATCH: case TOKEN_RETURN:
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

static void emitExportName(Compiler* c, Token name) {
  int nameIdx = emitStringConstant(c, name);
  emitByte(c, OP_EXPORT, name);
  emitShort(c, (uint16_t)nameIdx, name);
}

static void emitExportValue(Compiler* c, uint16_t nameIdx, Token token) {
  emitByte(c, OP_EXPORT_VALUE, token);
  emitShort(c, nameIdx, token);
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

static char* parseStringChars(const char* src, int length) {
  if (length < 0) length = 0;
  char* buffer = (char*)malloc((size_t)length + 1);
  if (!buffer) { fprintf(stderr, "Out of memory.\n"); exit(1); }
  int out = 0;
  for (int i = 0; i < length; i++) {
    char ch = src[i];
    if (ch == '\r' && i + 1 < length && src[i + 1] == '\n') {
      continue;
    }
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

static bool isTripleQuoted(Token token) {
  if (token.length < 6) return false;
  return token.start[0] == '"' && token.start[1] == '"' && token.start[2] == '"' &&
         token.start[token.length - 1] == '"' &&
         token.start[token.length - 2] == '"' &&
         token.start[token.length - 3] == '"';
}

static char* parseStringLiteral(Token token) {
  int startOffset = 1;
  int endOffset = 1;
  if (isTripleQuoted(token)) {
    startOffset = 3;
    endOffset = 3;
  }
  int length = token.length - startOffset - endOffset;
  if (length < 0) length = 0;
  const char* src = token.start + startOffset;
  return parseStringChars(src, length);
}

static char* parseStringSegment(Token token) {
  return parseStringChars(token.start, token.length);
}

static void expression(Compiler* c);
static void declaration(Compiler* c);
static void statement(Compiler* c);
static void block(Compiler* c, Token open);

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

static double parseNumberToken(Token token) {
  char* temp = copyTokenLexeme(token);
  double value = strtod(temp, NULL);
  free(temp);
  return value;
}

static void string(Compiler* c, bool canAssign) {
  (void)canAssign;
  Token token = previous(c);
  char* value = parseStringLiteral(token);
  ObjString* str = takeStringWithLength(c->vm, value, (int)strlen(value));
  emitConstant(c, OBJ_VAL(str), token);
}

static void stringSegment(Compiler* c, bool canAssign) {
  (void)canAssign;
  Token segment = previous(c);
  char* value = parseStringSegment(segment);
  ObjString* str = takeStringWithLength(c->vm, value, (int)strlen(value));
  emitConstant(c, OBJ_VAL(str), segment);

  while (match(c, TOKEN_INTERP_START)) {
    Token interpStart = previous(c);
    expression(c);
    consumeClosing(c, TOKEN_INTERP_END, "Expect '}' after interpolation.", interpStart);
    emitByte(c, OP_STRINGIFY, segment);
    emitByte(c, OP_ADD, segment);

    Token tail = consume(c, TOKEN_STRING_SEGMENT, "Expect string segment after interpolation.");
    char* tailValue = parseStringSegment(tail);
    ObjString* tailStr = takeStringWithLength(c->vm, tailValue, (int)strlen(tailValue));
    emitConstant(c, OBJ_VAL(tailStr), tail);
    emitByte(c, OP_ADD, tail);
  }
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
  Token open = previous(c);
  expression(c);
  consumeClosing(c, TOKEN_RIGHT_PAREN, "Expect ')' after expression.", open);
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
  c->pendingOptionalCall = false;
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
  c->pendingOptionalCall = false;
  Token op = previous(c);
  int jumpIfFalse = emitJump(c, OP_JUMP_IF_FALSE, op);
  emitByte(c, OP_POP, noToken());
  parsePrecedence(c, PREC_AND);
  patchJump(c, jumpIfFalse, op);
}

static void orExpr(Compiler* c, bool canAssign) {
  (void)canAssign;
  c->pendingOptionalCall = false;
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
  bool optionalCall = c->pendingOptionalCall;
  c->pendingOptionalCall = false;
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
  consumeClosing(c, TOKEN_RIGHT_PAREN, "Expect ')' after arguments.", paren);
  emitByte(c, optionalCall ? OP_CALL_OPTIONAL : OP_CALL, paren);
  emitByte(c, (uint8_t)argc, paren);
}

static void dot(Compiler* c, bool canAssign) {
  c->pendingOptionalCall = false;
  Token name = consume(c, TOKEN_IDENTIFIER, "Expect property name after '.'.");
  int nameIdx = emitStringConstant(c, name);
  if (canAssign && match(c, TOKEN_EQUAL)) {
    expression(c);
    emitByte(c, OP_SET_PROPERTY, name);
    emitShort(c, (uint16_t)nameIdx, name);
  } else if (check(c, TOKEN_LEFT_PAREN)) {
    Token paren = advance(c);
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
    consumeClosing(c, TOKEN_RIGHT_PAREN, "Expect ')' after arguments.", paren);
    emitByte(c, OP_INVOKE, paren);
    emitShort(c, (uint16_t)nameIdx, name);
    emitByte(c, (uint8_t)argc, paren);
  } else {
    emitByte(c, OP_GET_PROPERTY, name);
    emitShort(c, (uint16_t)nameIdx, name);
  }
}

static void optionalDot(Compiler* c, bool canAssign) {
  (void)canAssign;
  Token name = consume(c, TOKEN_IDENTIFIER, "Expect property name after '?.'.");
  int nameIdx = emitStringConstant(c, name);
  emitByte(c, OP_GET_PROPERTY_OPTIONAL, name);
  emitShort(c, (uint16_t)nameIdx, name);
  c->pendingOptionalCall = true;
}

static void index_(Compiler* c, bool canAssign) {
  c->pendingOptionalCall = false;
  Token bracket = previous(c);
  expression(c);
  consumeClosing(c, TOKEN_RIGHT_BRACKET, "Expect ']' after index.", bracket);
  if (canAssign && match(c, TOKEN_EQUAL)) {
    expression(c);
    emitByte(c, OP_SET_INDEX, bracket);
  } else {
    emitByte(c, OP_GET_INDEX, bracket);
  }
}

static void array(Compiler* c, bool canAssign) {
  (void)canAssign;
  Token open = previous(c);
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
  consumeClosing(c, TOKEN_RIGHT_BRACKET, "Expect ']' after array literal.", open);
  c->chunk->code[sizeOffset] = (uint8_t)((count >> 8) & 0xff);
  c->chunk->code[sizeOffset + 1] = (uint8_t)(count & 0xff);
}

static void map(Compiler* c, bool canAssign) {
  (void)canAssign;
  Token open = previous(c);
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
  consumeClosing(c, TOKEN_RIGHT_BRACE, "Expect '}' after map literal.", open);
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
  rules[TOKEN_QUESTION_DOT] = (ParseRule){NULL, optionalDot, PREC_CALL};
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
  rules[TOKEN_STRING_SEGMENT] = (ParseRule){stringSegment, NULL, PREC_NONE};
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
    char message[128];
    snprintf(message, sizeof(message), "Expect expression. Found %s.",
             tokenDescription(previous(c).type));
    errorAt(c, previous(c), message);
    synchronizeExpression(c);
    c->panicMode = false;
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
  c->pendingOptionalCall = false;
  parsePrecedence(c, PREC_ASSIGNMENT);
}

static void expressionStatement(Compiler* c) {
  expression(c);
  consume(c, TOKEN_SEMICOLON, "Expect ';' after expression.");
  emitByte(c, OP_POP, noToken());
  emitGc(c);
}

static void varDeclaration(Compiler* c, bool isConst, bool isExport) {
  Token name = consume(c, TOKEN_IDENTIFIER, "Expect variable name.");
  bool hasInitializer = match(c, TOKEN_EQUAL);
  if (hasInitializer) {
    expression(c);
  } else {
    if (isConst) {
      errorAt(c, name, "Const declarations require an initializer.");
    }
    emitByte(c, OP_NULL, noToken());
  }
  consume(c, TOKEN_SEMICOLON, "Expect ';' after variable declaration.");
  int nameIdx = emitStringConstant(c, name);
  emitByte(c, isConst ? OP_DEFINE_CONST : OP_DEFINE_VAR, name);
  emitShort(c, (uint16_t)nameIdx, name);
  if (isExport) {
    emitByte(c, OP_EXPORT, name);
    emitShort(c, (uint16_t)nameIdx, name);
  }
  emitGc(c);
}

static void block(Compiler* c, Token open) {
  while (!check(c, TOKEN_RIGHT_BRACE) && !isAtEnd(c)) {
    declaration(c);
  }
  consumeClosing(c, TOKEN_RIGHT_BRACE, "Expect '}' after block.", open);
}

static void blockStatement(Compiler* c) {
  Token open = previous(c);
  emitByte(c, OP_BEGIN_SCOPE, noToken());
  c->scopeDepth++;
  block(c, open);
  emitByte(c, OP_END_SCOPE, noToken());
  c->scopeDepth--;
  emitGc(c);
}

static void ifStatement(Compiler* c) {
  Token keyword = previous(c);
  Token openParen = consume(c, TOKEN_LEFT_PAREN, "Expect '(' after 'if'.");
  expression(c);
  consumeClosing(c, TOKEN_RIGHT_PAREN, "Expect ')' after if condition.", openParen);
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
    int endJump = emitJump(c, OP_JUMP, keyword);
    patchJump(c, thenJump, keyword);
    emitByte(c, OP_POP, noToken());
    patchJump(c, endJump, keyword);
  }
  emitGc(c);
}

static void whileStatement(Compiler* c) {
  Token keyword = previous(c);
  int loopStart = c->chunk->count;
  Token openParen = consume(c, TOKEN_LEFT_PAREN, "Expect '(' after 'while'.");
  expression(c);
  consumeClosing(c, TOKEN_RIGHT_PAREN, "Expect ')' after condition.", openParen);
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
  Token openParen = consume(c, TOKEN_LEFT_PAREN, "Expect '(' after 'for'.");

  if (match(c, TOKEN_SEMICOLON)) {
  } else if (match(c, TOKEN_LET)) {
    varDeclaration(c, false, false);
  } else if (match(c, TOKEN_CONST)) {
    varDeclaration(c, true, false);
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
  consumeClosing(c, TOKEN_RIGHT_PAREN, "Expect ')' after for clauses.", openParen);

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
  Token openParen = consume(c, TOKEN_LEFT_PAREN, "Expect '(' after 'foreach'.");

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
  consumeClosing(c, TOKEN_RIGHT_PAREN, "Expect ')' after foreach iterable.", openParen);

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
  const char* keywordName = keyword.type == TOKEN_MATCH ? "match" : "switch";
  char message[64];
  emitByte(c, OP_BEGIN_SCOPE, noToken());
  c->scopeDepth++;
  snprintf(message, sizeof(message), "Expect '(' after '%s'.", keywordName);
  Token openParen = consume(c, TOKEN_LEFT_PAREN, message);
  expression(c);
  snprintf(message, sizeof(message), "Expect ')' after %s value.", keywordName);
  consumeClosing(c, TOKEN_RIGHT_PAREN, message, openParen);
  snprintf(message, sizeof(message), "Expect '{' after %s value.", keywordName);
  Token openBrace = consume(c, TOKEN_LEFT_BRACE, message);

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

  consumeClosing(c, TOKEN_RIGHT_BRACE, "Expect '}' after switch cases.", openBrace);
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
  if (match(c, TOKEN_STAR)) {
    consume(c, TOKEN_AS, "Expect 'as' after '*'.");
    Token alias = consume(c, TOKEN_IDENTIFIER, "Expect name after 'as'.");
    consume(c, TOKEN_FROM, "Expect 'from' after import alias.");
    expression(c);
    consume(c, TOKEN_SEMICOLON, "Expect ';' after import.");
    emitByte(c, OP_IMPORT, keyword);
    emitByte(c, 1, keyword);
    uint16_t aliasIdx = (uint16_t)emitStringConstant(c, alias);
    emitShort(c, aliasIdx, keyword);
    emitGc(c);
    return;
  }

  if (check(c, TOKEN_IDENTIFIER) && checkNext(c, TOKEN_FROM)) {
    Token alias = consume(c, TOKEN_IDENTIFIER, "Expect name after 'import'.");
    consume(c, TOKEN_FROM, "Expect 'from' after import name.");
    expression(c);
    consume(c, TOKEN_SEMICOLON, "Expect ';' after import.");
    emitByte(c, OP_IMPORT_MODULE, keyword);
    int defaultIdx = emitStringConstantFromChars(c, "default", 7);
    emitByte(c, OP_GET_PROPERTY, keyword);
    emitShort(c, (uint16_t)defaultIdx, keyword);
    int nameIdx = emitStringConstant(c, alias);
    emitByte(c, OP_DEFINE_VAR, alias);
    emitShort(c, (uint16_t)nameIdx, alias);
    emitGc(c);
    return;
  }

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

static void functionDeclaration(Compiler* c, bool isExport) {
  Token name = consume(c, TOKEN_IDENTIFIER, "Expect function name.");
  ObjFunction* function = compileFunction(c, name, false);
  if (!function) return;
  int constant = makeConstant(c, OBJ_VAL(function), name);
  emitByte(c, OP_CLOSURE, name);
  emitShort(c, (uint16_t)constant, name);
  int nameIdx = emitStringConstant(c, name);
  emitByte(c, OP_DEFINE_VAR, name);
  emitShort(c, (uint16_t)nameIdx, name);
  if (isExport) {
    emitByte(c, OP_EXPORT, name);
    emitShort(c, (uint16_t)nameIdx, name);
  }
  emitGc(c);
}

static void classDeclaration(Compiler* c, bool isExport) {
  Token name = consume(c, TOKEN_IDENTIFIER, "Expect class name.");
  Token openBrace = consume(c, TOKEN_LEFT_BRACE, "Expect '{' before class body.");

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
  consumeClosing(c, TOKEN_RIGHT_BRACE, "Expect '}' after class body.", openBrace);

  emitByte(c, OP_CLASS, name);
  emitShort(c, (uint16_t)nameConst, name);
  emitShort(c, (uint16_t)methodCount, name);
  if (isExport) {
    emitByte(c, OP_EXPORT, name);
    emitShort(c, (uint16_t)nameConst, name);
  }
  emitGc(c);
}

static void enumDeclaration(Compiler* c, bool isExport) {
  Token name = consume(c, TOKEN_IDENTIFIER, "Expect enum name.");
  Token openBrace = consume(c, TOKEN_LEFT_BRACE, "Expect '{' before enum body.");

  emitByte(c, OP_MAP, noToken());
  emitShort(c, 0, noToken());
  int sizeOffset = c->chunk->count - 2;
  int count = 0;
  double nextValue = 0.0;

  if (!check(c, TOKEN_RIGHT_BRACE)) {
    do {
      Token member = consume(c, TOKEN_IDENTIFIER, "Expect enum member name.");
      char* memberName = copyTokenLexeme(member);
      ObjString* keyStr = takeStringWithLength(c->vm, memberName, member.length);
      emitConstant(c, OBJ_VAL(keyStr), member);

      double value = nextValue;
      if (match(c, TOKEN_EQUAL)) {
        bool negative = false;
        if (match(c, TOKEN_MINUS)) {
          negative = true;
        }
        Token numToken = consume(c, TOKEN_NUMBER, "Expect number after '='.");
        value = parseNumberToken(numToken);
        if (negative) value = -value;
        nextValue = value + 1.0;
      } else {
        nextValue = value + 1.0;
      }

      emitConstant(c, NUMBER_VAL(value), member);
      emitByte(c, OP_MAP_SET, member);
      count++;
    } while (match(c, TOKEN_COMMA));
  }

  consumeClosing(c, TOKEN_RIGHT_BRACE, "Expect '}' after enum body.", openBrace);

  c->chunk->code[sizeOffset] = (uint8_t)((count >> 8) & 0xff);
  c->chunk->code[sizeOffset + 1] = (uint8_t)(count & 0xff);

  int nameIdx = emitStringConstant(c, name);
  emitByte(c, OP_DEFINE_VAR, name);
  emitShort(c, (uint16_t)nameIdx, name);
  if (isExport) {
    emitByte(c, OP_EXPORT, name);
    emitShort(c, (uint16_t)nameIdx, name);
  }
  emitGc(c);
}

typedef struct {
  uint16_t from;
  uint16_t to;
} ExportName;

static ExportName* parseExportList(Compiler* c, int* outCount, Token open) {
  int count = 0;
  int capacity = 4;
  ExportName* names = (ExportName*)malloc(sizeof(ExportName) * (size_t)capacity);
  if (!names) {
    fprintf(stderr, "Out of memory.\n");
    exit(1);
  }

  if (!check(c, TOKEN_RIGHT_BRACE)) {
    do {
      Token from;
      if (match(c, TOKEN_IDENTIFIER) || match(c, TOKEN_DEFAULT)) {
        from = previous(c);
      } else {
        errorAtCurrent(c, "Expect export name.");
        break;
      }
      Token to = from;
      if (match(c, TOKEN_AS)) {
        to = consume(c, TOKEN_IDENTIFIER, "Expect export name after 'as'.");
      }
      if (count >= capacity) {
        int oldCap = capacity;
        capacity = GROW_CAPACITY(oldCap);
        names = GROW_ARRAY(ExportName, names, oldCap, capacity);
        if (!names) {
          fprintf(stderr, "Out of memory.\n");
          exit(1);
        }
      }
      names[count].from = (uint16_t)emitStringConstant(c, from);
      names[count].to = (uint16_t)emitStringConstant(c, to);
      count++;
    } while (match(c, TOKEN_COMMA));
  }
  consumeClosing(c, TOKEN_RIGHT_BRACE, "Expect '}' after export list.", open);

  *outCount = count;
  return names;
}

static void exportDeclaration(Compiler* c) {
  Token keyword = previous(c);
  bool allowExport = c->enclosing == NULL && c->scopeDepth == 0;
  if (!allowExport) {
    errorAt(c, keyword, "Export declarations must be at top level.");
  }

  if (match(c, TOKEN_DEFAULT)) {
    if (match(c, TOKEN_FUN)) {
      Token name = consume(c, TOKEN_IDENTIFIER, "Expect function name.");
      ObjFunction* function = compileFunction(c, name, false);
      if (!function) return;
      int constant = makeConstant(c, OBJ_VAL(function), name);
      emitByte(c, OP_CLOSURE, name);
      emitShort(c, (uint16_t)constant, name);
      int nameIdx = emitStringConstant(c, name);
      emitByte(c, OP_DEFINE_VAR, name);
      emitShort(c, (uint16_t)nameIdx, name);
      if (allowExport) {
        emitGetVarConstant(c, nameIdx);
        int defaultIdx = emitStringConstantFromChars(c, "default", 7);
        emitExportValue(c, (uint16_t)defaultIdx, name);
      }
      emitGc(c);
      return;
    }
    if (match(c, TOKEN_CLASS)) {
      Token name = consume(c, TOKEN_IDENTIFIER, "Expect class name.");
      Token openBrace = consume(c, TOKEN_LEFT_BRACE, "Expect '{' before class body.");

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
      consumeClosing(c, TOKEN_RIGHT_BRACE, "Expect '}' after class body.", openBrace);

      emitByte(c, OP_CLASS, name);
      emitShort(c, (uint16_t)nameConst, name);
      emitShort(c, (uint16_t)methodCount, name);
      if (allowExport) {
        emitGetVarConstant(c, nameConst);
        int defaultIdx = emitStringConstantFromChars(c, "default", 7);
        emitExportValue(c, (uint16_t)defaultIdx, name);
      }
      emitGc(c);
      return;
    }
    expression(c);
    consume(c, TOKEN_SEMICOLON, "Expect ';' after export.");
    if (allowExport) {
      int defaultIdx = emitStringConstantFromChars(c, "default", 7);
      emitExportValue(c, (uint16_t)defaultIdx, keyword);
    } else {
      emitByte(c, OP_POP, keyword);
    }
    return;
  }

  if (match(c, TOKEN_STAR)) {
    consume(c, TOKEN_FROM, "Expect 'from' after '*'.");
    expression(c);
    consume(c, TOKEN_SEMICOLON, "Expect ';' after export.");
    if (allowExport) {
      emitByte(c, OP_IMPORT_MODULE, keyword);
      emitByte(c, OP_EXPORT_FROM, keyword);
      emitShort(c, 0, keyword);
    } else {
      emitByte(c, OP_POP, keyword);
    }
    emitGc(c);
    return;
  }

  if (match(c, TOKEN_LEFT_BRACE)) {
    Token openBrace = previous(c);
    int nameCount = 0;
    ExportName* names = parseExportList(c, &nameCount, openBrace);
    bool hasFrom = match(c, TOKEN_FROM);
    if (hasFrom) {
      expression(c);
    }
    consume(c, TOKEN_SEMICOLON, "Expect ';' after export.");

    if (allowExport) {
      if (hasFrom) {
        emitByte(c, OP_IMPORT_MODULE, keyword);
        emitByte(c, OP_EXPORT_FROM, keyword);
        emitShort(c, (uint16_t)nameCount, keyword);
        for (int i = 0; i < nameCount; i++) {
          emitShort(c, names[i].from, keyword);
          emitShort(c, names[i].to, keyword);
        }
      } else {
        for (int i = 0; i < nameCount; i++) {
          emitGetVarConstant(c, names[i].from);
          emitExportValue(c, names[i].to, keyword);
        }
      }
    } else if (hasFrom) {
      emitByte(c, OP_POP, keyword);
    }

    free(names);
    emitGc(c);
    return;
  }

  if (match(c, TOKEN_LET)) {
    varDeclaration(c, false, allowExport);
    return;
  }
  if (match(c, TOKEN_CONST)) {
    varDeclaration(c, true, allowExport);
    return;
  }
  if (match(c, TOKEN_FUN)) {
    functionDeclaration(c, allowExport);
    return;
  }
  if (match(c, TOKEN_CLASS)) {
    classDeclaration(c, allowExport);
    return;
  }
  if (match(c, TOKEN_ENUM)) {
    enumDeclaration(c, allowExport);
    return;
  }

  Token name = consume(c, TOKEN_IDENTIFIER, "Expect declaration or identifier after 'export'.");
  consume(c, TOKEN_SEMICOLON, "Expect ';' after export.");
  if (allowExport) {
    emitExportName(c, name);
  }
}

static void declaration(Compiler* c) {
  if (match(c, TOKEN_EXPORT)) {
    exportDeclaration(c);
  } else if (match(c, TOKEN_CLASS)) {
    classDeclaration(c, false);
  } else if (match(c, TOKEN_FUN)) {
    functionDeclaration(c, false);
  } else if (match(c, TOKEN_CONST)) {
    varDeclaration(c, true, false);
  } else if (match(c, TOKEN_LET)) {
    varDeclaration(c, false, false);
  } else if (match(c, TOKEN_ENUM)) {
    enumDeclaration(c, false);
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
  } else if (match(c, TOKEN_SWITCH) || match(c, TOKEN_MATCH)) {
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
  Token openParen = consume(c, TOKEN_LEFT_PAREN, "Expect '(' after function name.");

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
  consumeClosing(c, TOKEN_RIGHT_PAREN, "Expect ')' after parameters.", openParen);
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
  consumeClosing(c, TOKEN_RIGHT_PAREN, "Expect ')' after parameters.", openParen);
  Token openBrace = consume(c, TOKEN_LEFT_BRACE, "Expect '{' before function body.");

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
  fnCompiler.pendingOptionalCall = false;
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
  consumeClosing(&fnCompiler, TOKEN_RIGHT_BRACE, "Expect '}' after function body.", openBrace);

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

  optimizeChunk(c->vm, chunk);
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
  c.pendingOptionalCall = false;
  c.breakContext = NULL;
  c.enclosing = NULL;
  vm->compiler = &c;

  while (!isAtEnd(&c)) {
    declaration(&c);
  }

  emitByte(&c, OP_NULL, noToken());
  emitByte(&c, OP_RETURN, noToken());

  vm->compiler = NULL;

  *hadError = c.hadError;
  if (c.hadError) {
    return NULL;
  }
  optimizeChunk(vm, chunk);
  return function;
}

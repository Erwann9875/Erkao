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
  {TOKEN_IMPLEMENTS, "implements"},
  {TOKEN_IN, "in"},
  {TOKEN_INTERFACE, "interface"},
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
    case TOKEN_QUESTION: return "'?'";
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
    case TOKEN_IMPLEMENTS: return "'implements'";
    case TOKEN_IN: return "'in'";
    case TOKEN_INTERFACE: return "'interface'";
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
static void errorAt(Compiler* c, Token token, const char* message);
static void errorAtCurrent(Compiler* c, const char* message);

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

  typedef enum {
    TYPE_ANY,
    TYPE_UNKNOWN,
    TYPE_NUMBER,
    TYPE_STRING,
    TYPE_BOOL,
    TYPE_NULL,
    TYPE_ARRAY,
    TYPE_MAP,
    TYPE_NAMED,
    TYPE_GENERIC,
    TYPE_FUNCTION
  } TypeKind;

  typedef struct {
    ObjString* name;
    ObjString* constraint;
  } TypeParam;

  typedef struct Type {
    TypeKind kind;
    ObjString* name;
    struct Type* elem;
    struct Type* key;
    struct Type* value;
    struct Type** params;
    int paramCount;
    struct Type* returnType;
    TypeParam* typeParams;
    int typeParamCount;
    struct Type** typeArgs;
    int typeArgCount;
    bool nullable;
  } Type;

  typedef struct {
    ObjString* name;
    Type* type;
    bool explicitType;
    int depth;
  } TypeEntry;

  typedef struct {
    ObjString* name;
    Type* type;
  } InterfaceMethod;

  typedef struct {
    ObjString* name;
    TypeParam* typeParams;
    int typeParamCount;
    InterfaceMethod* methods;
    int methodCount;
    int methodCapacity;
  } InterfaceDef;

  typedef struct {
    ObjString* name;
    ObjString** interfaces;
    int interfaceCount;
    int interfaceCapacity;
  } ClassDef;

  typedef struct {
    InterfaceDef* interfaces;
    int interfaceCount;
    int interfaceCapacity;
    ClassDef* classes;
    int classCount;
    int classCapacity;
  } TypeRegistry;

  typedef struct {
    ObjString* name;
    ObjString* constraint;
    Type* bound;
  } TypeBinding;

  typedef struct {
    ObjString* name;
    Type* type;
  } ClassMethod;

  static bool typeNamesEqual(ObjString* a, ObjString* b);

  struct TypeChecker {
    bool enabled;
    int errorCount;
    int scopeDepth;
    struct TypeChecker* enclosing;
    TypeEntry* entries;
    int count;
    int capacity;
    Type** stack;
    int stackCount;
    int stackCapacity;
    Type** allocated;
    int allocatedCount;
    int allocatedCapacity;
    Type* currentReturn;
    TypeParam* typeParams;
    int typeParamCount;
    int typeParamCapacity;
  };

  static Type TYPE_ANY_VALUE = { TYPE_ANY, NULL, NULL, NULL, NULL, NULL, 0, NULL,
                                 NULL, 0, NULL, 0, false };
  static Type TYPE_UNKNOWN_VALUE = { TYPE_UNKNOWN, NULL, NULL, NULL, NULL, NULL, 0, NULL,
                                     NULL, 0, NULL, 0, false };
  static Type TYPE_NUMBER_VALUE = { TYPE_NUMBER, NULL, NULL, NULL, NULL, NULL, 0, NULL,
                                    NULL, 0, NULL, 0, false };
  static Type TYPE_STRING_VALUE = { TYPE_STRING, NULL, NULL, NULL, NULL, NULL, 0, NULL,
                                    NULL, 0, NULL, 0, false };
  static Type TYPE_BOOL_VALUE = { TYPE_BOOL, NULL, NULL, NULL, NULL, NULL, 0, NULL,
                                  NULL, 0, NULL, 0, false };
  static Type TYPE_NULL_VALUE = { TYPE_NULL, NULL, NULL, NULL, NULL, NULL, 0, NULL,
                                  NULL, 0, NULL, 0, false };
  static TypeRegistry* gTypeRegistry = NULL;

static Type* typeNamed(TypeChecker* tc, ObjString* name);
static Type* typeGeneric(TypeChecker* tc, ObjString* name);
static Type* typeArray(TypeChecker* tc, Type* elem);
static Type* typeMap(TypeChecker* tc, Type* key, Type* value);
static Type* typeFunction(TypeChecker* tc, Type** params, int paramCount, Type* returnType);
static TypeParam* parseTypeParams(Compiler* c, int* outCount);
static void typeToString(Type* type, char* buffer, size_t size);
static void typeErrorAt(Compiler* c, Token token, const char* format, ...);

static bool typecheckEnabled(Compiler* c) {
  return c->typecheck && c->typecheck->enabled;
}

static Type* typeAny(void) { return &TYPE_ANY_VALUE; }
static Type* typeUnknown(void) { return &TYPE_UNKNOWN_VALUE; }
static Type* typeNumber(void) { return &TYPE_NUMBER_VALUE; }
static Type* typeString(void) { return &TYPE_STRING_VALUE; }
static Type* typeBool(void) { return &TYPE_BOOL_VALUE; }
static Type* typeNull(void) { return &TYPE_NULL_VALUE; }

static Type* typeAlloc(TypeChecker* tc, TypeKind kind) {
  if (!tc) return typeAny();
  Type* type = (Type*)malloc(sizeof(Type));
  if (!type) {
    fprintf(stderr, "Out of memory.\n");
    exit(1);
  }
  memset(type, 0, sizeof(Type));
  type->kind = kind;
  if (tc->allocatedCount >= tc->allocatedCapacity) {
    int oldCap = tc->allocatedCapacity;
    tc->allocatedCapacity = GROW_CAPACITY(oldCap);
    tc->allocated = GROW_ARRAY(Type*, tc->allocated, oldCap, tc->allocatedCapacity);
    if (!tc->allocated) {
      fprintf(stderr, "Out of memory.\n");
      exit(1);
    }
  }
  tc->allocated[tc->allocatedCount++] = type;
  return type;
}

  static void typeCheckerInit(TypeChecker* tc, TypeChecker* enclosing, bool enabled) {
    tc->enabled = enabled;
    tc->errorCount = 0;
    tc->scopeDepth = 0;
    tc->enclosing = enclosing;
    tc->entries = NULL;
    tc->count = 0;
    tc->capacity = 0;
    tc->stack = NULL;
    tc->stackCount = 0;
    tc->stackCapacity = 0;
    tc->allocated = NULL;
    tc->allocatedCount = 0;
    tc->allocatedCapacity = 0;
    tc->currentReturn = NULL;
    tc->typeParams = NULL;
    tc->typeParamCount = 0;
    tc->typeParamCapacity = 0;
  }

  static void typeCheckerFree(TypeChecker* tc) {
    if (!tc) return;
    for (int i = 0; i < tc->allocatedCount; i++) {
      if (tc->allocated[i]) {
        if (tc->allocated[i]->params) {
          free(tc->allocated[i]->params);
        }
        if (tc->allocated[i]->typeArgs) {
          free(tc->allocated[i]->typeArgs);
        }
        if (tc->allocated[i]->typeParams) {
          free(tc->allocated[i]->typeParams);
        }
        free(tc->allocated[i]);
      }
    }
    FREE_ARRAY(Type*, tc->allocated, tc->allocatedCapacity);
    FREE_ARRAY(TypeEntry, tc->entries, tc->capacity);
    FREE_ARRAY(Type*, tc->stack, tc->stackCapacity);
    FREE_ARRAY(TypeParam, tc->typeParams, tc->typeParamCapacity);
    tc->allocated = NULL;
    tc->allocatedCount = 0;
    tc->allocatedCapacity = 0;
    tc->entries = NULL;
    tc->count = 0;
    tc->capacity = 0;
    tc->stack = NULL;
    tc->stackCount = 0;
    tc->stackCapacity = 0;
    tc->typeParams = NULL;
    tc->typeParamCount = 0;
    tc->typeParamCapacity = 0;
  }

  static void typeRegistryInit(TypeRegistry* registry) {
    registry->interfaces = NULL;
    registry->interfaceCount = 0;
    registry->interfaceCapacity = 0;
    registry->classes = NULL;
    registry->classCount = 0;
    registry->classCapacity = 0;
  }

  static void typeRegistryFree(TypeRegistry* registry) {
    if (!registry) return;
    for (int i = 0; i < registry->interfaceCount; i++) {
      InterfaceDef* def = &registry->interfaces[i];
      if (def->methods) {
        free(def->methods);
        def->methods = NULL;
      }
      if (def->typeParams) {
        free(def->typeParams);
        def->typeParams = NULL;
      }
    }
    for (int i = 0; i < registry->classCount; i++) {
      ClassDef* def = &registry->classes[i];
      if (def->interfaces) {
        free(def->interfaces);
        def->interfaces = NULL;
      }
    }
    free(registry->interfaces);
    free(registry->classes);
    registry->interfaces = NULL;
    registry->classes = NULL;
    registry->interfaceCount = 0;
    registry->interfaceCapacity = 0;
    registry->classCount = 0;
    registry->classCapacity = 0;
  }

  static InterfaceDef* typeRegistryFindInterface(TypeRegistry* registry, ObjString* name) {
    if (!registry || !name) return NULL;
    for (int i = 0; i < registry->interfaceCount; i++) {
      if (typeNamesEqual(registry->interfaces[i].name, name)) {
        return &registry->interfaces[i];
      }
    }
    return NULL;
  }

  static ClassDef* typeRegistryFindClass(TypeRegistry* registry, ObjString* name) {
    if (!registry || !name) return NULL;
    for (int i = 0; i < registry->classCount; i++) {
      if (typeNamesEqual(registry->classes[i].name, name)) {
        return &registry->classes[i];
      }
    }
    return NULL;
  }

  static void typeRegistryAddInterface(TypeRegistry* registry, const InterfaceDef* def) {
    if (!registry || !def) return;
    if (registry->interfaceCount >= registry->interfaceCapacity) {
      int oldCap = registry->interfaceCapacity;
      registry->interfaceCapacity = GROW_CAPACITY(oldCap);
      registry->interfaces = GROW_ARRAY(InterfaceDef, registry->interfaces,
                                        oldCap, registry->interfaceCapacity);
      if (!registry->interfaces) {
        fprintf(stderr, "Out of memory.\n");
        exit(1);
      }
    }
    registry->interfaces[registry->interfaceCount++] = *def;
  }

  static void typeRegistryAddClass(TypeRegistry* registry, ObjString* name,
                                   ObjString** interfaces, int interfaceCount) {
    if (!registry || !name) return;
    ClassDef* existing = typeRegistryFindClass(registry, name);
    if (existing) {
      if (existing->interfaces) free(existing->interfaces);
      existing->interfaces = interfaces;
      existing->interfaceCount = interfaceCount;
      existing->interfaceCapacity = interfaceCount;
      return;
    }
    if (registry->classCount >= registry->classCapacity) {
      int oldCap = registry->classCapacity;
      registry->classCapacity = GROW_CAPACITY(oldCap);
      registry->classes = GROW_ARRAY(ClassDef, registry->classes,
                                     oldCap, registry->classCapacity);
      if (!registry->classes) {
        fprintf(stderr, "Out of memory.\n");
        exit(1);
      }
    }
    ClassDef* def = &registry->classes[registry->classCount++];
    def->name = name;
    def->interfaces = interfaces;
    def->interfaceCount = interfaceCount;
    def->interfaceCapacity = interfaceCount;
  }

  static bool typeRegistryClassImplements(TypeRegistry* registry, ObjString* className,
                                          ObjString* interfaceName) {
    if (!registry || !className || !interfaceName) return false;
    ClassDef* def = typeRegistryFindClass(registry, className);
    if (!def) return false;
    for (int i = 0; i < def->interfaceCount; i++) {
      if (typeNamesEqual(def->interfaces[i], interfaceName)) {
        return true;
      }
    }
    return false;
  }

  static void typeParamsEnsure(TypeChecker* tc, int needed) {
    if (!tc) return;
    if (tc->typeParamCount + needed <= tc->typeParamCapacity) return;
    int oldCap = tc->typeParamCapacity;
    while (tc->typeParamCount + needed > tc->typeParamCapacity) {
      tc->typeParamCapacity = GROW_CAPACITY(tc->typeParamCapacity);
    }
    tc->typeParams = GROW_ARRAY(TypeParam, tc->typeParams, oldCap, tc->typeParamCapacity);
    if (!tc->typeParams) {
      fprintf(stderr, "Out of memory.\n");
      exit(1);
    }
  }

  static void typeParamsPushList(TypeChecker* tc, const TypeParam* params, int count) {
    if (!tc || !params || count <= 0) return;
    typeParamsEnsure(tc, count);
    for (int i = 0; i < count; i++) {
      tc->typeParams[tc->typeParamCount++] = params[i];
    }
  }

  static void typeParamsTruncate(TypeChecker* tc, int count) {
    if (!tc) return;
    if (count < 0) count = 0;
    if (count > tc->typeParamCount) count = tc->typeParamCount;
    tc->typeParamCount = count;
  }

  static TypeParam* typeParamFindToken(TypeChecker* tc, Token token) {
    if (!tc) return NULL;
    for (TypeChecker* cur = tc; cur != NULL; cur = cur->enclosing) {
      for (int i = cur->typeParamCount - 1; i >= 0; i--) {
        TypeParam* param = &cur->typeParams[i];
        if (!param->name) continue;
        if (param->name->length == token.length &&
            memcmp(param->name->chars, token.start, (size_t)token.length) == 0) {
          return param;
        }
      }
    }
    return NULL;
  }

static void typePush(Compiler* c, Type* type) {
  if (!typecheckEnabled(c)) return;
  TypeChecker* tc = c->typecheck;
  if (tc->stackCount >= tc->stackCapacity) {
    int oldCap = tc->stackCapacity;
    tc->stackCapacity = GROW_CAPACITY(oldCap);
    tc->stack = GROW_ARRAY(Type*, tc->stack, oldCap, tc->stackCapacity);
    if (!tc->stack) {
      fprintf(stderr, "Out of memory.\n");
      exit(1);
    }
  }
  tc->stack[tc->stackCount++] = type;
}

static Type* typePop(Compiler* c) {
  if (!typecheckEnabled(c)) return typeAny();
  TypeChecker* tc = c->typecheck;
  if (tc->stackCount <= 0) return typeAny();
  return tc->stack[--tc->stackCount];
}

static bool typeIsAny(Type* type) {
  if (!type) return true;
  return type->kind == TYPE_ANY || type->kind == TYPE_UNKNOWN || type->kind == TYPE_GENERIC;
}

static bool typeIsNullable(Type* type) {
  if (!type) return false;
  return type->kind == TYPE_NULL || type->nullable;
}

static Type* typeClone(TypeChecker* tc, Type* src) {
  if (!src) return typeAny();
  switch (src->kind) {
    case TYPE_ANY:
      return typeAny();
    case TYPE_UNKNOWN:
      return typeUnknown();
    case TYPE_NUMBER: {
      if (!src->nullable) return typeNumber();
      Type* type = typeAlloc(tc, TYPE_NUMBER);
      type->nullable = true;
      return type;
    }
    case TYPE_STRING: {
      if (!src->nullable) return typeString();
      Type* type = typeAlloc(tc, TYPE_STRING);
      type->nullable = true;
      return type;
    }
    case TYPE_BOOL: {
      if (!src->nullable) return typeBool();
      Type* type = typeAlloc(tc, TYPE_BOOL);
      type->nullable = true;
      return type;
    }
    case TYPE_NULL:
      return typeNull();
      case TYPE_NAMED: {
        Type* type = typeAlloc(tc, TYPE_NAMED);
        type->name = src->name;
        type->nullable = src->nullable;
        if (src->typeArgCount > 0 && src->typeArgs) {
          type->typeArgCount = src->typeArgCount;
          type->typeArgs = (Type**)malloc(sizeof(Type*) * (size_t)src->typeArgCount);
          if (!type->typeArgs) {
            fprintf(stderr, "Out of memory.\n");
            exit(1);
          }
          for (int i = 0; i < src->typeArgCount; i++) {
            type->typeArgs[i] = typeClone(tc, src->typeArgs[i]);
          }
        }
        return type;
      }
      case TYPE_GENERIC: {
        Type* type = typeAlloc(tc, TYPE_GENERIC);
        type->name = src->name;
        type->nullable = src->nullable;
        return type;
      }
      case TYPE_ARRAY: {
        Type* type = typeAlloc(tc, TYPE_ARRAY);
        type->elem = typeClone(tc, src->elem);
        type->nullable = src->nullable;
        return type;
    }
    case TYPE_MAP: {
      Type* type = typeAlloc(tc, TYPE_MAP);
      type->key = typeClone(tc, src->key);
      type->value = typeClone(tc, src->value);
      type->nullable = src->nullable;
      return type;
    }
      case TYPE_FUNCTION: {
        Type* type = typeAlloc(tc, TYPE_FUNCTION);
        type->paramCount = src->paramCount;
        type->returnType = typeClone(tc, src->returnType);
        type->nullable = src->nullable;
        if (src->typeParamCount > 0 && src->typeParams) {
          type->typeParamCount = src->typeParamCount;
          type->typeParams = (TypeParam*)malloc(sizeof(TypeParam) * (size_t)src->typeParamCount);
          if (!type->typeParams) {
            fprintf(stderr, "Out of memory.\n");
            exit(1);
          }
          for (int i = 0; i < src->typeParamCount; i++) {
            type->typeParams[i] = src->typeParams[i];
          }
        }
        if (src->paramCount > 0 && src->params) {
          type->params = (Type**)malloc(sizeof(Type*) * (size_t)src->paramCount);
          if (!type->params) {
            fprintf(stderr, "Out of memory.\n");
          exit(1);
        }
        for (int i = 0; i < src->paramCount; i++) {
          type->params[i] = typeClone(tc, src->params[i]);
        }
      }
      return type;
    }
  }
  return typeAny();
}

static Type* typeMakeNullable(TypeChecker* tc, Type* type) {
  if (!type) return typeAny();
  if (type->kind == TYPE_ANY) return typeAny();
  if (type->kind == TYPE_UNKNOWN) return typeUnknown();
  if (type->kind == TYPE_NULL) return typeNull();
  if (type->nullable) return type;
  if (!tc) return type;
  if (type->kind == TYPE_NUMBER || type->kind == TYPE_STRING || type->kind == TYPE_BOOL) {
    Type* copy = typeAlloc(tc, type->kind);
    copy->nullable = true;
    return copy;
  }
  Type* copy = typeClone(tc, type);
  if (copy) copy->nullable = true;
  return copy;
}

static bool typeNamesEqual(ObjString* a, ObjString* b) {
  if (a == b) return true;
  if (!a || !b) return false;
  if (a->length != b->length) return false;
  return memcmp(a->chars, b->chars, (size_t)a->length) == 0;
}

static bool typeEquals(Type* a, Type* b) {
  if (a == b) return true;
  if (!a || !b) return false;
  if (a->kind != b->kind) return false;
  if (a->kind != TYPE_NULL && a->nullable != b->nullable) return false;
  switch (a->kind) {
    case TYPE_ANY:
    case TYPE_UNKNOWN:
    case TYPE_NUMBER:
    case TYPE_STRING:
    case TYPE_BOOL:
    case TYPE_NULL:
      return true;
      case TYPE_NAMED:
        if (!typeNamesEqual(a->name, b->name)) return false;
        if (a->typeArgCount == 0 && b->typeArgCount == 0) return true;
        if (a->typeArgCount != b->typeArgCount) return false;
        for (int i = 0; i < a->typeArgCount; i++) {
          if (!typeEquals(a->typeArgs[i], b->typeArgs[i])) return false;
        }
        return true;
      case TYPE_GENERIC:
        return typeNamesEqual(a->name, b->name);
    case TYPE_ARRAY:
      return typeEquals(a->elem, b->elem);
    case TYPE_MAP:
      return typeEquals(a->key, b->key) && typeEquals(a->value, b->value);
    case TYPE_FUNCTION:
      if (a->paramCount != b->paramCount) return false;
      for (int i = 0; i < a->paramCount; i++) {
        if (!typeEquals(a->params[i], b->params[i])) return false;
      }
      return typeEquals(a->returnType, b->returnType);
  }
  return false;
}

  static bool typeAssignable(Type* dst, Type* src) {
    if (typeIsAny(dst) || typeIsAny(src)) return true;
    if (!dst || !src) return true;
    if (dst->kind == TYPE_NULL) return src->kind == TYPE_NULL;
    if (src->kind == TYPE_NULL) return dst->nullable;
    if (typeIsNullable(src) && !typeIsNullable(dst)) return false;
    if (dst->kind == TYPE_NAMED && src->kind == TYPE_NAMED) {
      if (typeNamesEqual(dst->name, src->name)) {
        if (dst->typeArgCount == 0 || src->typeArgCount == 0) return true;
        if (dst->typeArgCount != src->typeArgCount) return false;
        for (int i = 0; i < dst->typeArgCount; i++) {
          if (!typeAssignable(dst->typeArgs[i], src->typeArgs[i])) return false;
        }
        return true;
      }
      if (gTypeRegistry &&
          typeRegistryClassImplements(gTypeRegistry, src->name, dst->name)) {
        return true;
      }
      return false;
    }
    if (dst->kind != src->kind) return false;
    switch (dst->kind) {
      case TYPE_ANY:
      case TYPE_UNKNOWN:
      case TYPE_NUMBER:
      case TYPE_STRING:
      case TYPE_BOOL:
      case TYPE_NULL:
        return true;
      case TYPE_NAMED:
        return false;
      case TYPE_GENERIC:
        return true;
      case TYPE_ARRAY:
        return typeAssignable(dst->elem, src->elem);
      case TYPE_MAP:
        return typeAssignable(dst->key, src->key) && typeAssignable(dst->value, src->value);
      case TYPE_FUNCTION:
      if (dst->paramCount != src->paramCount) return false;
      for (int i = 0; i < dst->paramCount; i++) {
        if (!typeAssignable(dst->params[i], src->params[i])) return false;
      }
      return typeAssignable(dst->returnType, src->returnType);
  }
    return true;
  }

  static TypeBinding* typeBindingFind(TypeBinding* bindings, int count, ObjString* name) {
    if (!bindings || !name) return NULL;
    for (int i = 0; i < count; i++) {
      if (typeNamesEqual(bindings[i].name, name)) {
        return &bindings[i];
      }
    }
    return NULL;
  }

  static bool typeSatisfiesConstraint(Type* actual, ObjString* constraint) {
    if (!constraint) return true;
    if (!actual || typeIsAny(actual)) return true;
    if (actual->kind == TYPE_NAMED) {
      if (typeNamesEqual(actual->name, constraint)) return true;
      return gTypeRegistry && typeRegistryClassImplements(gTypeRegistry, actual->name, constraint);
    }
    return false;
  }

  static bool typeUnify(Compiler* c, Type* pattern, Type* actual,
                        TypeBinding* bindings, int bindingCount, Token token) {
    if (!pattern || !actual) return true;
    if (pattern->kind == TYPE_GENERIC) {
      TypeBinding* binding = typeBindingFind(bindings, bindingCount, pattern->name);
      if (!binding) return true;
      if (!binding->bound) {
        if (!typeSatisfiesConstraint(actual, binding->constraint)) {
          char expected[64];
          typeToString(typeNamed(c->typecheck, binding->constraint), expected, sizeof(expected));
          typeErrorAt(c, token, "Type argument for '%s' must implement %s.",
                      binding->name ? binding->name->chars : "T", expected);
          return false;
        }
        binding->bound = actual;
        return true;
      }
      return typeAssignable(binding->bound, actual);
    }
    if (pattern->kind == TYPE_ARRAY) {
      if (actual->kind != TYPE_ARRAY && !typeIsAny(actual)) return false;
      if (actual->kind == TYPE_ARRAY) {
        return typeUnify(c, pattern->elem, actual->elem, bindings, bindingCount, token);
      }
      return true;
    }
    if (pattern->kind == TYPE_MAP) {
      if (actual->kind != TYPE_MAP && !typeIsAny(actual)) return false;
      if (actual->kind == TYPE_MAP) {
        if (!typeUnify(c, pattern->key, actual->key, bindings, bindingCount, token)) return false;
        return typeUnify(c, pattern->value, actual->value, bindings, bindingCount, token);
      }
      return true;
    }
    if (pattern->kind == TYPE_FUNCTION && actual->kind == TYPE_FUNCTION) {
      if (pattern->paramCount >= 0 && actual->paramCount >= 0 &&
          pattern->paramCount != actual->paramCount) {
        return false;
      }
      int count = pattern->paramCount >= 0 ? pattern->paramCount : actual->paramCount;
      for (int i = 0; i < count; i++) {
        if (!typeUnify(c, pattern->params[i], actual->params[i], bindings, bindingCount, token)) {
          return false;
        }
      }
      return typeUnify(c, pattern->returnType, actual->returnType, bindings, bindingCount, token);
    }
    if (pattern->kind == TYPE_NAMED && actual->kind == TYPE_NAMED) {
      if (!typeNamesEqual(pattern->name, actual->name)) return false;
      if (pattern->typeArgCount == 0 || actual->typeArgCount == 0) return true;
      if (pattern->typeArgCount != actual->typeArgCount) return false;
      for (int i = 0; i < pattern->typeArgCount; i++) {
        if (!typeUnify(c, pattern->typeArgs[i], actual->typeArgs[i], bindings, bindingCount, token)) {
          return false;
        }
      }
      return true;
    }
    return typeAssignable(pattern, actual);
  }

  static Type* typeSubstitute(TypeChecker* tc, Type* type,
                              TypeBinding* bindings, int bindingCount) {
    if (!type) return typeAny();
    if (type->kind == TYPE_GENERIC) {
      TypeBinding* binding = typeBindingFind(bindings, bindingCount, type->name);
      if (binding && binding->bound) return binding->bound;
      return typeAny();
    }
    if (type->kind == TYPE_ARRAY) {
      Type* elem = typeSubstitute(tc, type->elem, bindings, bindingCount);
      Type* result = typeArray(tc, elem);
      result->nullable = type->nullable;
      return result;
    }
    if (type->kind == TYPE_MAP) {
      Type* key = typeSubstitute(tc, type->key, bindings, bindingCount);
      Type* value = typeSubstitute(tc, type->value, bindings, bindingCount);
      Type* result = typeMap(tc, key, value);
      result->nullable = type->nullable;
      return result;
    }
    if (type->kind == TYPE_NAMED) {
      Type* result = typeNamed(tc, type->name);
      result->nullable = type->nullable;
      if (type->typeArgCount > 0 && type->typeArgs) {
        result->typeArgCount = type->typeArgCount;
        result->typeArgs = (Type**)malloc(sizeof(Type*) * (size_t)type->typeArgCount);
        if (!result->typeArgs) {
          fprintf(stderr, "Out of memory.\n");
          exit(1);
        }
        for (int i = 0; i < type->typeArgCount; i++) {
          result->typeArgs[i] = typeSubstitute(tc, type->typeArgs[i], bindings, bindingCount);
        }
      }
      return result;
    }
    if (type->kind == TYPE_FUNCTION) {
      Type* result = typeAlloc(tc, TYPE_FUNCTION);
      result->paramCount = type->paramCount;
      result->returnType = typeSubstitute(tc, type->returnType, bindings, bindingCount);
      result->nullable = type->nullable;
      if (type->paramCount > 0 && type->params) {
        result->params = (Type**)malloc(sizeof(Type*) * (size_t)type->paramCount);
        if (!result->params) {
          fprintf(stderr, "Out of memory.\n");
          exit(1);
        }
        for (int i = 0; i < type->paramCount; i++) {
          result->params[i] = typeSubstitute(tc, type->params[i], bindings, bindingCount);
        }
      }
      return result;
    }
    return typeClone(tc, type);
  }

static void typeToString(Type* type, char* buffer, size_t size) {
  if (!buffer || size == 0) return;
  if (!type) {
    snprintf(buffer, size, "any");
    return;
  }
  switch (type->kind) {
    case TYPE_ANY:
      snprintf(buffer, size, "any");
      return;
    case TYPE_UNKNOWN:
      snprintf(buffer, size, "unknown");
      return;
    case TYPE_NUMBER:
      snprintf(buffer, size, type->nullable ? "number?" : "number");
      return;
    case TYPE_STRING:
      snprintf(buffer, size, type->nullable ? "string?" : "string");
      return;
    case TYPE_BOOL:
      snprintf(buffer, size, type->nullable ? "bool?" : "bool");
      return;
    case TYPE_NULL:
      snprintf(buffer, size, "null");
      return;
      case TYPE_NAMED:
        if (type->name) {
          size_t used = (size_t)snprintf(buffer, size, "%s", type->name->chars);
          if (type->typeArgCount > 0 && type->typeArgs && used < size) {
            used += (size_t)snprintf(buffer + used, size - used, "<");
            for (int i = 0; i < type->typeArgCount && used < size; i++) {
              char arg[32];
              typeToString(type->typeArgs[i], arg, sizeof(arg));
              used += (size_t)snprintf(buffer + used, size - used, "%s%s",
                                       i > 0 ? ", " : "", arg);
            }
            if (used < size) {
              used += (size_t)snprintf(buffer + used, size - used, ">");
            }
          }
          if (type->nullable && used < size) {
            (void)snprintf(buffer + used, size - used, "?");
          }
        } else {
          snprintf(buffer, size, "named");
        }
        return;
      case TYPE_GENERIC:
        if (type->name) {
          if (type->nullable) {
            snprintf(buffer, size, "%s?", type->name->chars);
          } else {
            snprintf(buffer, size, "%s", type->name->chars);
          }
        } else {
          snprintf(buffer, size, "T");
        }
        return;
    case TYPE_ARRAY: {
      char inner[64];
      typeToString(type->elem, inner, sizeof(inner));
      if (type->nullable) {
        snprintf(buffer, size, "array<%s>?", inner);
      } else {
        snprintf(buffer, size, "array<%s>", inner);
      }
      return;
    }
    case TYPE_MAP: {
      char key[64];
      char value[64];
      typeToString(type->key, key, sizeof(key));
      typeToString(type->value, value, sizeof(value));
      if (type->nullable) {
        snprintf(buffer, size, "map<%s, %s>?", key, value);
      } else {
        snprintf(buffer, size, "map<%s, %s>", key, value);
      }
      return;
    }
    case TYPE_FUNCTION:
      snprintf(buffer, size, type->nullable ? "fun?" : "fun");
      return;
  }
  snprintf(buffer, size, "any");
}

static void typeErrorAt(Compiler* c, Token token, const char* format, ...) {
  if (!typecheckEnabled(c)) return;
  if (c->panicMode) return;
  char message[256];
  va_list args;
  va_start(args, format);
  vsnprintf(message, sizeof(message), format, args);
  va_end(args);
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
  if (c->typecheck) {
    c->typecheck->errorCount++;
  }
}

static void typeCheckerEnterScope(Compiler* c) {
  if (!typecheckEnabled(c)) return;
  c->typecheck->scopeDepth = c->scopeDepth;
}

static void typeCheckerExitScope(Compiler* c) {
  if (!typecheckEnabled(c)) return;
  TypeChecker* tc = c->typecheck;
  int targetDepth = c->scopeDepth;
  while (tc->count > 0 && tc->entries[tc->count - 1].depth > targetDepth) {
    tc->count--;
  }
  tc->scopeDepth = targetDepth;
}

static void typeDefine(Compiler* c, Token name, Type* type, bool explicitType) {
  if (!typecheckEnabled(c)) return;
  TypeChecker* tc = c->typecheck;
  if (tc->count >= tc->capacity) {
    int oldCap = tc->capacity;
    tc->capacity = GROW_CAPACITY(oldCap);
    tc->entries = GROW_ARRAY(TypeEntry, tc->entries, oldCap, tc->capacity);
    if (!tc->entries) {
      fprintf(stderr, "Out of memory.\n");
      exit(1);
    }
  }
  ObjString* nameStr = stringFromToken(c->vm, name);
  tc->entries[tc->count].name = nameStr;
  tc->entries[tc->count].type = type ? type : typeAny();
  tc->entries[tc->count].explicitType = explicitType;
  tc->entries[tc->count].depth = c->scopeDepth;
  tc->count++;
}

static TypeEntry* typeLookupEntry(TypeChecker* tc, ObjString* name) {
  if (!tc) return NULL;
  for (int i = tc->count - 1; i >= 0; i--) {
    if (tc->entries[i].name == name) {
      return &tc->entries[i];
    }
  }
  if (tc->enclosing) {
    return typeLookupEntry(tc->enclosing, name);
  }
  return NULL;
}

static Type* typeLookup(Compiler* c, Token name) {
  if (!typecheckEnabled(c)) return typeAny();
  ObjString* nameStr = stringFromToken(c->vm, name);
  TypeEntry* entry = typeLookupEntry(c->typecheck, nameStr);
  if (!entry) return typeAny();
  return entry->type ? entry->type : typeAny();
}

static void typeAssign(Compiler* c, Token name, Type* valueType) {
  if (!typecheckEnabled(c)) return;
  ObjString* nameStr = stringFromToken(c->vm, name);
  TypeEntry* entry = typeLookupEntry(c->typecheck, nameStr);
  if (!entry) {
    return;
  }
  Type* target = entry->type ? entry->type : typeAny();
  if (entry->explicitType) {
    if (!typeAssignable(target, valueType)) {
      char expected[64];
      char got[64];
      typeToString(target, expected, sizeof(expected));
      typeToString(valueType, got, sizeof(got));
      typeErrorAt(c, name, "Type mismatch. Expected %s but got %s.", expected, got);
    }
    return;
  }
  if (target->kind == TYPE_UNKNOWN) {
    entry->type = valueType ? valueType : typeAny();
    return;
  }
  if (typeIsAny(target) || typeIsAny(valueType)) {
    return;
  }
  if (valueType->kind == TYPE_NULL && target->kind != TYPE_NULL) {
    entry->type = typeMakeNullable(c->typecheck, target);
    return;
  }
  if (target->kind == TYPE_NULL && valueType->kind != TYPE_NULL) {
    entry->type = typeMakeNullable(c->typecheck, valueType);
    return;
  }
  if (!typeAssignable(target, valueType)) {
    if (target->kind == valueType->kind && typeIsNullable(valueType)) {
      entry->type = typeMakeNullable(c->typecheck, target);
      return;
    }
    {
      char expected[64];
      char got[64];
      typeToString(target, expected, sizeof(expected));
      typeToString(valueType, got, sizeof(got));
      typeErrorAt(c, name, "Type mismatch. Expected %s but got %s.", expected, got);
    }
    return;
  }
}

static bool tokenMatches(Token token, const char* text) {
  int length = (int)strlen(text);
  if (token.length != length) return false;
  return memcmp(token.start, text, (size_t)length) == 0;
}

static Token syntheticToken(const char* text) {
  Token token;
  memset(&token, 0, sizeof(Token));
  token.start = text;
  token.length = (int)strlen(text);
  return token;
}

static void typeDefineSynthetic(Compiler* c, const char* name, Type* type) {
  if (!typecheckEnabled(c)) return;
  Token token = syntheticToken(name);
  typeDefine(c, token, type, true);
}

static bool typeNamedIs(Type* type, const char* name) {
  if (!type || type->kind != TYPE_NAMED || !type->name || !name) return false;
  size_t length = strlen(name);
  if ((size_t)type->name->length != length) return false;
  return memcmp(type->name->chars, name, length) == 0;
}

static Type* typeFunctionN(TypeChecker* tc, int paramCount, Type* returnType, ...) {
  if (paramCount < 0) {
    return typeFunction(tc, NULL, -1, returnType);
  }
  Type* params[8];
  if (paramCount > (int)(sizeof(params) / sizeof(params[0]))) {
    paramCount = (int)(sizeof(params) / sizeof(params[0]));
  }
  va_list args;
  va_start(args, returnType);
  for (int i = 0; i < paramCount; i++) {
    params[i] = va_arg(args, Type*);
  }
  va_end(args);
  return typeFunction(tc, params, paramCount, returnType);
}

static Type* typeLookupStdlibMember(Compiler* c, Type* objectType, Token name) {
  if (!typecheckEnabled(c)) return typeAny();
  if (!objectType || typeIsAny(objectType)) return typeAny();
  if (objectType->kind != TYPE_NAMED || !objectType->name) return typeAny();
  TypeChecker* tc = c->typecheck;
  Type* any = typeAny();
  Type* number = typeNumber();
  Type* string = typeString();
  Type* boolean = typeBool();

  if (typeNamedIs(objectType, "fs")) {
    Type* arrayString = typeArray(tc, string);
    if (tokenMatches(name, "readText")) return typeFunctionN(tc, 1, string, string);
    if (tokenMatches(name, "writeText")) return typeFunctionN(tc, 2, boolean, string, string);
    if (tokenMatches(name, "exists")) return typeFunctionN(tc, 1, boolean, string);
    if (tokenMatches(name, "cwd")) return typeFunctionN(tc, 0, string);
    if (tokenMatches(name, "listDir")) return typeFunctionN(tc, 1, arrayString, string);
    if (tokenMatches(name, "isFile")) return typeFunctionN(tc, 1, boolean, string);
    if (tokenMatches(name, "isDir")) return typeFunctionN(tc, 1, boolean, string);
    if (tokenMatches(name, "size")) return typeFunctionN(tc, 1, number, string);
    if (tokenMatches(name, "glob")) return typeFunctionN(tc, 1, arrayString, string);
  }

  if (typeNamedIs(objectType, "path")) {
    Type* arrayString = typeArray(tc, string);
    if (tokenMatches(name, "join")) return typeFunctionN(tc, 2, string, string, string);
    if (tokenMatches(name, "dirname")) return typeFunctionN(tc, 1, string, string);
    if (tokenMatches(name, "basename")) return typeFunctionN(tc, 1, string, string);
    if (tokenMatches(name, "extname")) return typeFunctionN(tc, 1, string, string);
    if (tokenMatches(name, "isAbs")) return typeFunctionN(tc, 1, boolean, string);
    if (tokenMatches(name, "normalize")) return typeFunctionN(tc, 1, string, string);
    if (tokenMatches(name, "stem")) return typeFunctionN(tc, 1, string, string);
    if (tokenMatches(name, "split")) return typeFunctionN(tc, 1, arrayString, string);
  }

  if (typeNamedIs(objectType, "json") || typeNamedIs(objectType, "yaml")) {
    if (tokenMatches(name, "parse")) return typeFunctionN(tc, 1, any, string);
    if (tokenMatches(name, "stringify")) return typeFunctionN(tc, 1, string, any);
  }

  if (typeNamedIs(objectType, "math")) {
    if (tokenMatches(name, "abs")) return typeFunctionN(tc, 1, number, number);
    if (tokenMatches(name, "floor")) return typeFunctionN(tc, 1, number, number);
    if (tokenMatches(name, "ceil")) return typeFunctionN(tc, 1, number, number);
    if (tokenMatches(name, "round")) return typeFunctionN(tc, 1, number, number);
    if (tokenMatches(name, "sqrt")) return typeFunctionN(tc, 1, number, number);
    if (tokenMatches(name, "pow")) return typeFunctionN(tc, 2, number, number, number);
    if (tokenMatches(name, "min")) return typeFunctionN(tc, -1, number);
    if (tokenMatches(name, "max")) return typeFunctionN(tc, -1, number);
    if (tokenMatches(name, "clamp")) return typeFunctionN(tc, 3, number, number, number, number);
    if (tokenMatches(name, "PI") || tokenMatches(name, "E")) return number;
  }

  if (typeNamedIs(objectType, "random")) {
    Type* arrayAny = typeArray(tc, any);
    if (tokenMatches(name, "seed")) return typeFunctionN(tc, 1, typeNull(), number);
    if (tokenMatches(name, "int")) return typeFunctionN(tc, -1, number);
    if (tokenMatches(name, "float")) return typeFunctionN(tc, -1, number);
    if (tokenMatches(name, "choice")) return typeFunctionN(tc, 1, any, arrayAny);
    if (tokenMatches(name, "normal")) return typeFunctionN(tc, 2, number, number, number);
    if (tokenMatches(name, "gaussian")) return typeFunctionN(tc, 2, number, number, number);
    if (tokenMatches(name, "exponential")) return typeFunctionN(tc, 1, number, number);
    if (tokenMatches(name, "uniform")) return typeFunctionN(tc, -1, number);
  }

  if (typeNamedIs(objectType, "str")) {
    Type* arrayString = typeArray(tc, string);
    if (tokenMatches(name, "upper")) return typeFunctionN(tc, 1, string, string);
    if (tokenMatches(name, "lower")) return typeFunctionN(tc, 1, string, string);
    if (tokenMatches(name, "trim")) return typeFunctionN(tc, 1, string, string);
    if (tokenMatches(name, "trimStart")) return typeFunctionN(tc, 1, string, string);
    if (tokenMatches(name, "trimEnd")) return typeFunctionN(tc, 1, string, string);
    if (tokenMatches(name, "startsWith")) return typeFunctionN(tc, 2, boolean, string, string);
    if (tokenMatches(name, "endsWith")) return typeFunctionN(tc, 2, boolean, string, string);
    if (tokenMatches(name, "contains")) return typeFunctionN(tc, 2, boolean, string, string);
    if (tokenMatches(name, "split")) return typeFunctionN(tc, 2, arrayString, string, string);
    if (tokenMatches(name, "join")) return typeFunctionN(tc, 2, string, arrayString, string);
    if (tokenMatches(name, "builder")) return typeFunctionN(tc, 0, arrayString);
    if (tokenMatches(name, "append")) return typeFunctionN(tc, 2, arrayString, arrayString, string);
    if (tokenMatches(name, "build")) return typeFunctionN(tc, -1, string);
    if (tokenMatches(name, "replace")) return typeFunctionN(tc, 3, string, string, string, string);
    if (tokenMatches(name, "replaceAll")) return typeFunctionN(tc, 3, string, string, string, string);
    if (tokenMatches(name, "repeat")) return typeFunctionN(tc, 2, string, string, number);
  }

  if (typeNamedIs(objectType, "array")) {
    Type* arrayAny = typeArray(tc, any);
    if (tokenMatches(name, "slice")) return typeFunctionN(tc, -1, arrayAny);
    if (tokenMatches(name, "map")) {
      Type* params[2] = { arrayAny, typeFunctionN(tc, 1, any, any) };
      return typeFunction(tc, params, 2, arrayAny);
    }
    if (tokenMatches(name, "filter")) {
      Type* params[2] = { arrayAny, typeFunctionN(tc, 1, boolean, any) };
      return typeFunction(tc, params, 2, arrayAny);
    }
    if (tokenMatches(name, "reduce")) return typeFunctionN(tc, -1, any);
    if (tokenMatches(name, "contains")) return typeFunctionN(tc, 2, boolean, arrayAny, any);
    if (tokenMatches(name, "indexOf")) return typeFunctionN(tc, 2, number, arrayAny, any);
    if (tokenMatches(name, "concat")) return typeFunctionN(tc, 2, arrayAny, arrayAny, arrayAny);
    if (tokenMatches(name, "reverse")) return typeFunctionN(tc, 1, arrayAny, arrayAny);
  }

  if (typeNamedIs(objectType, "os")) {
    if (tokenMatches(name, "platform")) return typeFunctionN(tc, 0, string);
    if (tokenMatches(name, "arch")) return typeFunctionN(tc, 0, string);
    if (tokenMatches(name, "sep")) return typeFunctionN(tc, 0, string);
    if (tokenMatches(name, "eol")) return typeFunctionN(tc, 0, string);
    if (tokenMatches(name, "cwd")) return typeFunctionN(tc, 0, string);
    if (tokenMatches(name, "home")) return typeFunctionN(tc, 0, string);
    if (tokenMatches(name, "tmp")) return typeFunctionN(tc, 0, string);
  }

  if (typeNamedIs(objectType, "time")) {
    Type* mapAny = typeMap(tc, string, any);
    if (tokenMatches(name, "now")) return typeFunctionN(tc, 0, number);
    if (tokenMatches(name, "sleep")) return typeFunctionN(tc, 1, typeNull(), number);
    if (tokenMatches(name, "format")) return typeFunctionN(tc, -1, string);
    if (tokenMatches(name, "iso")) return typeFunctionN(tc, -1, string);
    if (tokenMatches(name, "parts")) return typeFunctionN(tc, -1, mapAny);
  }

  if (typeNamedIs(objectType, "di")) {
    Type* mapAny = typeMap(tc, string, any);
    if (tokenMatches(name, "container")) return typeFunctionN(tc, 0, mapAny);
    if (tokenMatches(name, "bind")) return typeFunctionN(tc, 3, typeNull(), mapAny, string, any);
    if (tokenMatches(name, "singleton")) return typeFunctionN(tc, 3, typeNull(), mapAny, string, any);
    if (tokenMatches(name, "value")) return typeFunctionN(tc, 3, typeNull(), mapAny, string, any);
    if (tokenMatches(name, "resolve")) return typeFunctionN(tc, 2, any, mapAny, string);
  }

  if (typeNamedIs(objectType, "vec2")) {
    Type* arrayNumber = typeArray(tc, number);
    if (tokenMatches(name, "make")) return typeFunctionN(tc, 2, arrayNumber, number, number);
    if (tokenMatches(name, "add")) return typeFunctionN(tc, 2, arrayNumber, arrayNumber, arrayNumber);
    if (tokenMatches(name, "sub")) return typeFunctionN(tc, 2, arrayNumber, arrayNumber, arrayNumber);
    if (tokenMatches(name, "scale")) return typeFunctionN(tc, 2, arrayNumber, arrayNumber, number);
    if (tokenMatches(name, "dot")) return typeFunctionN(tc, 2, number, arrayNumber, arrayNumber);
    if (tokenMatches(name, "len")) return typeFunctionN(tc, 1, number, arrayNumber);
    if (tokenMatches(name, "norm")) return typeFunctionN(tc, 1, arrayNumber, arrayNumber);
    if (tokenMatches(name, "lerp")) return typeFunctionN(tc, 3, arrayNumber, arrayNumber, arrayNumber, number);
    if (tokenMatches(name, "dist")) return typeFunctionN(tc, 2, number, arrayNumber, arrayNumber);
  }

  if (typeNamedIs(objectType, "vec3")) {
    Type* arrayNumber = typeArray(tc, number);
    if (tokenMatches(name, "make")) return typeFunctionN(tc, 3, arrayNumber, number, number, number);
    if (tokenMatches(name, "add")) return typeFunctionN(tc, 2, arrayNumber, arrayNumber, arrayNumber);
    if (tokenMatches(name, "sub")) return typeFunctionN(tc, 2, arrayNumber, arrayNumber, arrayNumber);
    if (tokenMatches(name, "scale")) return typeFunctionN(tc, 2, arrayNumber, arrayNumber, number);
    if (tokenMatches(name, "dot")) return typeFunctionN(tc, 2, number, arrayNumber, arrayNumber);
    if (tokenMatches(name, "len")) return typeFunctionN(tc, 1, number, arrayNumber);
    if (tokenMatches(name, "norm")) return typeFunctionN(tc, 1, arrayNumber, arrayNumber);
    if (tokenMatches(name, "lerp")) return typeFunctionN(tc, 3, arrayNumber, arrayNumber, arrayNumber, number);
    if (tokenMatches(name, "dist")) return typeFunctionN(tc, 2, number, arrayNumber, arrayNumber);
    if (tokenMatches(name, "cross")) return typeFunctionN(tc, 2, arrayNumber, arrayNumber, arrayNumber);
  }

  if (typeNamedIs(objectType, "vec4")) {
    Type* arrayNumber = typeArray(tc, number);
    if (tokenMatches(name, "make")) return typeFunctionN(tc, 4, arrayNumber, number, number, number, number);
    if (tokenMatches(name, "add")) return typeFunctionN(tc, 2, arrayNumber, arrayNumber, arrayNumber);
    if (tokenMatches(name, "sub")) return typeFunctionN(tc, 2, arrayNumber, arrayNumber, arrayNumber);
    if (tokenMatches(name, "scale")) return typeFunctionN(tc, 2, arrayNumber, arrayNumber, number);
    if (tokenMatches(name, "dot")) return typeFunctionN(tc, 2, number, arrayNumber, arrayNumber);
    if (tokenMatches(name, "len")) return typeFunctionN(tc, 1, number, arrayNumber);
    if (tokenMatches(name, "norm")) return typeFunctionN(tc, 1, arrayNumber, arrayNumber);
    if (tokenMatches(name, "lerp")) return typeFunctionN(tc, 3, arrayNumber, arrayNumber, arrayNumber, number);
    if (tokenMatches(name, "dist")) return typeFunctionN(tc, 2, number, arrayNumber, arrayNumber);
  }

  if (typeNamedIs(objectType, "http")) {
    Type* mapAny = typeMap(tc, string, any);
    if (tokenMatches(name, "get")) return typeFunctionN(tc, 1, mapAny, string);
    if (tokenMatches(name, "post")) return typeFunctionN(tc, 2, mapAny, string, string);
    if (tokenMatches(name, "request")) return typeFunctionN(tc, 3, mapAny, string, string, any);
    if (tokenMatches(name, "serve")) return typeFunctionN(tc, -1, typeNull());
  }

  if (typeNamedIs(objectType, "proc")) {
    if (tokenMatches(name, "run")) return typeFunctionN(tc, 1, number, string);
  }

  if (typeNamedIs(objectType, "env")) {
    Type* arrayString = typeArray(tc, string);
    Type* mapString = typeMap(tc, string, string);
    if (tokenMatches(name, "args")) return typeFunctionN(tc, 0, arrayString);
    if (tokenMatches(name, "get")) return typeFunctionN(tc, 1, typeMakeNullable(tc, string), string);
    if (tokenMatches(name, "set")) return typeFunctionN(tc, 2, boolean, string, string);
    if (tokenMatches(name, "has")) return typeFunctionN(tc, 1, boolean, string);
    if (tokenMatches(name, "unset")) return typeFunctionN(tc, 1, boolean, string);
    if (tokenMatches(name, "all")) return typeFunctionN(tc, 0, mapString);
  }

  if (typeNamedIs(objectType, "plugin")) {
    if (tokenMatches(name, "load")) return typeFunctionN(tc, 1, boolean, string);
  }

  return typeAny();
}

static void typeDefineStdlib(Compiler* c) {
  if (!typecheckEnabled(c)) return;
  TypeChecker* tc = c->typecheck;

  typeDefineSynthetic(c, "print", typeFunctionN(tc, -1, typeNull()));
  typeDefineSynthetic(c, "clock", typeFunctionN(tc, 0, typeNumber()));
  typeDefineSynthetic(c, "type", typeFunctionN(tc, 1, typeString(), typeAny()));
  typeDefineSynthetic(c, "len", typeFunctionN(tc, 1, typeNumber(), typeAny()));
  typeDefineSynthetic(c, "args", typeFunctionN(tc, 0, typeArray(tc, typeString())));
  {
    Type* params[2] = { typeArray(tc, typeAny()), typeAny() };
    typeDefineSynthetic(c, "push", typeFunction(tc, params, 2, typeNumber()));
  }
  typeDefineSynthetic(c, "keys", typeFunctionN(tc, 1, typeArray(tc, typeString()),
                                               typeMap(tc, typeString(), typeAny())));
  typeDefineSynthetic(c, "values", typeFunctionN(tc, 1, typeArray(tc, typeAny()),
                                                 typeMap(tc, typeString(), typeAny())));

  typeDefineSynthetic(c, "fs", typeNamed(tc, copyString(c->vm, "fs")));
  typeDefineSynthetic(c, "path", typeNamed(tc, copyString(c->vm, "path")));
  typeDefineSynthetic(c, "json", typeNamed(tc, copyString(c->vm, "json")));
  typeDefineSynthetic(c, "yaml", typeNamed(tc, copyString(c->vm, "yaml")));
  typeDefineSynthetic(c, "math", typeNamed(tc, copyString(c->vm, "math")));
  typeDefineSynthetic(c, "random", typeNamed(tc, copyString(c->vm, "random")));
  typeDefineSynthetic(c, "str", typeNamed(tc, copyString(c->vm, "str")));
  typeDefineSynthetic(c, "array", typeNamed(tc, copyString(c->vm, "array")));
  typeDefineSynthetic(c, "os", typeNamed(tc, copyString(c->vm, "os")));
    typeDefineSynthetic(c, "time", typeNamed(tc, copyString(c->vm, "time")));
    typeDefineSynthetic(c, "vec2", typeNamed(tc, copyString(c->vm, "vec2")));
    typeDefineSynthetic(c, "vec3", typeNamed(tc, copyString(c->vm, "vec3")));
    typeDefineSynthetic(c, "vec4", typeNamed(tc, copyString(c->vm, "vec4")));
    typeDefineSynthetic(c, "http", typeNamed(tc, copyString(c->vm, "http")));
    typeDefineSynthetic(c, "proc", typeNamed(tc, copyString(c->vm, "proc")));
    typeDefineSynthetic(c, "env", typeNamed(tc, copyString(c->vm, "env")));
    typeDefineSynthetic(c, "plugin", typeNamed(tc, copyString(c->vm, "plugin")));
    typeDefineSynthetic(c, "di", typeNamed(tc, copyString(c->vm, "di")));
  }

  static Type* typeNamed(TypeChecker* tc, ObjString* name) {
    Type* type = typeAlloc(tc, TYPE_NAMED);
    type->name = name;
    return type;
  }

  static Type* typeGeneric(TypeChecker* tc, ObjString* name) {
    Type* type = typeAlloc(tc, TYPE_GENERIC);
    type->name = name;
    return type;
  }

static Type* typeArray(TypeChecker* tc, Type* elem) {
  Type* type = typeAlloc(tc, TYPE_ARRAY);
  type->elem = elem ? elem : typeAny();
  return type;
}

static Type* typeMap(TypeChecker* tc, Type* key, Type* value) {
  Type* type = typeAlloc(tc, TYPE_MAP);
  type->key = key ? key : typeString();
  type->value = value ? value : typeAny();
  return type;
}

static Type* typeFunction(TypeChecker* tc, Type** params, int paramCount, Type* returnType) {
  Type* type = typeAlloc(tc, TYPE_FUNCTION);
  type->paramCount = paramCount;
  type->returnType = returnType ? returnType : typeAny();
  if (paramCount > 0) {
    type->params = (Type**)malloc(sizeof(Type*) * (size_t)paramCount);
    if (!type->params) {
      fprintf(stderr, "Out of memory.\n");
      exit(1);
    }
    for (int i = 0; i < paramCount; i++) {
      type->params[i] = params[i] ? params[i] : typeAny();
    }
  }
  return type;
}

static Type* parseType(Compiler* c);
static Type* typeLookupStdlibMember(Compiler* c, Type* objectType, Token name);
static void typeDefineStdlib(Compiler* c);

static TypeParam* parseTypeParams(Compiler* c, int* outCount) {
  if (outCount) *outCount = 0;
  if (!match(c, TOKEN_LESS)) return NULL;
  int count = 0;
  int capacity = 0;
  TypeParam* params = NULL;
  do {
    Token name = consume(c, TOKEN_IDENTIFIER, "Expect type parameter name.");
    ObjString* nameStr = stringFromToken(c->vm, name);
    ObjString* constraint = NULL;
    if (match(c, TOKEN_COLON)) {
      Token constraintName = consume(c, TOKEN_IDENTIFIER, "Expect interface name after ':'.");
      constraint = stringFromToken(c->vm, constraintName);
    }
    if (count >= capacity) {
      int oldCap = capacity;
      capacity = GROW_CAPACITY(oldCap);
      params = GROW_ARRAY(TypeParam, params, oldCap, capacity);
      if (!params) {
        fprintf(stderr, "Out of memory.\n");
        exit(1);
      }
    }
    params[count].name = nameStr;
    params[count].constraint = constraint;
    count++;
  } while (match(c, TOKEN_COMMA));
  consume(c, TOKEN_GREATER, "Expect '>' after type parameters.");
  if (outCount) *outCount = count;
  return params;
}

static Type* typeFromToken(Compiler* c, Token token) {
  if (tokenMatches(token, "number")) return typeNumber();
  if (tokenMatches(token, "string")) return typeString();
  if (tokenMatches(token, "bool") || tokenMatches(token, "boolean")) return typeBool();
  if (tokenMatches(token, "null") || tokenMatches(token, "void")) return typeNull();
  if (tokenMatches(token, "any")) return typeAny();
  if (tokenMatches(token, "array")) return typeArray(c->typecheck, typeAny());
  if (tokenMatches(token, "map")) return typeMap(c->typecheck, typeString(), typeAny());
  ObjString* name = stringFromToken(c->vm, token);
  return typeNamed(c->typecheck, name);
}

  static Type* parseTypeArguments(Compiler* c, Type* base, Token typeToken) {
    if (!match(c, TOKEN_LESS)) {
      return base;
    }

    if (base->kind == TYPE_ARRAY) {
      Type* elem = parseType(c);
      consume(c, TOKEN_GREATER, "Expect '>' after array type.");
      return typeArray(c->typecheck, elem);
    }

    if (base->kind == TYPE_MAP) {
      Type* key = parseType(c);
      Type* value = NULL;
      if (match(c, TOKEN_COMMA)) {
        value = parseType(c);
      } else {
        value = key;
        key = typeString();
      }
      if (!typeIsAny(key) && key->kind != TYPE_STRING) {
        typeErrorAt(c, typeToken, "Map keys must be string.");
        key = typeString();
      }
      consume(c, TOKEN_GREATER, "Expect '>' after map type.");
      return typeMap(c->typecheck, key, value);
    }

    if (base->kind == TYPE_NAMED) {
      int count = 0;
      int capacity = 0;
      Type** args = NULL;
      if (!check(c, TOKEN_GREATER)) {
        do {
          Type* arg = parseType(c);
          if (count >= capacity) {
            int oldCap = capacity;
            capacity = GROW_CAPACITY(oldCap);
            args = GROW_ARRAY(Type*, args, oldCap, capacity);
            if (!args) {
              fprintf(stderr, "Out of memory.\n");
              exit(1);
            }
          }
          args[count++] = arg;
        } while (match(c, TOKEN_COMMA));
      }
      consume(c, TOKEN_GREATER, "Expect '>' after type arguments.");
      base->typeArgs = args;
      base->typeArgCount = count;
      return base;
    }

    typeErrorAt(c, typeToken, "Only array/map/named types accept type arguments.");
    int depth = 1;
    while (!isAtEnd(c) && depth > 0) {
      if (match(c, TOKEN_LESS)) depth++;
      else if (match(c, TOKEN_GREATER)) depth--;
      else advance(c);
    }
    return base;
  }

  static Type* parseType(Compiler* c) {
    if (!check(c, TOKEN_IDENTIFIER) && !check(c, TOKEN_NULL)) {
      errorAtCurrent(c, "Expect type name.");
      return typeAny();
    }
    Token name = advance(c);
    TypeParam* param = typeParamFindToken(c->typecheck, name);
    Type* base = param ? typeGeneric(c->typecheck, param->name) : typeFromToken(c, name);
    if (check(c, TOKEN_LESS)) {
      base = parseTypeArguments(c, base, name);
    }
  if (match(c, TOKEN_QUESTION)) {
    base = typeMakeNullable(c->typecheck, base);
  }
  return base;
}

static Type* typeMerge(TypeChecker* tc, Type* current, Type* next) {
  if (!current) return next;
  if (!next) return current;
  if (current->kind == TYPE_UNKNOWN) return next;
  if (next->kind == TYPE_UNKNOWN) return current;
  if (typeEquals(current, next)) return current;
  if (current->kind == TYPE_NULL) {
    return typeMakeNullable(tc, next);
  }
  if (next->kind == TYPE_NULL) {
    return typeMakeNullable(tc, current);
  }
  if (current->kind == next->kind) {
    if (typeEquals(current, next)) return current;
    switch (current->kind) {
      case TYPE_NUMBER:
      case TYPE_STRING:
      case TYPE_BOOL:
        return typeMakeNullable(tc, current);
        case TYPE_NAMED:
          if (typeNamesEqual(current->name, next->name)) {
            return typeMakeNullable(tc, current);
          }
          break;
        case TYPE_GENERIC:
          if (typeNamesEqual(current->name, next->name)) {
            return typeMakeNullable(tc, current);
          }
          break;
      case TYPE_ARRAY:
        if (typeEquals(current->elem, next->elem)) {
          return typeMakeNullable(tc, current);
        }
        break;
      case TYPE_MAP:
        if (typeEquals(current->key, next->key) &&
            typeEquals(current->value, next->value)) {
          return typeMakeNullable(tc, current);
        }
        break;
      case TYPE_FUNCTION:
        if (typeEquals(current, next)) return current;
        break;
      case TYPE_ANY:
      case TYPE_UNKNOWN:
      case TYPE_NULL:
        break;
    }
  }
  if (typeIsAny(current) || typeIsAny(next)) return typeAny();
  return typeAny();
}

static bool typeEnsureNonNull(Compiler* c, Token token, Type* type, const char* message) {
  if (!typecheckEnabled(c)) return true;
  if (typeIsAny(type)) return true;
  if (typeIsNullable(type)) {
    typeErrorAt(c, token, "%s", message);
    return false;
  }
  return true;
}

static Type* typeUnaryResult(Compiler* c, Token op, Type* right) {
  if (op.type == TOKEN_MINUS) {
    typeEnsureNonNull(c, op, right, "Unary '-' expects a non-null number.");
    if (!typeIsAny(right) && right->kind != TYPE_NUMBER) {
      typeErrorAt(c, op, "Unary '-' expects a number.");
    }
    return typeNumber();
  }
  if (op.type == TOKEN_BANG) {
    return typeBool();
  }
  return typeAny();
}

static Type* typeBinaryResult(Compiler* c, Token op, Type* left, Type* right) {
  switch (op.type) {
    case TOKEN_PLUS:
      typeEnsureNonNull(c, op, left, "Operator '+' expects non-null operands.");
      typeEnsureNonNull(c, op, right, "Operator '+' expects non-null operands.");
      if (left->kind == TYPE_NUMBER && right->kind == TYPE_NUMBER) return typeNumber();
      if (left->kind == TYPE_STRING && right->kind == TYPE_STRING) return typeString();
      if (typeIsAny(left) || typeIsAny(right)) return typeAny();
      typeErrorAt(c, op, "Operator '+' expects two numbers or two strings.");
      return typeAny();
    case TOKEN_MINUS:
    case TOKEN_STAR:
    case TOKEN_SLASH:
      typeEnsureNonNull(c, op, left, "Operator expects non-null numbers.");
      typeEnsureNonNull(c, op, right, "Operator expects non-null numbers.");
      if (!typeIsAny(left) && left->kind != TYPE_NUMBER) {
        typeErrorAt(c, op, "Operator expects numbers.");
      }
      if (!typeIsAny(right) && right->kind != TYPE_NUMBER) {
        typeErrorAt(c, op, "Operator expects numbers.");
      }
      return typeNumber();
    case TOKEN_GREATER:
    case TOKEN_GREATER_EQUAL:
    case TOKEN_LESS:
    case TOKEN_LESS_EQUAL:
      typeEnsureNonNull(c, op, left, "Comparison expects non-null numbers.");
      typeEnsureNonNull(c, op, right, "Comparison expects non-null numbers.");
      if (!typeIsAny(left) && left->kind != TYPE_NUMBER) {
        typeErrorAt(c, op, "Comparison expects numbers.");
      }
      if (!typeIsAny(right) && right->kind != TYPE_NUMBER) {
        typeErrorAt(c, op, "Comparison expects numbers.");
      }
      return typeBool();
    case TOKEN_BANG_EQUAL:
    case TOKEN_EQUAL_EQUAL:
      return typeBool();
    default:
      return typeAny();
  }
}

static Type* typeLogicalResult(Type* left, Type* right) {
  if (typeIsAny(left) || typeIsAny(right)) return typeAny();
  if (typeEquals(left, right)) return left;
  return typeAny();
}

static Type* typeIndexResult(Compiler* c, Token op, Type* objectType, Type* indexType) {
  if (typeIsAny(objectType)) return typeAny();
  if (objectType->kind == TYPE_NULL) return typeNull();
  if (objectType->kind == TYPE_ARRAY) {
    if (!typeIsAny(indexType) && indexType->kind != TYPE_NUMBER) {
      typeErrorAt(c, op, "Array index expects a number.");
    }
    return objectType->elem ? objectType->elem : typeAny();
  }
  if (objectType->kind == TYPE_MAP) {
    if (!typeIsAny(indexType) && indexType->kind != TYPE_STRING) {
      typeErrorAt(c, op, "Map index expects a string.");
    }
    return objectType->value ? objectType->value : typeAny();
  }
  return typeAny();
}

static void typeCheckIndexAssign(Compiler* c, Token op, Type* objectType, Type* indexType,
                                 Type* valueType) {
  if (typeIsAny(objectType)) return;
  if (!typeEnsureNonNull(c, op, objectType,
                         "Cannot index nullable value. Use '?.['.")) {
    return;
  }
  if (objectType->kind == TYPE_ARRAY) {
    if (!typeIsAny(indexType) && indexType->kind != TYPE_NUMBER) {
      typeErrorAt(c, op, "Array index expects a number.");
    }
    if (objectType->elem && !typeAssignable(objectType->elem, valueType)) {
      char expected[64];
      char got[64];
      typeToString(objectType->elem, expected, sizeof(expected));
      typeToString(valueType, got, sizeof(got));
      typeErrorAt(c, op, "Array element expects %s but got %s.", expected, got);
    }
    return;
  }
  if (objectType->kind == TYPE_MAP) {
    if (!typeIsAny(indexType) && indexType->kind != TYPE_STRING) {
      typeErrorAt(c, op, "Map index expects a string.");
    }
    if (objectType->value && !typeAssignable(objectType->value, valueType)) {
      char expected[64];
      char got[64];
      typeToString(objectType->value, expected, sizeof(expected));
      typeToString(valueType, got, sizeof(got));
      typeErrorAt(c, op, "Map value expects %s but got %s.", expected, got);
    }
  }
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
  typePush(c, typeNumber());
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
  typePush(c, typeString());
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
    typePop(c);
    consumeClosing(c, TOKEN_INTERP_END, "Expect '}' after interpolation.", interpStart);
    emitByte(c, OP_STRINGIFY, segment);
    emitByte(c, OP_ADD, segment);

    Token tail = consume(c, TOKEN_STRING_SEGMENT, "Expect string segment after interpolation.");
    char* tailValue = parseStringSegment(tail);
    ObjString* tailStr = takeStringWithLength(c->vm, tailValue, (int)strlen(tailValue));
    emitConstant(c, OBJ_VAL(tailStr), tail);
    emitByte(c, OP_ADD, tail);
  }
  typePush(c, typeString());
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
  if (token.type == TOKEN_FALSE || token.type == TOKEN_TRUE) {
    typePush(c, typeBool());
  } else if (token.type == TOKEN_NULL) {
    typePush(c, typeNull());
  }
}

static void variable(Compiler* c, bool canAssign) {
  Token name = previous(c);
  int nameIdx = emitStringConstant(c, name);
  if (canAssign && match(c, TOKEN_EQUAL)) {
    expression(c);
    Type* valueType = typePop(c);
    typeAssign(c, name, valueType);
    typePush(c, valueType);
    emitByte(c, OP_SET_VAR, name);
    emitShort(c, (uint16_t)nameIdx, name);
  } else {
    emitByte(c, OP_GET_VAR, name);
    emitShort(c, (uint16_t)nameIdx, name);
    typePush(c, typeLookup(c, name));
  }
}

static void thisExpr(Compiler* c, bool canAssign) {
  (void)canAssign;
  Token token = previous(c);
  int name = emitStringConstant(c, token);
  emitByte(c, OP_GET_THIS, token);
  emitShort(c, (uint16_t)name, token);
  typePush(c, typeAny());
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
  Type* right = typePop(c);
  typePush(c, typeUnaryResult(c, op, right));
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
  Type* right = typePop(c);
  Type* left = typePop(c);
  typePush(c, typeBinaryResult(c, op, left, right));
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
  Type* right = typePop(c);
  Type* left = typePop(c);
  typePush(c, typeLogicalResult(left, right));
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
  Type* right = typePop(c);
  Type* left = typePop(c);
  typePush(c, typeLogicalResult(left, right));
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
    if (typecheckEnabled(c)) {
      Type* argTypes[ERK_MAX_ARGS];
      for (int i = argc - 1; i >= 0; i--) {
        argTypes[i] = typePop(c);
      }
      Type* callee = typePop(c);
      Type* result = typeAny();
      if (callee && callee->kind == TYPE_FUNCTION) {
        int bindingCount = callee->typeParamCount;
        TypeBinding* bindings = NULL;
        if (bindingCount > 0 && callee->typeParams) {
          bindings = (TypeBinding*)malloc(sizeof(TypeBinding) * (size_t)bindingCount);
          if (!bindings) {
            fprintf(stderr, "Out of memory.\n");
            exit(1);
          }
          for (int i = 0; i < bindingCount; i++) {
            bindings[i].name = callee->typeParams[i].name;
            bindings[i].constraint = callee->typeParams[i].constraint;
            bindings[i].bound = NULL;
          }
        }
        if (callee->paramCount >= 0 && callee->paramCount != argc) {
          typeErrorAt(c, paren, "Function expects %d arguments but got %d.",
                      callee->paramCount, argc);
        } else if (callee->params) {
          int checkCount = callee->paramCount >= 0 ? callee->paramCount : argc;
          for (int i = 0; i < checkCount && i < argc; i++) {
            bool ok = true;
            if (bindings) {
              ok = typeUnify(c, callee->params[i], argTypes[i], bindings, bindingCount, paren);
            } else {
              ok = typeAssignable(callee->params[i], argTypes[i]);
            }
            if (!ok) {
              if (bindings && callee->params[i]->kind == TYPE_GENERIC) {
                continue;
              }
              char expected[64];
              char got[64];
              typeToString(callee->params[i], expected, sizeof(expected));
              typeToString(argTypes[i], got, sizeof(got));
              typeErrorAt(c, paren, "Argument %d expects %s but got %s.",
                          i + 1, expected, got);
            }
          }
        }
        result = callee->returnType ? callee->returnType : typeAny();
        if (bindings) {
          result = typeSubstitute(c->typecheck, result, bindings, bindingCount);
          free(bindings);
        }
      }
      if (!optionalCall) {
        typeEnsureNonNull(c, paren, callee, "Cannot call nullable value. Use '?.'.");
      } else if (typeIsNullable(callee)) {
      result = typeMakeNullable(c->typecheck, result);
    }
    typePush(c, result);
  }
  emitByte(c, optionalCall ? OP_CALL_OPTIONAL : OP_CALL, paren);
  emitByte(c, (uint8_t)argc, paren);
}

static void dot(Compiler* c, bool canAssign) {
  c->pendingOptionalCall = false;
  Token name = consume(c, TOKEN_IDENTIFIER, "Expect property name after '.'.");
  int nameIdx = emitStringConstant(c, name);
  Type* objectType = typePop(c);
  typeEnsureNonNull(c, name, objectType,
                    "Cannot access property on nullable value. Use '?.'.");
  if (canAssign && match(c, TOKEN_EQUAL)) {
    expression(c);
    Type* valueType = typePop(c);
    typePush(c, valueType);
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
    if (typecheckEnabled(c)) {
      Type* argTypes[ERK_MAX_ARGS];
      for (int i = argc - 1; i >= 0; i--) {
        argTypes[i] = typePop(c);
      }
      Type* memberType = typeLookupStdlibMember(c, objectType, name);
      Type* resultType = typeAny();
      if (memberType && memberType->kind == TYPE_FUNCTION) {
        if (memberType->paramCount >= 0 && memberType->paramCount != argc) {
          typeErrorAt(c, paren, "Function expects %d arguments but got %d.",
                      memberType->paramCount, argc);
        } else if (memberType->params) {
          int checkCount = memberType->paramCount >= 0 ? memberType->paramCount : argc;
          for (int i = 0; i < checkCount && i < argc; i++) {
            if (!typeAssignable(memberType->params[i], argTypes[i])) {
              char expected[64];
              char got[64];
              typeToString(memberType->params[i], expected, sizeof(expected));
              typeToString(argTypes[i], got, sizeof(got));
              typeErrorAt(c, paren, "Argument %d expects %s but got %s.",
                          i + 1, expected, got);
            }
          }
        }
        resultType = memberType->returnType ? memberType->returnType : typeAny();
      } else if (memberType && !typeIsAny(memberType)) {
        typeErrorAt(c, paren, "Property is not callable.");
      }
      typePush(c, resultType);
    }
    emitByte(c, OP_INVOKE, paren);
    emitShort(c, (uint16_t)nameIdx, name);
    emitByte(c, (uint8_t)argc, paren);
  } else {
    Type* memberType = typeAny();
    if (typecheckEnabled(c)) {
      memberType = typeLookupStdlibMember(c, objectType, name);
    }
    typePush(c, memberType);
    emitByte(c, OP_GET_PROPERTY, name);
    emitShort(c, (uint16_t)nameIdx, name);
  }
  (void)objectType;
}

static void optionalDot(Compiler* c, bool canAssign) {
  (void)canAssign;
  if (check(c, TOKEN_LEFT_PAREN)) {
    c->pendingOptionalCall = true;
    return;
  }
  if (match(c, TOKEN_LEFT_BRACKET)) {
    Token bracket = previous(c);
    expression(c);
    Type* indexType = typePop(c);
    Type* objectType = typePop(c);
    consumeClosing(c, TOKEN_RIGHT_BRACKET, "Expect ']' after index.", bracket);
    Type* result = typeIndexResult(c, bracket, objectType, indexType);
    typePush(c, typeMakeNullable(c->typecheck, result));
    emitByte(c, OP_GET_INDEX_OPTIONAL, bracket);
    c->pendingOptionalCall = true;
    return;
  }
  Token name = consume(c, TOKEN_IDENTIFIER, "Expect property name after '?.'.");
  int nameIdx = emitStringConstant(c, name);
  Type* objectType = typePop(c);
  Type* memberType = typeAny();
  if (typecheckEnabled(c)) {
    memberType = typeLookupStdlibMember(c, objectType, name);
  }
  typePush(c, typeMakeNullable(c->typecheck, memberType));
  emitByte(c, OP_GET_PROPERTY_OPTIONAL, name);
  emitShort(c, (uint16_t)nameIdx, name);
  c->pendingOptionalCall = true;
}

static void index_(Compiler* c, bool canAssign) {
  c->pendingOptionalCall = false;
  Token bracket = previous(c);
  expression(c);
  Type* indexType = typePop(c);
  Type* objectType = typePop(c);
  typeEnsureNonNull(c, bracket, objectType,
                    "Cannot index nullable value. Use '?.['.");
  consumeClosing(c, TOKEN_RIGHT_BRACKET, "Expect ']' after index.", bracket);
  if (canAssign && match(c, TOKEN_EQUAL)) {
    expression(c);
    Type* valueType = typePop(c);
    typeCheckIndexAssign(c, bracket, objectType, indexType, valueType);
    typePush(c, valueType);
    emitByte(c, OP_SET_INDEX, bracket);
  } else {
    typePush(c, typeIndexResult(c, bracket, objectType, indexType));
    emitByte(c, OP_GET_INDEX, bracket);
  }
}

static void array(Compiler* c, bool canAssign) {
  (void)canAssign;
  Token open = previous(c);
  int count = 0;
  Type* elementType = NULL;
  emitByte(c, OP_ARRAY, noToken());
  emitShort(c, 0, noToken());
  int sizeOffset = c->chunk->count - 2;
  if (!check(c, TOKEN_RIGHT_BRACKET)) {
    do {
      expression(c);
      Type* itemType = typePop(c);
      elementType = typeMerge(c->typecheck, elementType, itemType);
      emitByte(c, OP_ARRAY_APPEND, noToken());
      count++;
    } while (match(c, TOKEN_COMMA));
  }
  consumeClosing(c, TOKEN_RIGHT_BRACKET, "Expect ']' after array literal.", open);
  c->chunk->code[sizeOffset] = (uint8_t)((count >> 8) & 0xff);
  c->chunk->code[sizeOffset + 1] = (uint8_t)(count & 0xff);
  if (typecheckEnabled(c)) {
    if (!elementType) elementType = typeAny();
    typePush(c, typeArray(c->typecheck, elementType));
  }
}

static void map(Compiler* c, bool canAssign) {
  (void)canAssign;
  Token open = previous(c);
  int count = 0;
  Type* valueType = NULL;
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
      Type* entryType = typePop(c);
      valueType = typeMerge(c->typecheck, valueType, entryType);
      emitByte(c, OP_MAP_SET, noToken());
      count++;
    } while (match(c, TOKEN_COMMA));
  }
  consumeClosing(c, TOKEN_RIGHT_BRACE, "Expect '}' after map literal.", open);
  c->chunk->code[sizeOffset] = (uint8_t)((count >> 8) & 0xff);
  c->chunk->code[sizeOffset + 1] = (uint8_t)(count & 0xff);
  if (typecheckEnabled(c)) {
    if (!valueType) valueType = typeAny();
    typePush(c, typeMap(c->typecheck, typeString(), valueType));
  }
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
  typePop(c);
  consume(c, TOKEN_SEMICOLON, "Expect ';' after expression.");
  emitByte(c, OP_POP, noToken());
  emitGc(c);
}

  static void varDeclaration(Compiler* c, bool isConst, bool isExport) {
  Token name = consume(c, TOKEN_IDENTIFIER, "Expect variable name.");
  Type* declaredType = NULL;
  bool hasType = false;
  if (match(c, TOKEN_COLON)) {
    declaredType = parseType(c);
    hasType = true;
  }
  bool hasInitializer = match(c, TOKEN_EQUAL);
  Type* valueType = typeUnknown();
  if (hasInitializer) {
    expression(c);
    valueType = typePop(c);
  } else {
    if (isConst) {
      errorAt(c, name, "Const declarations require an initializer.");
    }
    emitByte(c, OP_NULL, noToken());
    valueType = typeNull();
  }
  consume(c, TOKEN_SEMICOLON, "Expect ';' after variable declaration.");
  int nameIdx = emitStringConstant(c, name);
  emitByte(c, isConst ? OP_DEFINE_CONST : OP_DEFINE_VAR, name);
  emitShort(c, (uint16_t)nameIdx, name);
  if (typecheckEnabled(c)) {
    if (hasType) {
      if (hasInitializer && !typeAssignable(declaredType, valueType)) {
        char expected[64];
        char got[64];
        typeToString(declaredType, expected, sizeof(expected));
        typeToString(valueType, got, sizeof(got));
        typeErrorAt(c, name, "Type mismatch. Expected %s but got %s.", expected, got);
      }
      typeDefine(c, name, declaredType, true);
    } else {
      Type* inferred = hasInitializer ? valueType : typeUnknown();
      typeDefine(c, name, inferred, false);
    }
  }
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
  typeCheckerEnterScope(c);
  block(c, open);
  emitByte(c, OP_END_SCOPE, noToken());
  c->scopeDepth--;
  typeCheckerExitScope(c);
  emitGc(c);
}

static void ifStatement(Compiler* c) {
  Token keyword = previous(c);
  Token openParen = consume(c, TOKEN_LEFT_PAREN, "Expect '(' after 'if'.");
  expression(c);
  typePop(c);
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
  typePop(c);
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
  typeCheckerEnterScope(c);
  Token openParen = consume(c, TOKEN_LEFT_PAREN, "Expect '(' after 'for'.");

  if (match(c, TOKEN_SEMICOLON)) {
  } else if (match(c, TOKEN_LET)) {
    varDeclaration(c, false, false);
  } else if (match(c, TOKEN_CONST)) {
    varDeclaration(c, true, false);
  } else {
    expression(c);
    typePop(c);
    consume(c, TOKEN_SEMICOLON, "Expect ';' after loop initializer.");
    emitByte(c, OP_POP, noToken());
  }

  int loopStart = c->chunk->count;
  int exitJump = -1;
  if (!check(c, TOKEN_SEMICOLON)) {
    expression(c);
    typePop(c);
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
    typePop(c);
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
  typeCheckerExitScope(c);
  emitGc(c);
}

static void foreachStatement(Compiler* c) {
  Token keyword = previous(c);
  emitByte(c, OP_BEGIN_SCOPE, noToken());
  c->scopeDepth++;
  typeCheckerEnterScope(c);
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
  Type* iterType = typePop(c);
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

  if (typecheckEnabled(c)) {
    Type* keyType = typeAny();
    Type* valueType = typeAny();
    if (iterType && iterType->kind == TYPE_ARRAY) {
      keyType = typeNumber();
      valueType = iterType->elem ? iterType->elem : typeAny();
    } else if (iterType && iterType->kind == TYPE_MAP) {
      keyType = iterType->key ? iterType->key : typeString();
      valueType = iterType->value ? iterType->value : typeAny();
    }
    if (hasKey) {
      typeDefine(c, keyToken, keyType, true);
    }
    typeDefine(c, valueToken, valueType, true);
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
  typeCheckerExitScope(c);
  emitGc(c);
}

static void switchStatement(Compiler* c) {
  Token keyword = previous(c);
  const char* keywordName = keyword.type == TOKEN_MATCH ? "match" : "switch";
  char message[64];
  emitByte(c, OP_BEGIN_SCOPE, noToken());
  c->scopeDepth++;
  typeCheckerEnterScope(c);
  snprintf(message, sizeof(message), "Expect '(' after '%s'.", keywordName);
  Token openParen = consume(c, TOKEN_LEFT_PAREN, message);
  expression(c);
  Type* switchType = typePop(c);
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
      Type* caseType = typePop(c);
      if (switchType && !typeIsAny(switchType) && !typeAssignable(switchType, caseType)) {
        char expected[64];
        char got[64];
        typeToString(switchType, expected, sizeof(expected));
        typeToString(caseType, got, sizeof(got));
        typeErrorAt(c, previous(c), "Case type %s does not match %s.", got, expected);
      }
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
  typeCheckerExitScope(c);
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
    Type* valueType = typePop(c);
    if (typecheckEnabled(c) && c->typecheck->currentReturn) {
      if (!typeAssignable(c->typecheck->currentReturn, valueType)) {
        char expected[64];
        char got[64];
        typeToString(c->typecheck->currentReturn, expected, sizeof(expected));
        typeToString(valueType, got, sizeof(got));
        typeErrorAt(c, keyword, "Return type mismatch. Expected %s but got %s.", expected, got);
      }
    }
  } else {
    if (typecheckEnabled(c) && c->typecheck->currentReturn &&
        c->typecheck->currentReturn->kind != TYPE_NULL &&
        c->typecheck->currentReturn->kind != TYPE_ANY &&
        c->typecheck->currentReturn->kind != TYPE_UNKNOWN) {
      char expected[64];
      typeToString(c->typecheck->currentReturn, expected, sizeof(expected));
      typeErrorAt(c, keyword, "Return type mismatch. Expected %s but got null.", expected);
    }
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
    typePop(c);
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
    typePop(c);
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
  typePop(c);
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
  typePop(c);
  consume(c, TOKEN_IMPORT, "Expect 'import' after module path.");
  Token alias = consume(c, TOKEN_IDENTIFIER, "Expect name after 'import'.");
  consume(c, TOKEN_SEMICOLON, "Expect ';' after import.");
  emitByte(c, OP_IMPORT, keyword);
  emitByte(c, 1, keyword);
  uint16_t aliasIdx = (uint16_t)emitStringConstant(c, alias);
  emitShort(c, aliasIdx, keyword);
  emitGc(c);
}

static ObjFunction* compileFunction(Compiler* c, Token name, bool isInitializer,
                                    Type** outType, bool defineType);

  static void functionDeclaration(Compiler* c, bool isExport) {
    Token name = consume(c, TOKEN_IDENTIFIER, "Expect function name.");
    Type* functionType = NULL;
    ObjFunction* function = compileFunction(c, name, false, &functionType, true);
    (void)functionType;
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

  static void interfaceDeclaration(Compiler* c) {
    Token nameToken = consume(c, TOKEN_IDENTIFIER, "Expect interface name.");
    ObjString* nameStr = stringFromToken(c->vm, nameToken);

    int typeParamCount = 0;
    TypeParam* typeParams = parseTypeParams(c, &typeParamCount);

    Token openBrace = consume(c, TOKEN_LEFT_BRACE, "Expect '{' before interface body.");

    int savedParamCount = 0;
    if (typecheckEnabled(c)) {
      savedParamCount = c->typecheck->typeParamCount;
      typeParamsPushList(c->typecheck, typeParams, typeParamCount);
    }

    InterfaceDef def;
    memset(&def, 0, sizeof(def));
    def.name = nameStr;
    def.typeParams = typeParams;
    def.typeParamCount = typeParamCount;

    while (!check(c, TOKEN_RIGHT_BRACE) && !isAtEnd(c)) {
      if (!match(c, TOKEN_FUN)) {
        errorAtCurrent(c, "Expect 'fun' in interface body.");
        synchronize(c);
        break;
      }
      Token methodName = consume(c, TOKEN_IDENTIFIER, "Expect method name.");
      Token openParen = consume(c, TOKEN_LEFT_PAREN, "Expect '(' after method name.");

      Type** paramTypes = NULL;
      int paramCount = 0;
      int paramCap = 0;
      if (!check(c, TOKEN_RIGHT_PAREN)) {
        do {
          Token paramName = consume(c, TOKEN_IDENTIFIER, "Expect parameter name.");
          Type* paramType = typeAny();
          if (match(c, TOKEN_COLON)) {
            paramType = parseType(c);
          }
          if (paramCount >= paramCap) {
            int oldCap = paramCap;
            paramCap = GROW_CAPACITY(oldCap);
            paramTypes = GROW_ARRAY(Type*, paramTypes, oldCap, paramCap);
            if (!paramTypes) {
              fprintf(stderr, "Out of memory.\n");
              exit(1);
            }
          }
          paramTypes[paramCount++] = paramType;
          (void)paramName;
        } while (match(c, TOKEN_COMMA));
      }
      consumeClosing(c, TOKEN_RIGHT_PAREN, "Expect ')' after parameters.", openParen);

      Type* returnType = typeAny();
      if (match(c, TOKEN_COLON)) {
        returnType = parseType(c);
      }
      consume(c, TOKEN_SEMICOLON, "Expect ';' after interface method.");

      if (typecheckEnabled(c)) {
        if (def.methodCount >= def.methodCapacity) {
          int oldCap = def.methodCapacity;
          def.methodCapacity = GROW_CAPACITY(oldCap);
          def.methods = GROW_ARRAY(InterfaceMethod, def.methods, oldCap, def.methodCapacity);
          if (!def.methods) {
            fprintf(stderr, "Out of memory.\n");
            exit(1);
          }
        }
        Type* methodType = typeFunction(c->typecheck, paramTypes, paramCount, returnType);
        def.methods[def.methodCount].name = stringFromToken(c->vm, methodName);
        def.methods[def.methodCount].type = methodType;
        def.methodCount++;
      }
      free(paramTypes);
    }
    consumeClosing(c, TOKEN_RIGHT_BRACE, "Expect '}' after interface body.", openBrace);

    if (typecheckEnabled(c)) {
      InterfaceDef* existing = typeRegistryFindInterface(gTypeRegistry, nameStr);
      if (existing) {
        typeErrorAt(c, nameToken, "Interface '%s' already defined.", nameStr->chars);
        if (def.methods) free(def.methods);
        if (def.typeParams) free(def.typeParams);
      } else {
        typeRegistryAddInterface(gTypeRegistry, &def);
      }
      typeParamsTruncate(c->typecheck, savedParamCount);
    } else {
      if (def.methods) free(def.methods);
      if (def.typeParams) free(def.typeParams);
    }
  }

  static ClassMethod* findClassMethod(ClassMethod* methods, int count, ObjString* name) {
    if (!methods || !name) return NULL;
    for (int i = 0; i < count; i++) {
      if (typeNamesEqual(methods[i].name, name)) return &methods[i];
    }
    return NULL;
  }

  static void checkClassImplements(Compiler* c, Token classNameToken, ObjString* className,
                                   ClassMethod* methods, int methodCount,
                                   Type** interfaces, Token* interfaceTokens, int interfaceCount) {
    if (!typecheckEnabled(c) || !gTypeRegistry) return;
    for (int i = 0; i < interfaceCount; i++) {
      Type* ifaceType = interfaces[i];
      Token ifaceToken = interfaceTokens[i];
      if (!ifaceType || ifaceType->kind != TYPE_NAMED || !ifaceType->name) continue;
      InterfaceDef* iface = typeRegistryFindInterface(gTypeRegistry, ifaceType->name);
      if (!iface) {
        typeErrorAt(c, ifaceToken, "Unknown interface '%s'.", ifaceType->name->chars);
        continue;
      }

      if (iface->typeParamCount > 0 &&
          ifaceType->typeArgCount > 0 &&
          ifaceType->typeArgCount != iface->typeParamCount) {
        typeErrorAt(c, ifaceToken, "Interface '%s' expects %d type arguments but got %d.",
                    ifaceType->name->chars, iface->typeParamCount, ifaceType->typeArgCount);
      }

      int bindingCount = iface->typeParamCount;
      TypeBinding* bindings = NULL;
      if (bindingCount > 0) {
        bindings = (TypeBinding*)malloc(sizeof(TypeBinding) * (size_t)bindingCount);
        if (!bindings) {
          fprintf(stderr, "Out of memory.\n");
          exit(1);
        }
        for (int b = 0; b < bindingCount; b++) {
          bindings[b].name = iface->typeParams[b].name;
          bindings[b].constraint = iface->typeParams[b].constraint;
          bindings[b].bound = (ifaceType->typeArgCount > b) ? ifaceType->typeArgs[b] : NULL;
          if (bindings[b].bound &&
              !typeSatisfiesConstraint(bindings[b].bound, bindings[b].constraint)) {
            typeErrorAt(c, ifaceToken, "Type argument for '%s' must implement %s.",
                        bindings[b].name ? bindings[b].name->chars : "T",
                        bindings[b].constraint ? bindings[b].constraint->chars : "interface");
          }
        }
      }

      for (int m = 0; m < iface->methodCount; m++) {
        InterfaceMethod* method = &iface->methods[m];
        ClassMethod* impl = findClassMethod(methods, methodCount, method->name);
        if (!impl) {
          typeErrorAt(c, classNameToken,
                      "Class '%s' is missing method '%s' from interface '%s'.",
                      className ? className->chars : "class",
                      method->name ? method->name->chars : "method",
                      iface->name ? iface->name->chars : "interface");
          continue;
        }

        Type* expected = method->type;
        if (bindingCount > 0) {
          expected = typeSubstitute(c->typecheck, expected, bindings, bindingCount);
        }
        if (!typeAssignable(expected, impl->type)) {
          typeErrorAt(c, classNameToken,
                      "Method '%s' does not match interface '%s'.",
                      method->name ? method->name->chars : "method",
                      iface->name ? iface->name->chars : "interface");
        }
      }

      if (bindings) free(bindings);
    }
  }

  static void classDeclarationWithName(Compiler* c, Token name,
                                       bool isExport, bool exportDefault) {
    ObjString* className = stringFromToken(c->vm, name);
    int classTypeParamCount = 0;
    TypeParam* classTypeParams = parseTypeParams(c, &classTypeParamCount);
    int savedTypeParamCount = 0;
    if (typecheckEnabled(c)) {
      savedTypeParamCount = c->typecheck->typeParamCount;
      typeParamsPushList(c->typecheck, classTypeParams, classTypeParamCount);
    }

    Type** interfaces = NULL;
    Token* interfaceTokens = NULL;
    int interfaceCount = 0;
    int interfaceCapacity = 0;
    if (match(c, TOKEN_IMPLEMENTS)) {
      do {
        Token ifaceName = consume(c, TOKEN_IDENTIFIER, "Expect interface name.");
        Type* ifaceType = typeNamed(c->typecheck, stringFromToken(c->vm, ifaceName));
        if (check(c, TOKEN_LESS)) {
          ifaceType = parseTypeArguments(c, ifaceType, ifaceName);
        }
        if (interfaceCount >= interfaceCapacity) {
          int oldCap = interfaceCapacity;
          interfaceCapacity = GROW_CAPACITY(oldCap);
          interfaces = GROW_ARRAY(Type*, interfaces, oldCap, interfaceCapacity);
          interfaceTokens = GROW_ARRAY(Token, interfaceTokens, oldCap, interfaceCapacity);
          if (!interfaces || !interfaceTokens) {
            fprintf(stderr, "Out of memory.\n");
            exit(1);
          }
        }
        interfaces[interfaceCount] = ifaceType;
        interfaceTokens[interfaceCount] = ifaceName;
        interfaceCount++;
      } while (match(c, TOKEN_COMMA));
    }

    Token openBrace = consume(c, TOKEN_LEFT_BRACE, "Expect '{' before class body.");

    int nameConst = emitStringConstant(c, name);
    emitByte(c, OP_NULL, noToken());
    emitByte(c, OP_DEFINE_VAR, name);
    emitShort(c, (uint16_t)nameConst, name);

    int methodCount = 0;
    ClassMethod* methods = NULL;
    int methodCap = 0;
    bool classOk = true;
    while (!check(c, TOKEN_RIGHT_BRACE) && !isAtEnd(c)) {
      if (!match(c, TOKEN_FUN)) {
        errorAtCurrent(c, "Expect 'fun' before method declaration.");
        synchronize(c);
        break;
      }
      Token methodName = consume(c, TOKEN_IDENTIFIER, "Expect method name.");
      bool isInit = methodName.length == 4 && memcmp(methodName.start, "init", 4) == 0;
      Type* methodType = NULL;
      ObjFunction* method = compileFunction(c, methodName, isInit,
                                            typecheckEnabled(c) ? &methodType : NULL, false);
      if (!method) {
        classOk = false;
        break;
      }
      int constant = makeConstant(c, OBJ_VAL(method), methodName);
      emitByte(c, OP_CLOSURE, methodName);
      emitShort(c, (uint16_t)constant, methodName);
      methodCount++;
      if (typecheckEnabled(c) && methodType) {
        if (methodCount > methodCap) {
          int oldCap = methodCap;
          methodCap = GROW_CAPACITY(oldCap);
          methods = GROW_ARRAY(ClassMethod, methods, oldCap, methodCap);
          if (!methods) {
            fprintf(stderr, "Out of memory.\n");
            exit(1);
          }
        }
        methods[methodCount - 1].name = stringFromToken(c->vm, methodName);
        methods[methodCount - 1].type = methodType;
      }
    }
    if (!classOk) {
      goto class_cleanup;
    }
    consumeClosing(c, TOKEN_RIGHT_BRACE, "Expect '}' after class body.", openBrace);

    emitByte(c, OP_CLASS, name);
    emitShort(c, (uint16_t)nameConst, name);
    emitShort(c, (uint16_t)methodCount, name);
    if (isExport) {
      emitByte(c, OP_EXPORT, name);
      emitShort(c, (uint16_t)nameConst, name);
    }
    if (exportDefault) {
      emitGetVarConstant(c, nameConst);
      int defaultIdx = emitStringConstantFromChars(c, "default", 7);
      emitExportValue(c, (uint16_t)defaultIdx, name);
    }
    emitGc(c);

    if (typecheckEnabled(c)) {
      checkClassImplements(c, name, className, methods, methodCount,
                           interfaces, interfaceTokens, interfaceCount);
      if (gTypeRegistry) {
        ObjString** implemented = NULL;
        if (interfaceCount > 0) {
          implemented = (ObjString**)malloc(sizeof(ObjString*) * (size_t)interfaceCount);
          if (!implemented) {
            fprintf(stderr, "Out of memory.\n");
            exit(1);
          }
          for (int i = 0; i < interfaceCount; i++) {
            implemented[i] = interfaces[i]->name;
          }
        }
        typeRegistryAddClass(gTypeRegistry, className, implemented, interfaceCount);
      }
    }

class_cleanup:
    if (typecheckEnabled(c)) {
      typeParamsTruncate(c->typecheck, savedTypeParamCount);
    }
    free(methods);
    free(interfaces);
    free(interfaceTokens);
    free(classTypeParams);
    if (!classOk) return;
  }

  static void classDeclaration(Compiler* c, bool isExport) {
    Token name = consume(c, TOKEN_IDENTIFIER, "Expect class name.");
    classDeclarationWithName(c, name, isExport, false);
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
        Type* functionType = NULL;
        ObjFunction* function = compileFunction(c, name, false, &functionType, true);
        (void)functionType;
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
      classDeclarationWithName(c, name, false, allowExport);
      return;
    }
    expression(c);
    typePop(c);
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
    typePop(c);
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
      typePop(c);
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
    if (match(c, TOKEN_INTERFACE)) {
      interfaceDeclaration(c);
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
    } else if (match(c, TOKEN_INTERFACE)) {
      interfaceDeclaration(c);
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

static ObjFunction* compileFunction(Compiler* c, Token name, bool isInitializer,
                                    Type** outType, bool defineType) {
  int typeParamCount = 0;
  TypeParam* typeParams = parseTypeParams(c, &typeParamCount);
  int savedTypeParamCount = 0;
  if (typecheckEnabled(c)) {
    savedTypeParamCount = c->typecheck->typeParamCount;
    typeParamsPushList(c->typecheck, typeParams, typeParamCount);
  }

  Token openParen = consume(c, TOKEN_LEFT_PAREN, "Expect '(' after function name.");

  int arity = 0;
  bool sawDefault = false;
  int savedStart = c->current;

  if (!check(c, TOKEN_RIGHT_PAREN)) {
    do {
      if (!check(c, TOKEN_IDENTIFIER)) {
        errorAtCurrent(c, "Expect parameter name.");
        break;
      }
      advance(c);
      arity++;
      if (match(c, TOKEN_COLON)) {
        parseType(c);
      }
      if (match(c, TOKEN_EQUAL)) {
        sawDefault = true;
        int depth = 0;
        while (!isAtEnd(c)) {
          if (check(c, TOKEN_COMMA) && depth == 0) break;
          if (check(c, TOKEN_RIGHT_PAREN) && depth == 0) break;
          if (check(c, TOKEN_LEFT_PAREN) || check(c, TOKEN_LEFT_BRACKET) ||
              check(c, TOKEN_LEFT_BRACE)) depth++;
          if (check(c, TOKEN_RIGHT_PAREN) || check(c, TOKEN_RIGHT_BRACKET) ||
              check(c, TOKEN_RIGHT_BRACE)) depth--;
          advance(c);
        }
      }
    } while (match(c, TOKEN_COMMA));
  }
  consumeClosing(c, TOKEN_RIGHT_PAREN, "Expect ')' after parameters.", openParen);

  c->current = savedStart;

  int minArity = arity;
  ObjString** params = NULL;
  Token* paramTokens = NULL;
  Type** paramTypes = NULL;
  bool* paramHasType = NULL;
  int* defaultStarts = NULL;
  int* defaultEnds = NULL;
  if (arity > 0) {
    params = (ObjString**)malloc(sizeof(ObjString*) * (size_t)arity);
    paramTokens = (Token*)malloc(sizeof(Token) * (size_t)arity);
    paramTypes = (Type**)malloc(sizeof(Type*) * (size_t)arity);
    paramHasType = (bool*)malloc(sizeof(bool) * (size_t)arity);
    defaultStarts = (int*)malloc(sizeof(int) * (size_t)arity);
    defaultEnds = (int*)malloc(sizeof(int) * (size_t)arity);
    if (!params || !paramTokens || !paramTypes || !paramHasType ||
        !defaultStarts || !defaultEnds) {
      fprintf(stderr, "Out of memory.\n");
      exit(1);
    }
    for (int i = 0; i < arity; i++) {
      defaultStarts[i] = -1;
      defaultEnds[i] = -1;
      paramTypes[i] = typeUnknown();
      paramHasType[i] = false;
    }
  }

  sawDefault = false;
  int paramIdx = 0;
  if (!check(c, TOKEN_RIGHT_PAREN)) {
    do {
      if (paramIdx >= arity) break;
      Token paramName = consume(c, TOKEN_IDENTIFIER, "Expect parameter name.");
      params[paramIdx] = stringFromToken(c->vm, paramName);
      paramTokens[paramIdx] = paramName;
      if (match(c, TOKEN_COLON)) {
        paramTypes[paramIdx] = parseType(c);
        paramHasType[paramIdx] = true;
      }
      if (match(c, TOKEN_EQUAL)) {
        if (!sawDefault) minArity = paramIdx;
        sawDefault = true;
        defaultStarts[paramIdx] = c->current;
        int depth = 0;
        while (!isAtEnd(c)) {
          if (check(c, TOKEN_COMMA) && depth == 0) break;
          if (check(c, TOKEN_RIGHT_PAREN) && depth == 0) break;
          if (check(c, TOKEN_LEFT_PAREN) || check(c, TOKEN_LEFT_BRACKET) ||
              check(c, TOKEN_LEFT_BRACE)) depth++;
          if (check(c, TOKEN_RIGHT_PAREN) || check(c, TOKEN_RIGHT_BRACKET) ||
              check(c, TOKEN_RIGHT_BRACE)) depth--;
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

  Type* returnType = typeAny();
  if (match(c, TOKEN_COLON)) {
    returnType = parseType(c);
  }
  if (typecheckEnabled(c)) {
    typeParamsTruncate(c->typecheck, savedTypeParamCount);
  }
  Token openBrace = consume(c, TOKEN_LEFT_BRACE, "Expect '{' before function body.");
  int bodyStart = c->current;

  Type* functionType = NULL;
  if (outType) {
    functionType = typeFunction(c->typecheck, paramTypes, arity, returnType);
    if (functionType && typeParamCount > 0 && typeParams) {
      functionType->typeParamCount = typeParamCount;
      functionType->typeParams = (TypeParam*)malloc(sizeof(TypeParam) * (size_t)typeParamCount);
      if (!functionType->typeParams) {
        fprintf(stderr, "Out of memory.\n");
        exit(1);
      }
      for (int i = 0; i < typeParamCount; i++) {
        functionType->typeParams[i] = typeParams[i];
      }
    }
    *outType = functionType;
    if (defineType && typecheckEnabled(c)) {
      typeDefine(c, name, functionType, true);
    }
  }

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

  TypeChecker fnTypeChecker;
  typeCheckerInit(&fnTypeChecker, c->typecheck,
                  c->typecheck ? c->typecheck->enabled : false);
  fnTypeChecker.currentReturn = returnType;
    if (c->typecheck && c->typecheck->enabled) {
      typeParamsPushList(&fnTypeChecker, typeParams, typeParamCount);
    }
  fnCompiler.typecheck = &fnTypeChecker;

  if (typecheckEnabled(&fnCompiler)) {
    for (int i = 0; i < arity; i++) {
      Type* paramType = paramHasType && paramHasType[i] ? paramTypes[i] : typeUnknown();
      bool isExplicit = paramHasType && paramHasType[i];
      typeDefine(&fnCompiler, paramTokens[i], paramType, isExplicit);
    }
  }

  for (int i = 0; i < arity; i++) {
    if (!defaultStarts || defaultStarts[i] < 0) continue;

    Token ptoken = paramTokens[i];
    emitByte(&fnCompiler, OP_ARG_COUNT, ptoken);
    emitConstant(&fnCompiler, NUMBER_VAL((double)(i + 1)), ptoken);
    emitByte(&fnCompiler, OP_LESS, ptoken);
    int skipJump = emitJump(&fnCompiler, OP_JUMP_IF_FALSE, ptoken);
    emitByte(&fnCompiler, OP_POP, noToken());

    int savedCurrent = fnCompiler.current;
    fnCompiler.current = defaultStarts[i];
    expression(&fnCompiler);
    Type* defaultType = typePop(&fnCompiler);
    if (typecheckEnabled(&fnCompiler) && paramHasType && paramHasType[i]) {
      if (!typeAssignable(paramTypes[i], defaultType)) {
        char expected[64];
        char got[64];
        typeToString(paramTypes[i], expected, sizeof(expected));
        typeToString(defaultType, got, sizeof(got));
        typeErrorAt(&fnCompiler, ptoken, "Default value expects %s but got %s.", expected, got);
      }
    }
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
  free(paramTypes);
  free(paramHasType);
  free(defaultStarts);
  free(defaultEnds);
  free(typeParams);

  typeCheckerFree(&fnTypeChecker);

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
    TypeChecker typecheck;
    TypeRegistry registry;
    typeRegistryInit(&registry);
    typeCheckerInit(&typecheck, NULL, vm->typecheck);
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
    c.typecheck = &typecheck;
    vm->compiler = &c;
    gTypeRegistry = &registry;
    typeDefineStdlib(&c);

  while (!isAtEnd(&c)) {
    declaration(&c);
  }

  emitByte(&c, OP_NULL, noToken());
  emitByte(&c, OP_RETURN, noToken());

    vm->compiler = NULL;
    gTypeRegistry = NULL;

    *hadError = c.hadError;
    if (c.hadError) {
      typeCheckerFree(&typecheck);
      typeRegistryFree(&registry);
      return NULL;
    }
    optimizeChunk(vm, chunk);
    typeCheckerFree(&typecheck);
    typeRegistryFree(&registry);
    return function;
  }

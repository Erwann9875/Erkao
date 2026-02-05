#include "singlepass_internal.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>

void constValueFree(ConstValue* v) {
  if (v->type == CONST_STRING && v->ownsString) {
    free((void*)v->as.string.chars);
    v->ownsString = false;
  }
}

bool constValueIsTruthy(const ConstValue* v) {
  if (v->type == CONST_NULL) return false;
  if (v->type == CONST_BOOL) return v->as.boolean;
  return true;
}

bool constValueFromValue(Value value, ConstValue* out) {
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

bool constValueEquals(const ConstValue* a, const ConstValue* b) {
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

bool constValueConcat(const ConstValue* a, const ConstValue* b, ConstValue* out) {
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

bool constValueStringify(const ConstValue* input, ConstValue* out) {
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

void codeBuilderInit(CodeBuilder* out) {
  out->code = NULL;
  out->tokens = NULL;
  out->caches = NULL;
  out->count = 0;
  out->capacity = 0;
}

void codeBuilderEnsure(CodeBuilder* out, int needed) {
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

void codeEmitByte(CodeBuilder* out, uint8_t byte, Token token) {
  codeBuilderEnsure(out, out->count + 1);
  out->code[out->count] = byte;
  out->tokens[out->count] = token;
  if (out->caches) {
    memset(&out->caches[out->count], 0, sizeof(InlineCache));
  }
  out->count++;
}

void codeEmitShort(CodeBuilder* out, uint16_t value, Token token) {
  codeEmitByte(out, (uint8_t)((value >> 8) & 0xff), token);
  codeEmitByte(out, (uint8_t)(value & 0xff), token);
}

int instructionLength(const Chunk* chunk, int offset) {
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
    case OP_PRIVATE:
    case OP_JUMP:
    case OP_JUMP_IF_FALSE:
    case OP_LOOP:
    case OP_TRY:
    case OP_ARRAY:
    case OP_MAP:
      return 3;
    case OP_MATCH_ENUM:
      return 5;
    case OP_EXPORT_FROM: {
      if (offset + 3 > chunk->count) return 1;
      uint16_t count = (uint16_t)((chunk->code[offset + 1] << 8) | chunk->code[offset + 2]);
      return 3 + (int)count * 4;
    }
    case OP_DEFER:
    case OP_CALL:
    case OP_CALL_OPTIONAL:
      return 2;
    case OP_INVOKE:
      return 4;
    case OP_CLASS:
      return 5;
    case OP_STRUCT:
      return 3;
    case OP_END_TRY:
    case OP_THROW:
    case OP_TRY_UNWRAP:
      return 1;
    case OP_IMPORT:
      return 4;
    case OP_IMPORT_MODULE:
      return 1;
    default:
      return 1;
  }
}

bool instrPushesConst(const Chunk* chunk, const InstrInfo* instr, ConstValue* out) {
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

void optimizeChunk(VM* vm, Chunk* chunk) {
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
        case OP_MODULO:
          if (a.type == CONST_NUMBER && b.type == CONST_NUMBER) {
            result.type = CONST_NUMBER;
            result.ownsString = false;
            result.as.number = fmod(a.as.number, b.as.number);
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

bool isAtEnd(Compiler* c) {
  if (c->current >= c->tokens->count) return true;
  return c->tokens->tokens[c->current].type == TOKEN_EOF;
}

Token peek(Compiler* c) {
  if (c->tokens->count == 0) {
    Token t;
    memset(&t, 0, sizeof(Token));
    t.type = TOKEN_EOF;
    return t;
  }
  if (c->current >= c->tokens->count) {
    return c->tokens->tokens[c->tokens->count - 1];
  }
  return c->tokens->tokens[c->current];
}

Token previous(Compiler* c) {
  if (c->tokens->count == 0) {
    Token t;
    memset(&t, 0, sizeof(Token));
    t.type = TOKEN_EOF;
    return t;
  }
  if (c->current == 0) {
    return c->tokens->tokens[0];
  }
  if (c->current > c->tokens->count) {
    return c->tokens->tokens[c->tokens->count - 1];
  }
  return c->tokens->tokens[c->current - 1];
}

Token advance(Compiler* c) {
  if (c->current < c->tokens->count) c->current++;
  return previous(c);
}

bool check(Compiler* c, ErkaoTokenType type) {
  if (isAtEnd(c)) return false;
  return peek(c).type == type;
}

bool checkNext(Compiler* c, ErkaoTokenType type) {
  if (c->current + 1 >= c->tokens->count) return false;
  return c->tokens->tokens[c->current + 1].type == type;
}

bool checkNextNext(Compiler* c, ErkaoTokenType type) {
  if (c->current + 2 >= c->tokens->count) return false;
  return c->tokens->tokens[c->current + 2].type == type;
}

bool match(Compiler* c, ErkaoTokenType type) {
  if (!check(c, type)) return false;
  advance(c);
  return true;
}

Token noToken(void) {
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
  {TOKEN_DEFER, "defer"},
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
  {TOKEN_TYPE_KW, "type"},
  {TOKEN_NULL, "null"},
  {TOKEN_OR, "or"},
  {TOKEN_PRIVATE, "private"},
  {TOKEN_READONLY, "readonly"},
  {TOKEN_RETURN, "return"},
  {TOKEN_STRUCT, "struct"},
  {TOKEN_TRY, "try"},
  {TOKEN_CATCH, "catch"},
  {TOKEN_THROW, "throw"},
  {TOKEN_SWITCH, "switch"},
  {TOKEN_THIS, "this"},
  {TOKEN_TRUE, "true"},
  {TOKEN_YIELD, "yield"},
  {TOKEN_WHILE, "while"},
  {TOKEN_ERROR, NULL}
};

const char* keywordLexeme(ErkaoTokenType type) {
  for (int i = 0; keywordEntries[i].lexeme != NULL; i++) {
    if (keywordEntries[i].type == type) return keywordEntries[i].lexeme;
  }
  return NULL;
}

const char* tokenDescription(ErkaoTokenType type) {
  switch (type) {
    case TOKEN_LEFT_PAREN: return "'('";
    case TOKEN_RIGHT_PAREN: return "')'";
    case TOKEN_LEFT_BRACE: return "'{'";
    case TOKEN_RIGHT_BRACE: return "'}'";
    case TOKEN_LEFT_BRACKET: return "'['";
    case TOKEN_RIGHT_BRACKET: return "']'";
    case TOKEN_COMMA: return "','";
    case TOKEN_DOT: return "'.'";
    case TOKEN_DOT_DOT: return "'..'";
    case TOKEN_ELLIPSIS: return "'...'";
    case TOKEN_QUESTION: return "'?'";
    case TOKEN_QUESTION_DOT: return "'?.'";
    case TOKEN_MINUS: return "'-'";
    case TOKEN_PLUS: return "'+'";
    case TOKEN_SEMICOLON: return "';'";
    case TOKEN_SLASH: return "'/'";
    case TOKEN_STAR: return "'*'";
    case TOKEN_PERCENT: return "'%'";
    case TOKEN_COLON: return "':'";
    case TOKEN_CARET: return "'^'";
    case TOKEN_PIPE: return "'|'";
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
    case TOKEN_DEFER: return "'defer'";
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
    case TOKEN_TYPE_KW: return "'type'";
    case TOKEN_NULL: return "'null'";
    case TOKEN_OR: return "'or'";
    case TOKEN_PRIVATE: return "'private'";
    case TOKEN_READONLY: return "'readonly'";
    case TOKEN_RETURN: return "'return'";
    case TOKEN_STRUCT: return "'struct'";
    case TOKEN_TRY: return "'try'";
    case TOKEN_CATCH: return "'catch'";
    case TOKEN_THROW: return "'throw'";
    case TOKEN_SWITCH: return "'switch'";
    case TOKEN_THIS: return "'this'";
    case TOKEN_TRUE: return "'true'";
    case TOKEN_YIELD: return "'yield'";
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


void appendMessage(char* buffer, size_t size, const char* format, ...) {
  size_t length = strlen(buffer);
  if (length >= size) return;
  va_list args;
  va_start(args, format);
  vsnprintf(buffer + length, size - length, format, args);
  va_end(args);
}

void noteAt(Compiler* c, Token token, const char* message) {
  if (!message || message[0] == '\0') return;
  if (token.line <= 0 || token.column <= 0) return;
#ifdef ERKAO_FUZZING
  (void)c;
#else
  const char* path = c->path ? c->path : "<repl>";
  fprintf(stderr, "%s:%d:%d: Note: %s\n", path, token.line, token.column, message);
  printErrorContext(c->source, token.line, token.column,
                    token.length > 0 ? token.length : 1);
#endif
}

void synchronizeExpression(Compiler* c) {
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

void recoverAfterConsumeError(Compiler* c, ErkaoTokenType expected) {
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

bool openMatchesClose(ErkaoTokenType open, ErkaoTokenType close) {
  if (open == TOKEN_LEFT_PAREN && close == TOKEN_RIGHT_PAREN) return true;
  if (open == TOKEN_LEFT_BRACKET && close == TOKEN_RIGHT_BRACKET) return true;
  if (open == TOKEN_LEFT_BRACE && close == TOKEN_RIGHT_BRACE) return true;
  if (open == TOKEN_INTERP_START && close == TOKEN_INTERP_END) return true;
  return false;
}

void noteUnclosedDelimiter(Compiler* c, Token open, ErkaoTokenType close) {
  if (!openMatchesClose(open.type, close)) return;
  const char* desc = tokenDescription(open.type);
  char message[96];
  snprintf(message, sizeof(message), "Opening %s is here.", desc);
  noteAt(c, open, message);
}

void errorAt(Compiler* c, Token token, const char* message) {
  if (c->panicMode) return;
  c->panicMode = true;
  c->hadError = true;
#ifdef ERKAO_FUZZING
  (void)token;
  (void)message;
#else
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
#endif
}

void errorAtCurrent(Compiler* c, const char* message) {
  errorAt(c, peek(c), message);
}

Token consume(Compiler* c, ErkaoTokenType type, const char* message) {
  if (check(c, type)) return advance(c);
  Token found = peek(c);
  if (c->panicMode) return found;
  emitConsumeError(c, type, found, message);
  recoverAfterConsumeError(c, type);
  return found;
}

Token consumeClosing(Compiler* c, ErkaoTokenType type, const char* message,
                            Token open) {
  if (check(c, type)) return advance(c);
  Token found = peek(c);
  if (c->panicMode) return found;
  emitConsumeError(c, type, found, message);
  noteUnclosedDelimiter(c, open, type);
  recoverAfterConsumeError(c, type);
  return found;
}

void synchronize(Compiler* c) {
  c->panicMode = false;
  while (!isAtEnd(c)) {
    if (previous(c).type == TOKEN_SEMICOLON) return;
      switch (peek(c).type) {
        case TOKEN_CLASS: case TOKEN_STRUCT: case TOKEN_FUN: case TOKEN_LET: case TOKEN_CONST:
      case TOKEN_ENUM: case TOKEN_EXPORT: case TOKEN_IMPORT: case TOKEN_FROM:
      case TOKEN_TYPE_KW:
      case TOKEN_IF: case TOKEN_WHILE: case TOKEN_FOR: case TOKEN_FOREACH:
      case TOKEN_SWITCH: case TOKEN_MATCH: case TOKEN_RETURN:
      case TOKEN_TRY: case TOKEN_THROW: case TOKEN_CATCH: case TOKEN_YIELD:
      case TOKEN_DEFER:
      case TOKEN_BREAK: case TOKEN_CONTINUE:
        return;
      default:
        break;
    }
    advance(c);
  }
}

void emitByte(Compiler* c, uint8_t byte, Token token) {
  writeChunk(c->chunk, byte, token);
}

void emitBytes(Compiler* c, uint8_t a, uint8_t b, Token token) {
  emitByte(c, a, token);
  emitByte(c, b, token);
}

void emitShort(Compiler* c, uint16_t value, Token token) {
  emitByte(c, (uint8_t)((value >> 8) & 0xff), token);
  emitByte(c, (uint8_t)(value & 0xff), token);
}

int makeConstant(Compiler* c, Value value, Token token) {
  int index = addConstant(c->chunk, value);
  if (index > UINT16_MAX) {
    errorAt(c, token, "Too many constants in chunk.");
    return 0;
  }
  return index;
}

void emitConstant(Compiler* c, Value value, Token token) {
  int constant = makeConstant(c, value, token);
  emitByte(c, OP_CONSTANT, token);
  emitShort(c, (uint16_t)constant, token);
}

int emitJump(Compiler* c, uint8_t instruction, Token token) {
  emitByte(c, instruction, token);
  emitByte(c, 0xff, token);
  emitByte(c, 0xff, token);
  return c->chunk->count - 2;
}

void patchJump(Compiler* c, int offset, Token token) {
  int jump = c->chunk->count - offset - 2;
  if (jump > UINT16_MAX) {
    errorAt(c, token, "Too much code to jump over.");
    return;
  }
  c->chunk->code[offset] = (uint8_t)((jump >> 8) & 0xff);
  c->chunk->code[offset + 1] = (uint8_t)(jump & 0xff);
}

void emitLoop(Compiler* c, int loopStart, Token token) {
  emitByte(c, OP_LOOP, token);
  int offset = c->chunk->count - loopStart + 2;
  if (offset > UINT16_MAX) {
    errorAt(c, token, "Loop body too large.");
    return;
  }
  emitShort(c, (uint16_t)offset, token);
}

int emitStringConstant(Compiler* c, Token token) {
  ObjString* name = stringFromToken(c->vm, token);
  return makeConstant(c, OBJ_VAL(name), token);
}

int emitStringConstantFromChars(Compiler* c, const char* chars, int len) {
  ObjString* name = copyStringWithLength(c->vm, chars, len);
  return makeConstant(c, OBJ_VAL(name), noToken());
}

int emitTempNameConstant(Compiler* c, const char* prefix) {
  char buffer[64];
  int length = snprintf(buffer, sizeof(buffer), "__%s%d", prefix, c->tempIndex++);
  if (length < 0) length = 0;
  if (length >= (int)sizeof(buffer)) length = (int)sizeof(buffer) - 1;
  return emitStringConstantFromChars(c, buffer, length);
}

void emitGetVarConstant(Compiler* c, int idx) {
  emitByte(c, OP_GET_VAR, noToken());
  emitShort(c, (uint16_t)idx, noToken());
}

void emitSetVarConstant(Compiler* c, int idx) {
  emitByte(c, OP_SET_VAR, noToken());
  emitShort(c, (uint16_t)idx, noToken());
}

void emitDefineVarConstant(Compiler* c, int idx) {
  emitByte(c, OP_DEFINE_VAR, noToken());
  emitShort(c, (uint16_t)idx, noToken());
}

void emitExportName(Compiler* c, Token name) {
  int nameIdx = emitStringConstant(c, name);
  emitByte(c, OP_EXPORT, name);
  emitShort(c, (uint16_t)nameIdx, name);
}

void emitExportValue(Compiler* c, uint16_t nameIdx, Token token) {
  emitByte(c, OP_EXPORT_VALUE, token);
  emitShort(c, nameIdx, token);
}

void emitPrivateName(Compiler* c, int nameIdx, Token token) {
  emitByte(c, OP_PRIVATE, token);
  emitShort(c, (uint16_t)nameIdx, token);
}

void emitGc(Compiler* c) {
  emitByte(c, OP_GC, noToken());
}

void initJumpList(JumpList* list) {
  list->offsets = NULL;
  list->count = 0;
  list->capacity = 0;
}

void writeJumpList(JumpList* list, int offset) {
  if (list->capacity < list->count + 1) {
    int oldCap = list->capacity;
    list->capacity = GROW_CAPACITY(oldCap);
    list->offsets = GROW_ARRAY(int, list->offsets, oldCap, list->capacity);
  }
  list->offsets[list->count++] = offset;
}

void freeJumpList(JumpList* list) {
  FREE_ARRAY(int, list->offsets, list->capacity);
  initJumpList(list);
}


void patchJumpTo(Compiler* c, int offset, int target, Token token) {
  int jump = target - offset - 2;
  if (jump < 0 || jump > UINT16_MAX) {
    errorAt(c, token, "Too much code to jump over.");
    return;
  }
  c->chunk->code[offset] = (uint8_t)((jump >> 8) & 0xff);
  c->chunk->code[offset + 1] = (uint8_t)(jump & 0xff);
}

void patchJumpList(Compiler* c, JumpList* list, int target, Token token) {
  for (int i = 0; i < list->count; i++) {
    patchJumpTo(c, list->offsets[i], target, token);
  }
}

void emitScopeExits(Compiler* c, int targetDepth) {
  for (int depth = c->scopeDepth; depth > targetDepth; depth--) {
    emitByte(c, OP_END_SCOPE, noToken());
  }
}

BreakContext* findLoopContext(Compiler* c) {
  for (BreakContext* ctx = c->breakContext; ctx; ctx = ctx->enclosing) {
    if (ctx->type == BREAK_LOOP) return ctx;
  }
  return NULL;
}

char* copyTokenLexeme(Token token) {
  char* buffer = (char*)malloc((size_t)token.length + 1);
  if (!buffer) { fprintf(stderr, "Out of memory.\n"); exit(1); }
  memcpy(buffer, token.start, (size_t)token.length);
  buffer[token.length] = '\0';
  return buffer;
}

Token syntheticToken(const char* text) {
  Token token;
  memset(&token, 0, sizeof(Token));
  token.start = text;
  token.length = (int)strlen(text);
  return token;
}

void enumInfoSetAdt(EnumInfo* info, bool isAdt) {
  if (info) {
    info->isAdt = isAdt;
  }
}

void enumInfoFree(EnumInfo* info) {
  if (!info) return;
  for (int i = 0; i < info->variantCount; i++) {
    free(info->variants[i].name);
  }
  free(info->variants);
  free(info->name);
  info->variants = NULL;
  info->variantCount = 0;
  info->variantCapacity = 0;
}

void compilerEnumsFree(Compiler* c) {
  if (!c || !c->enums) return;
  for (int i = 0; i < c->enumCount; i++) {
    enumInfoFree(&c->enums[i]);
  }
  free(c->enums);
  c->enums = NULL;
  c->enumCount = 0;
  c->enumCapacity = 0;
}

StructInfo* compilerAddStruct(Compiler* c, Token name) {
  if (c->structCount >= c->structCapacity) {
    int oldCap = c->structCapacity;
    c->structCapacity = GROW_CAPACITY(oldCap);
    c->structs = (StructInfo*)realloc(c->structs,
                                      sizeof(StructInfo) * (size_t)c->structCapacity);
    if (!c->structs) {
      fprintf(stderr, "Out of memory.\n");
      exit(1);
    }
    memset(c->structs + oldCap, 0, sizeof(StructInfo) * (size_t)(c->structCapacity - oldCap));
  }
  StructInfo* info = &c->structs[c->structCount++];
  info->name = copyTokenLexeme(name);
  info->nameLength = name.length;
  return info;
}

StructInfo* findStructInfo(Compiler* c, Token name) {
  for (Compiler* cur = c; cur; cur = cur->enclosing) {
    for (int i = 0; i < cur->structCount; i++) {
      StructInfo* info = &cur->structs[i];
      if (info->nameLength != name.length) continue;
      if (memcmp(info->name, name.start, (size_t)name.length) == 0) {
        return info;
      }
    }
  }
  return NULL;
}

void compilerStructsFree(Compiler* c) {
  if (!c || !c->structs) return;
  for (int i = 0; i < c->structCount; i++) {
    free(c->structs[i].name);
  }
  free(c->structs);
  c->structs = NULL;
  c->structCount = 0;
  c->structCapacity = 0;
}

EnumInfo* compilerAddEnum(Compiler* c, Token name) {
  if (c->enumCount >= c->enumCapacity) {
    int oldCap = c->enumCapacity;
    c->enumCapacity = GROW_CAPACITY(oldCap);
    c->enums = (EnumInfo*)realloc(c->enums, sizeof(EnumInfo) * (size_t)c->enumCapacity);
    if (!c->enums) {
      fprintf(stderr, "Out of memory.\n");
      exit(1);
    }
    memset(c->enums + oldCap, 0, sizeof(EnumInfo) * (size_t)(c->enumCapacity - oldCap));
  }
  EnumInfo* info = &c->enums[c->enumCount++];
  info->name = copyTokenLexeme(name);
  info->nameLength = name.length;
  info->variants = NULL;
  info->variantCount = 0;
  info->variantCapacity = 0;
  info->isAdt = false;
  return info;
}

EnumInfo* findEnumInfo(Compiler* c, Token name) {
  for (Compiler* cur = c; cur; cur = cur->enclosing) {
    for (int i = 0; i < cur->enumCount; i++) {
      EnumInfo* info = &cur->enums[i];
      if (info->nameLength != name.length) continue;
      if (memcmp(info->name, name.start, (size_t)name.length) == 0) {
        return info;
      }
    }
  }
  return NULL;
}

EnumVariantInfo* enumInfoAddVariant(EnumInfo* info, Token name, int arity) {
  for (int i = 0; i < info->variantCount; i++) {
    EnumVariantInfo* existing = &info->variants[i];
    if (existing->nameLength == name.length &&
        memcmp(existing->name, name.start, (size_t)name.length) == 0) {
      return existing;
    }
  }
  if (info->variantCount >= info->variantCapacity) {
    int oldCap = info->variantCapacity;
    info->variantCapacity = GROW_CAPACITY(oldCap);
    info->variants = (EnumVariantInfo*)realloc(info->variants,
                                               sizeof(EnumVariantInfo) * (size_t)info->variantCapacity);
    if (!info->variants) {
      fprintf(stderr, "Out of memory.\n");
      exit(1);
    }
    memset(info->variants + oldCap, 0,
           sizeof(EnumVariantInfo) * (size_t)(info->variantCapacity - oldCap));
  }
  EnumVariantInfo* variant = &info->variants[info->variantCount++];
  variant->name = copyTokenLexeme(name);
  variant->nameLength = name.length;
  variant->arity = arity;
  return variant;
}

EnumVariantInfo* findEnumVariant(EnumInfo* info, Token name) {
  if (!info) return NULL;
  for (int i = 0; i < info->variantCount; i++) {
    EnumVariantInfo* variant = &info->variants[i];
    if (variant->nameLength != name.length) continue;
    if (memcmp(variant->name, name.start, (size_t)name.length) == 0) {
      return variant;
    }
  }
  return NULL;
}

int enumVariantIndex(EnumInfo* info, Token name) {
  if (!info) return -1;
  for (int i = 0; i < info->variantCount; i++) {
    EnumVariantInfo* variant = &info->variants[i];
    if (variant->nameLength != name.length) continue;
    if (memcmp(variant->name, name.start, (size_t)name.length) == 0) {
      return i;
    }
  }
  return -1;
}

char* parseStringChars(const char* src, int length) {
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

bool isTripleQuoted(Token token) {
  if (token.length < 6) return false;
  return token.start[0] == '"' && token.start[1] == '"' && token.start[2] == '"' &&
         token.start[token.length - 1] == '"' &&
         token.start[token.length - 2] == '"' &&
         token.start[token.length - 3] == '"';
}

char* parseStringLiteral(Token token) {
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

char* parseStringSegment(Token token) {
  return parseStringChars(token.start, token.length);
}


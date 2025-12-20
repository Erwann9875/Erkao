#include "compiler.h"
#include "chunk.h"
#include "value.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  VM* vm;
  Program* program;
  Chunk* chunk;
  bool hadError;
  int scopeDepth;
  int tempIndex;
  struct BreakContext* breakContext;
} Compiler;

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
    struct {
      const char* chars;
      int length;
    } string;
  } as;
} ConstValue;

static void constValueFree(ConstValue* value) {
  if (value->type == CONST_STRING && value->ownsString) {
    free((void*)value->as.string.chars);
    value->ownsString = false;
  }
}

static ConstValue constValueTake(ConstValue* value) {
  ConstValue out = *value;
  value->ownsString = false;
  return out;
}

static bool constValueIsTruthy(const ConstValue* value) {
  if (value->type == CONST_NULL) return false;
  if (value->type == CONST_BOOL) return value->as.boolean;
  return true;
}

static bool constValuesEqual(const ConstValue* a, const ConstValue* b) {
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

static bool evalConstExpr(Expr* expr, ConstValue* out) {
  if (!expr) return false;

  switch (expr->type) {
    case EXPR_LITERAL: {
      Literal literal = expr->as.literal.literal;
      switch (literal.type) {
        case LIT_NUMBER:
          out->type = CONST_NUMBER;
          out->as.number = literal.as.number;
          return true;
        case LIT_STRING:
          out->type = CONST_STRING;
          out->ownsString = false;
          out->as.string.chars = literal.as.string;
          out->as.string.length = (int)strlen(literal.as.string);
          return true;
        case LIT_BOOL:
          out->type = CONST_BOOL;
          out->as.boolean = literal.as.boolean;
          return true;
        case LIT_NULL:
          out->type = CONST_NULL;
          return true;
      }
      break;
    }
    case EXPR_GROUPING:
      return evalConstExpr(expr->as.grouping.expression, out);
    case EXPR_UNARY: {
      ConstValue right;
      memset(&right, 0, sizeof(ConstValue));
      if (!evalConstExpr(expr->as.unary.right, &right)) return false;

      bool ok = false;
      switch (expr->as.unary.op.type) {
        case TOKEN_MINUS:
          if (right.type == CONST_NUMBER) {
            out->type = CONST_NUMBER;
            out->as.number = -right.as.number;
            ok = true;
          }
          break;
        case TOKEN_BANG:
          out->type = CONST_BOOL;
          out->as.boolean = !constValueIsTruthy(&right);
          ok = true;
          break;
        default:
          break;
      }

      constValueFree(&right);
      return ok;
    }
    case EXPR_BINARY: {
      ConstValue left;
      ConstValue right;
      memset(&left, 0, sizeof(ConstValue));
      memset(&right, 0, sizeof(ConstValue));
      if (!evalConstExpr(expr->as.binary.left, &left)) return false;
      if (!evalConstExpr(expr->as.binary.right, &right)) {
        constValueFree(&left);
        return false;
      }

      bool ok = false;
      switch (expr->as.binary.op.type) {
        case TOKEN_PLUS:
          if (left.type == CONST_NUMBER && right.type == CONST_NUMBER) {
            out->type = CONST_NUMBER;
            out->as.number = left.as.number + right.as.number;
            ok = true;
          } else if (left.type == CONST_STRING && right.type == CONST_STRING) {
            int length = left.as.string.length + right.as.string.length;
            char* buffer = (char*)malloc((size_t)length + 1);
            if (!buffer) {
              fprintf(stderr, "Out of memory.\n");
              exit(1);
            }
            memcpy(buffer, left.as.string.chars, (size_t)left.as.string.length);
            memcpy(buffer + left.as.string.length, right.as.string.chars,
                   (size_t)right.as.string.length);
            buffer[length] = '\0';
            out->type = CONST_STRING;
            out->ownsString = true;
            out->as.string.chars = buffer;
            out->as.string.length = length;
            ok = true;
          }
          break;
        case TOKEN_MINUS:
          if (left.type == CONST_NUMBER && right.type == CONST_NUMBER) {
            out->type = CONST_NUMBER;
            out->as.number = left.as.number - right.as.number;
            ok = true;
          }
          break;
        case TOKEN_STAR:
          if (left.type == CONST_NUMBER && right.type == CONST_NUMBER) {
            out->type = CONST_NUMBER;
            out->as.number = left.as.number * right.as.number;
            ok = true;
          }
          break;
        case TOKEN_SLASH:
          if (left.type == CONST_NUMBER && right.type == CONST_NUMBER) {
            out->type = CONST_NUMBER;
            out->as.number = left.as.number / right.as.number;
            ok = true;
          }
          break;
        case TOKEN_GREATER:
          if (left.type == CONST_NUMBER && right.type == CONST_NUMBER) {
            out->type = CONST_BOOL;
            out->as.boolean = left.as.number > right.as.number;
            ok = true;
          }
          break;
        case TOKEN_GREATER_EQUAL:
          if (left.type == CONST_NUMBER && right.type == CONST_NUMBER) {
            out->type = CONST_BOOL;
            out->as.boolean = left.as.number >= right.as.number;
            ok = true;
          }
          break;
        case TOKEN_LESS:
          if (left.type == CONST_NUMBER && right.type == CONST_NUMBER) {
            out->type = CONST_BOOL;
            out->as.boolean = left.as.number < right.as.number;
            ok = true;
          }
          break;
        case TOKEN_LESS_EQUAL:
          if (left.type == CONST_NUMBER && right.type == CONST_NUMBER) {
            out->type = CONST_BOOL;
            out->as.boolean = left.as.number <= right.as.number;
            ok = true;
          }
          break;
        case TOKEN_EQUAL_EQUAL:
          out->type = CONST_BOOL;
          out->as.boolean = constValuesEqual(&left, &right);
          ok = true;
          break;
        case TOKEN_BANG_EQUAL:
          out->type = CONST_BOOL;
          out->as.boolean = !constValuesEqual(&left, &right);
          ok = true;
          break;
        default:
          break;
      }

      constValueFree(&left);
      constValueFree(&right);
      return ok;
    }
    case EXPR_LOGICAL: {
      ConstValue left;
      ConstValue right;
      memset(&left, 0, sizeof(ConstValue));
      memset(&right, 0, sizeof(ConstValue));
      if (!evalConstExpr(expr->as.logical.left, &left)) return false;
      if (!evalConstExpr(expr->as.logical.right, &right)) {
        constValueFree(&left);
        return false;
      }

      bool leftTruthy = constValueIsTruthy(&left);
      if (expr->as.logical.op.type == TOKEN_OR) {
        if (leftTruthy) {
          *out = constValueTake(&left);
          constValueFree(&right);
        } else {
          *out = constValueTake(&right);
          constValueFree(&left);
        }
        return true;
      }
      if (expr->as.logical.op.type == TOKEN_AND) {
        if (leftTruthy) {
          *out = constValueTake(&right);
          constValueFree(&left);
        } else {
          *out = constValueTake(&left);
          constValueFree(&right);
        }
        return true;
      }

      constValueFree(&left);
      constValueFree(&right);
      return false;
    }
    default:
      break;
  }

  return false;
}

static Token noToken(void) {
  Token token;
  memset(&token, 0, sizeof(Token));
  return token;
}

static void compilerErrorAt(Compiler* compiler, Token token, const char* message) {
  if (compiler->hadError) return;
  compiler->hadError = true;

  const char* displayPath = "<repl>";
  const char* source = NULL;
  if (compiler->program) {
    if (compiler->program->path) {
      displayPath = compiler->program->path;
    }
    source = compiler->program->source;
  }

  if (token.line > 0 && token.column > 0) {
    fprintf(stderr, "%s:%d:%d: CompileError", displayPath, token.line, token.column);
    if (token.length > 0) {
      fprintf(stderr, " at '%.*s'", token.length, token.start);
    }
    fprintf(stderr, ": %s\n", message);
    if (source) {
      int length = token.length > 0 ? token.length : 1;
      printErrorContext(source, token.line, token.column, length);
    }
  } else {
    fprintf(stderr, "%s: CompileError: %s\n", displayPath, message);
  }
}

static void compilerError(Compiler* compiler, const char* message) {
  compilerErrorAt(compiler, noToken(), message);
}

static void emitByte(Compiler* compiler, uint8_t byte, Token token) {
  writeChunk(compiler->chunk, byte, token);
}

static void emitBytes(Compiler* compiler, uint8_t a, uint8_t b, Token token) {
  emitByte(compiler, a, token);
  emitByte(compiler, b, token);
}

static void emitShort(Compiler* compiler, uint16_t value, Token token) {
  emitByte(compiler, (uint8_t)((value >> 8) & 0xff), token);
  emitByte(compiler, (uint8_t)(value & 0xff), token);
}

static int makeConstant(Compiler* compiler, Value value, Token token) {
  int index = addConstant(compiler->chunk, value);
  if (index > UINT16_MAX) {
    compilerErrorAt(compiler, token, "Too many constants in chunk.");
    return 0;
  }
  return index;
}

static void emitConstant(Compiler* compiler, Value value, Token token) {
  int constant = makeConstant(compiler, value, token);
  emitByte(compiler, OP_CONSTANT, token);
  emitShort(compiler, (uint16_t)constant, token);
}

static void emitConstValue(Compiler* compiler, ConstValue* value, Token token) {
  switch (value->type) {
    case CONST_NULL:
      emitByte(compiler, OP_NULL, token);
      break;
    case CONST_BOOL:
      emitByte(compiler, value->as.boolean ? OP_TRUE : OP_FALSE, token);
      break;
    case CONST_NUMBER:
      emitConstant(compiler, NUMBER_VAL(value->as.number), token);
      break;
    case CONST_STRING: {
      ObjString* string = NULL;
      if (value->ownsString) {
        string = takeStringWithLength(compiler->vm, (char*)value->as.string.chars,
                                      value->as.string.length);
        value->ownsString = false;
      } else {
        string = copyStringWithLength(compiler->vm, value->as.string.chars,
                                      value->as.string.length);
      }
      emitConstant(compiler, OBJ_VAL(string), token);
      break;
    }
  }
}

static int emitJump(Compiler* compiler, uint8_t instruction, Token token) {
  emitByte(compiler, instruction, token);
  emitByte(compiler, 0xff, token);
  emitByte(compiler, 0xff, token);
  return compiler->chunk->count - 2;
}

static void patchJump(Compiler* compiler, int offset, Token token) {
  int jump = compiler->chunk->count - offset - 2;
  if (jump > UINT16_MAX) {
    compilerErrorAt(compiler, token, "Too much code to jump over.");
    return;
  }
  compiler->chunk->code[offset] = (uint8_t)((jump >> 8) & 0xff);
  compiler->chunk->code[offset + 1] = (uint8_t)(jump & 0xff);
}

static void emitLoop(Compiler* compiler, int loopStart, Token token) {
  emitByte(compiler, OP_LOOP, token);
  int offset = compiler->chunk->count - loopStart + 2;
  if (offset > UINT16_MAX) {
    compilerErrorAt(compiler, token, "Loop body too large.");
    return;
  }
  emitShort(compiler, (uint16_t)offset, token);
}

static int emitStringConstant(Compiler* compiler, Token token) {
  ObjString* name = stringFromToken(compiler->vm, token);
  return makeConstant(compiler, OBJ_VAL(name), token);
}

static void initJumpList(JumpList* list) {
  list->offsets = NULL;
  list->count = 0;
  list->capacity = 0;
}

static void writeJumpList(JumpList* list, int offset) {
  if (list->capacity < list->count + 1) {
    int oldCapacity = list->capacity;
    list->capacity = GROW_CAPACITY(oldCapacity);
    list->offsets = GROW_ARRAY(int, list->offsets, oldCapacity, list->capacity);
  }
  list->offsets[list->count++] = offset;
}

static void freeJumpList(JumpList* list) {
  FREE_ARRAY(int, list->offsets, list->capacity);
  initJumpList(list);
}

static void patchJumpTo(Compiler* compiler, int offset, int target, Token token) {
  int jump = target - offset - 2;
  if (jump < 0 || jump > UINT16_MAX) {
    compilerErrorAt(compiler, token, "Too much code to jump over.");
    return;
  }
  compiler->chunk->code[offset] = (uint8_t)((jump >> 8) & 0xff);
  compiler->chunk->code[offset + 1] = (uint8_t)(jump & 0xff);
}

static void patchJumpList(Compiler* compiler, JumpList* list, int target, Token token) {
  for (int i = 0; i < list->count; i++) {
    patchJumpTo(compiler, list->offsets[i], target, token);
  }
}

static void emitScopeExits(Compiler* compiler, int targetDepth) {
  for (int depth = compiler->scopeDepth; depth > targetDepth; depth--) {
    emitByte(compiler, OP_END_SCOPE, noToken());
  }
}

static BreakContext* findLoopContext(Compiler* compiler) {
  for (BreakContext* ctx = compiler->breakContext; ctx; ctx = ctx->enclosing) {
    if (ctx->type == BREAK_LOOP) return ctx;
  }
  return NULL;
}

static int emitStringConstantFromChars(Compiler* compiler, const char* chars, int length) {
  ObjString* name = copyStringWithLength(compiler->vm, chars, length);
  return makeConstant(compiler, OBJ_VAL(name), noToken());
}

static int emitTempNameConstant(Compiler* compiler, const char* prefix) {
  char buffer[64];
  int length = snprintf(buffer, sizeof(buffer), "__%s%d", prefix, compiler->tempIndex++);
  if (length < 0) length = 0;
  if (length >= (int)sizeof(buffer)) length = (int)sizeof(buffer) - 1;
  return emitStringConstantFromChars(compiler, buffer, length);
}

static void emitGetVarConstant(Compiler* compiler, int nameIndex) {
  emitByte(compiler, OP_GET_VAR, noToken());
  emitShort(compiler, (uint16_t)nameIndex, noToken());
}

static void emitSetVarConstant(Compiler* compiler, int nameIndex) {
  emitByte(compiler, OP_SET_VAR, noToken());
  emitShort(compiler, (uint16_t)nameIndex, noToken());
}

static void emitDefineVarConstant(Compiler* compiler, int nameIndex) {
  emitByte(compiler, OP_DEFINE_VAR, noToken());
  emitShort(compiler, (uint16_t)nameIndex, noToken());
}

static void emitGc(Compiler* compiler) {
  emitByte(compiler, OP_GC, noToken());
}

static void compileExpr(Compiler* compiler, Expr* expr);
static void compileStmt(Compiler* compiler, Stmt* stmt);
static void optimizeChunk(Chunk* chunk);

static void compileExprArray(Compiler* compiler, ExprArray* array) {
  for (int i = 0; i < array->count; i++) {
    compileExpr(compiler, array->items[i]);
  }
}

static void compileExpr(Compiler* compiler, Expr* expr) {
  if (!expr || compiler->hadError) return;

  ConstValue folded;
  memset(&folded, 0, sizeof(ConstValue));
  if (evalConstExpr(expr, &folded)) {
    emitConstValue(compiler, &folded, noToken());
    constValueFree(&folded);
    return;
  }

  switch (expr->type) {
    case EXPR_LITERAL: {
      Literal literal = expr->as.literal.literal;
      switch (literal.type) {
        case LIT_NUMBER:
          emitConstant(compiler, NUMBER_VAL(literal.as.number), noToken());
          break;
        case LIT_STRING: {
          ObjString* string = copyString(compiler->vm, literal.as.string);
          emitConstant(compiler, OBJ_VAL(string), noToken());
          break;
        }
        case LIT_BOOL:
          emitByte(compiler, literal.as.boolean ? OP_TRUE : OP_FALSE, noToken());
          break;
        case LIT_NULL:
          emitByte(compiler, OP_NULL, noToken());
          break;
      }
      break;
    }
    case EXPR_GROUPING:
      compileExpr(compiler, expr->as.grouping.expression);
      break;
    case EXPR_UNARY: {
      compileExpr(compiler, expr->as.unary.right);
      switch (expr->as.unary.op.type) {
        case TOKEN_MINUS:
          emitByte(compiler, OP_NEGATE, expr->as.unary.op);
          break;
        case TOKEN_BANG:
          emitByte(compiler, OP_NOT, expr->as.unary.op);
          break;
        default:
          break;
      }
      break;
    }
    case EXPR_BINARY: {
      compileExpr(compiler, expr->as.binary.left);
      compileExpr(compiler, expr->as.binary.right);
      Token op = expr->as.binary.op;
      switch (op.type) {
        case TOKEN_PLUS:
          emitByte(compiler, OP_ADD, op);
          break;
        case TOKEN_MINUS:
          emitByte(compiler, OP_SUBTRACT, op);
          break;
        case TOKEN_STAR:
          emitByte(compiler, OP_MULTIPLY, op);
          break;
        case TOKEN_SLASH:
          emitByte(compiler, OP_DIVIDE, op);
          break;
        case TOKEN_GREATER:
          emitByte(compiler, OP_GREATER, op);
          break;
        case TOKEN_GREATER_EQUAL:
          emitByte(compiler, OP_GREATER_EQUAL, op);
          break;
        case TOKEN_LESS:
          emitByte(compiler, OP_LESS, op);
          break;
        case TOKEN_LESS_EQUAL:
          emitByte(compiler, OP_LESS_EQUAL, op);
          break;
        case TOKEN_BANG_EQUAL:
          emitByte(compiler, OP_EQUAL, op);
          emitByte(compiler, OP_NOT, op);
          break;
        case TOKEN_EQUAL_EQUAL:
          emitByte(compiler, OP_EQUAL, op);
          break;
        default:
          break;
      }
      break;
    }
    case EXPR_VARIABLE: {
      int name = emitStringConstant(compiler, expr->as.variable.name);
      emitByte(compiler, OP_GET_VAR, expr->as.variable.name);
      emitShort(compiler, (uint16_t)name, expr->as.variable.name);
      break;
    }
    case EXPR_ASSIGN: {
      compileExpr(compiler, expr->as.assign.value);
      int name = emitStringConstant(compiler, expr->as.assign.name);
      emitByte(compiler, OP_SET_VAR, expr->as.assign.name);
      emitShort(compiler, (uint16_t)name, expr->as.assign.name);
      break;
    }
    case EXPR_LOGICAL: {
      compileExpr(compiler, expr->as.logical.left);
      if (expr->as.logical.op.type == TOKEN_OR) {
        int jumpIfFalse = emitJump(compiler, OP_JUMP_IF_FALSE, expr->as.logical.op);
        int jumpToEnd = emitJump(compiler, OP_JUMP, expr->as.logical.op);
        patchJump(compiler, jumpIfFalse, expr->as.logical.op);
        emitByte(compiler, OP_POP, noToken());
        compileExpr(compiler, expr->as.logical.right);
        patchJump(compiler, jumpToEnd, expr->as.logical.op);
      } else {
        int jumpIfFalse = emitJump(compiler, OP_JUMP_IF_FALSE, expr->as.logical.op);
        emitByte(compiler, OP_POP, noToken());
        compileExpr(compiler, expr->as.logical.right);
        patchJump(compiler, jumpIfFalse, expr->as.logical.op);
      }
      break;
    }
    case EXPR_CALL: {
      compileExpr(compiler, expr->as.call.callee);
      compileExprArray(compiler, &expr->as.call.args);
      int argCount = expr->as.call.args.count;
      if (argCount > UINT8_MAX) {
        compilerError(compiler, "Too many arguments in call.");
        return;
      }
      emitByte(compiler, OP_CALL, expr->as.call.paren);
      emitByte(compiler, (uint8_t)argCount, expr->as.call.paren);
      break;
    }
    case EXPR_GET: {
      compileExpr(compiler, expr->as.get.object);
      int name = emitStringConstant(compiler, expr->as.get.name);
      emitByte(compiler, OP_GET_PROPERTY, expr->as.get.name);
      emitShort(compiler, (uint16_t)name, expr->as.get.name);
      break;
    }
    case EXPR_SET: {
      compileExpr(compiler, expr->as.set.object);
      compileExpr(compiler, expr->as.set.value);
      int name = emitStringConstant(compiler, expr->as.set.name);
      emitByte(compiler, OP_SET_PROPERTY, expr->as.set.name);
      emitShort(compiler, (uint16_t)name, expr->as.set.name);
      break;
    }
    case EXPR_THIS: {
      int name = emitStringConstant(compiler, expr->as.thisExpr.keyword);
      emitByte(compiler, OP_GET_THIS, expr->as.thisExpr.keyword);
      emitShort(compiler, (uint16_t)name, expr->as.thisExpr.keyword);
      break;
    }
    case EXPR_ARRAY: {
      if (expr->as.array.elements.count > UINT16_MAX) {
        compilerErrorAt(compiler, noToken(), "Array literal too large.");
        return;
      }
      emitByte(compiler, OP_ARRAY, noToken());
      emitShort(compiler, (uint16_t)expr->as.array.elements.count, noToken());
      for (int i = 0; i < expr->as.array.elements.count; i++) {
        compileExpr(compiler, expr->as.array.elements.items[i]);
        emitByte(compiler, OP_ARRAY_APPEND, noToken());
      }
      break;
    }
    case EXPR_MAP: {
      if (expr->as.map.entries.count > UINT16_MAX) {
        compilerErrorAt(compiler, noToken(), "Map literal too large.");
        return;
      }
      emitByte(compiler, OP_MAP, noToken());
      emitShort(compiler, (uint16_t)expr->as.map.entries.count, noToken());
      for (int i = 0; i < expr->as.map.entries.count; i++) {
        compileExpr(compiler, expr->as.map.entries.entries[i].key);
        compileExpr(compiler, expr->as.map.entries.entries[i].value);
        emitByte(compiler, OP_MAP_SET, noToken());
      }
      break;
    }
    case EXPR_INDEX: {
      compileExpr(compiler, expr->as.index.object);
      compileExpr(compiler, expr->as.index.index);
      emitByte(compiler, OP_GET_INDEX, expr->as.index.bracket);
      break;
    }
    case EXPR_SET_INDEX: {
      compileExpr(compiler, expr->as.setIndex.object);
      compileExpr(compiler, expr->as.setIndex.index);
      compileExpr(compiler, expr->as.setIndex.value);
      emitByte(compiler, OP_SET_INDEX, expr->as.setIndex.equals);
      break;
    }
  }
}

static ObjFunction* compileFunction(Compiler* compiler, Stmt* stmt, bool isInitializer) {
  VM* vm = compiler->vm;
  ObjString* name = stringFromToken(vm, stmt->as.function.name);
  int arity = stmt->as.function.params.count;
  int minArity = arity;
  for (int i = 0; i < arity; i++) {
    if (stmt->as.function.params.items[i].defaultValue) {
      minArity = i;
      break;
    }
  }
  ObjString** params = NULL;
  if (arity > 0) {
    params = (ObjString**)malloc(sizeof(ObjString*) * (size_t)arity);
    if (!params) {
      fprintf(stderr, "Out of memory.\n");
      exit(1);
    }
    for (int i = 0; i < arity; i++) {
      params[i] = stringFromToken(vm, stmt->as.function.params.items[i].name);
    }
  }

  Chunk* chunk = (Chunk*)malloc(sizeof(Chunk));
  if (!chunk) {
    fprintf(stderr, "Out of memory.\n");
    exit(1);
  }
  initChunk(chunk);

  ObjFunction* function = newFunction(vm, name, arity, minArity, isInitializer, params, chunk,
                                      NULL, compiler->program);

  Compiler fnCompiler;
  fnCompiler.vm = compiler->vm;
  fnCompiler.program = compiler->program;
  fnCompiler.chunk = chunk;
  fnCompiler.hadError = false;
  fnCompiler.scopeDepth = 0;
  fnCompiler.tempIndex = 0;
  fnCompiler.breakContext = NULL;

  for (int i = 0; i < arity; i++) {
    Param* param = &stmt->as.function.params.items[i];
    if (!param->defaultValue) continue;

    emitByte(&fnCompiler, OP_ARG_COUNT, param->name);
    emitConstant(&fnCompiler, NUMBER_VAL((double)(i + 1)), param->name);
    emitByte(&fnCompiler, OP_LESS, param->name);
    int skipJump = emitJump(&fnCompiler, OP_JUMP_IF_FALSE, param->name);
    emitByte(&fnCompiler, OP_POP, noToken());
    compileExpr(&fnCompiler, param->defaultValue);
    int nameIndex = emitStringConstant(&fnCompiler, param->name);
    emitByte(&fnCompiler, OP_SET_VAR, param->name);
    emitShort(&fnCompiler, (uint16_t)nameIndex, param->name);
    emitByte(&fnCompiler, OP_POP, noToken());
    int endJump = emitJump(&fnCompiler, OP_JUMP, param->name);
    patchJump(&fnCompiler, skipJump, param->name);
    emitByte(&fnCompiler, OP_POP, noToken());
    patchJump(&fnCompiler, endJump, param->name);
    emitGc(&fnCompiler);
  }

  for (int i = 0; i < stmt->as.function.body.count; i++) {
    compileStmt(&fnCompiler, stmt->as.function.body.items[i]);
  }

  emitByte(&fnCompiler, OP_NULL, noToken());
  emitByte(&fnCompiler, OP_RETURN, noToken());

  if (fnCompiler.hadError) {
    compiler->hadError = true;
    return NULL;
  }

  optimizeChunk(chunk);
  return function;
}

static void compileStmt(Compiler* compiler, Stmt* stmt) {
  if (!stmt || compiler->hadError) return;

  switch (stmt->type) {
    case STMT_EXPR:
      compileExpr(compiler, stmt->as.expr.expression);
      emitByte(compiler, OP_POP, noToken());
      emitGc(compiler);
      break;
    case STMT_VAR: {
      if (stmt->as.var.initializer) {
        compileExpr(compiler, stmt->as.var.initializer);
      } else {
        emitByte(compiler, OP_NULL, noToken());
      }
      int name = emitStringConstant(compiler, stmt->as.var.name);
      emitByte(compiler, OP_DEFINE_VAR, stmt->as.var.name);
      emitShort(compiler, (uint16_t)name, stmt->as.var.name);
      emitGc(compiler);
      break;
    }
    case STMT_BLOCK: {
      emitByte(compiler, OP_BEGIN_SCOPE, noToken());
      compiler->scopeDepth++;
      for (int i = 0; i < stmt->as.block.statements.count; i++) {
        compileStmt(compiler, stmt->as.block.statements.items[i]);
      }
      emitByte(compiler, OP_END_SCOPE, noToken());
      compiler->scopeDepth--;
      emitGc(compiler);
      break;
    }
    case STMT_IF: {
      ConstValue folded;
      memset(&folded, 0, sizeof(ConstValue));
      if (evalConstExpr(stmt->as.ifStmt.condition, &folded)) {
        bool truthy = constValueIsTruthy(&folded);
        constValueFree(&folded);
        if (truthy) {
          compileStmt(compiler, stmt->as.ifStmt.thenBranch);
        } else if (stmt->as.ifStmt.elseBranch) {
          compileStmt(compiler, stmt->as.ifStmt.elseBranch);
        }
        emitGc(compiler);
        break;
      }
      compileExpr(compiler, stmt->as.ifStmt.condition);
      int thenJump = emitJump(compiler, OP_JUMP_IF_FALSE, stmt->as.ifStmt.keyword);
      emitByte(compiler, OP_POP, noToken());
      compileStmt(compiler, stmt->as.ifStmt.thenBranch);
      if (stmt->as.ifStmt.elseBranch) {
        int elseJump = emitJump(compiler, OP_JUMP, stmt->as.ifStmt.keyword);
        patchJump(compiler, thenJump, stmt->as.ifStmt.keyword);
        emitByte(compiler, OP_POP, noToken());
        compileStmt(compiler, stmt->as.ifStmt.elseBranch);
        patchJump(compiler, elseJump, stmt->as.ifStmt.keyword);
      } else {
        patchJump(compiler, thenJump, stmt->as.ifStmt.keyword);
        emitByte(compiler, OP_POP, noToken());
      }
      emitGc(compiler);
      break;
    }
    case STMT_WHILE: {
      ConstValue folded;
      memset(&folded, 0, sizeof(ConstValue));
      if (evalConstExpr(stmt->as.whileStmt.condition, &folded)) {
        bool truthy = constValueIsTruthy(&folded);
        constValueFree(&folded);
        if (!truthy) {
          emitGc(compiler);
          break;
        }
      }
      int loopStart = compiler->chunk->count;
      compileExpr(compiler, stmt->as.whileStmt.condition);
      int exitJump = emitJump(compiler, OP_JUMP_IF_FALSE, stmt->as.whileStmt.keyword);
      emitByte(compiler, OP_POP, noToken());
      BreakContext loop;
      loop.type = BREAK_LOOP;
      loop.enclosing = compiler->breakContext;
      loop.scopeDepth = compiler->scopeDepth;
      initJumpList(&loop.breaks);
      initJumpList(&loop.continues);
      compiler->breakContext = &loop;
      compileStmt(compiler, stmt->as.whileStmt.body);
      int continueTarget = compiler->chunk->count;
      emitGc(compiler);
      emitLoop(compiler, loopStart, stmt->as.whileStmt.keyword);
      compiler->breakContext = loop.enclosing;
      patchJump(compiler, exitJump, stmt->as.whileStmt.keyword);
      emitByte(compiler, OP_POP, noToken());
      emitGc(compiler);
      int loopEnd = compiler->chunk->count;
      patchJumpList(compiler, &loop.breaks, loopEnd, stmt->as.whileStmt.keyword);
      patchJumpList(compiler, &loop.continues, continueTarget, stmt->as.whileStmt.keyword);
      freeJumpList(&loop.breaks);
      freeJumpList(&loop.continues);
      break;
    }
    case STMT_FOR: {
      emitByte(compiler, OP_BEGIN_SCOPE, noToken());
      compiler->scopeDepth++;

      if (stmt->as.forStmt.initializer) {
        compileStmt(compiler, stmt->as.forStmt.initializer);
      }

      int loopStart = compiler->chunk->count;
      int exitJump = -1;
      if (stmt->as.forStmt.condition) {
        compileExpr(compiler, stmt->as.forStmt.condition);
        exitJump = emitJump(compiler, OP_JUMP_IF_FALSE, stmt->as.forStmt.keyword);
        emitByte(compiler, OP_POP, noToken());
      }

      BreakContext loop;
      loop.type = BREAK_LOOP;
      loop.enclosing = compiler->breakContext;
      loop.scopeDepth = compiler->scopeDepth;
      initJumpList(&loop.breaks);
      initJumpList(&loop.continues);
      compiler->breakContext = &loop;

      compileStmt(compiler, stmt->as.forStmt.body);

      int continueTarget = compiler->chunk->count;
      if (stmt->as.forStmt.increment) {
        compileExpr(compiler, stmt->as.forStmt.increment);
        emitByte(compiler, OP_POP, noToken());
      }
      emitGc(compiler);
      emitLoop(compiler, loopStart, stmt->as.forStmt.keyword);
      compiler->breakContext = loop.enclosing;

      if (exitJump != -1) {
        patchJump(compiler, exitJump, stmt->as.forStmt.keyword);
        emitByte(compiler, OP_POP, noToken());
      }
      emitGc(compiler);

      int loopEnd = compiler->chunk->count;
      patchJumpList(compiler, &loop.breaks, loopEnd, stmt->as.forStmt.keyword);
      patchJumpList(compiler, &loop.continues, continueTarget, stmt->as.forStmt.keyword);
      freeJumpList(&loop.breaks);
      freeJumpList(&loop.continues);

      emitByte(compiler, OP_END_SCOPE, noToken());
      compiler->scopeDepth--;
      emitGc(compiler);
      break;
    }
    case STMT_FOREACH: {
      emitByte(compiler, OP_BEGIN_SCOPE, noToken());
      compiler->scopeDepth++;

      int iterName = emitTempNameConstant(compiler, "iter");
      compileExpr(compiler, stmt->as.foreachStmt.iterable);
      emitDefineVarConstant(compiler, iterName);

      int collectionName = iterName;
      if (stmt->as.foreachStmt.hasKey) {
        int keysFn = emitStringConstantFromChars(compiler, "keys", 4);
        emitGetVarConstant(compiler, keysFn);
        emitGetVarConstant(compiler, iterName);
        emitByte(compiler, OP_CALL, noToken());
        emitByte(compiler, 1, noToken());
        int keysName = emitTempNameConstant(compiler, "keys");
        emitDefineVarConstant(compiler, keysName);
        collectionName = keysName;
      }

      int indexName = emitTempNameConstant(compiler, "i");
      emitConstant(compiler, NUMBER_VAL(0), noToken());
      emitDefineVarConstant(compiler, indexName);

      int lenFn = emitStringConstantFromChars(compiler, "len", 3);
      int loopStart = compiler->chunk->count;
      emitGetVarConstant(compiler, indexName);
      emitGetVarConstant(compiler, lenFn);
      emitGetVarConstant(compiler, collectionName);
      emitByte(compiler, OP_CALL, noToken());
      emitByte(compiler, 1, noToken());
      emitByte(compiler, OP_LESS, stmt->as.foreachStmt.keyword);
      int exitJump = emitJump(compiler, OP_JUMP_IF_FALSE, stmt->as.foreachStmt.keyword);
      emitByte(compiler, OP_POP, noToken());

      BreakContext loop;
      loop.type = BREAK_LOOP;
      loop.enclosing = compiler->breakContext;
      loop.scopeDepth = compiler->scopeDepth;
      initJumpList(&loop.breaks);
      initJumpList(&loop.continues);
      compiler->breakContext = &loop;

      if (stmt->as.foreachStmt.hasKey) {
        int keyName = emitStringConstant(compiler, stmt->as.foreachStmt.key);
        int valueName = emitStringConstant(compiler, stmt->as.foreachStmt.value);
        emitGetVarConstant(compiler, collectionName);
        emitGetVarConstant(compiler, indexName);
        emitByte(compiler, OP_GET_INDEX, stmt->as.foreachStmt.key);
        emitByte(compiler, OP_DEFINE_VAR, stmt->as.foreachStmt.key);
        emitShort(compiler, (uint16_t)keyName, stmt->as.foreachStmt.key);

        emitGetVarConstant(compiler, iterName);
        emitByte(compiler, OP_GET_VAR, stmt->as.foreachStmt.key);
        emitShort(compiler, (uint16_t)keyName, stmt->as.foreachStmt.key);
        emitByte(compiler, OP_GET_INDEX, stmt->as.foreachStmt.value);
        emitByte(compiler, OP_DEFINE_VAR, stmt->as.foreachStmt.value);
        emitShort(compiler, (uint16_t)valueName, stmt->as.foreachStmt.value);
      } else {
        int valueName = emitStringConstant(compiler, stmt->as.foreachStmt.value);
        emitGetVarConstant(compiler, iterName);
        emitGetVarConstant(compiler, indexName);
        emitByte(compiler, OP_GET_INDEX, stmt->as.foreachStmt.value);
        emitByte(compiler, OP_DEFINE_VAR, stmt->as.foreachStmt.value);
        emitShort(compiler, (uint16_t)valueName, stmt->as.foreachStmt.value);
      }

      compileStmt(compiler, stmt->as.foreachStmt.body);

      int continueTarget = compiler->chunk->count;
      emitGetVarConstant(compiler, indexName);
      emitConstant(compiler, NUMBER_VAL(1), noToken());
      emitByte(compiler, OP_ADD, noToken());
      emitSetVarConstant(compiler, indexName);
      emitByte(compiler, OP_POP, noToken());
      emitGc(compiler);
      emitLoop(compiler, loopStart, stmt->as.foreachStmt.keyword);
      compiler->breakContext = loop.enclosing;

      patchJump(compiler, exitJump, stmt->as.foreachStmt.keyword);
      emitByte(compiler, OP_POP, noToken());
      emitGc(compiler);

      int loopEnd = compiler->chunk->count;
      patchJumpList(compiler, &loop.breaks, loopEnd, stmt->as.foreachStmt.keyword);
      patchJumpList(compiler, &loop.continues, continueTarget, stmt->as.foreachStmt.keyword);
      freeJumpList(&loop.breaks);
      freeJumpList(&loop.continues);

      emitByte(compiler, OP_END_SCOPE, noToken());
      compiler->scopeDepth--;
      emitGc(compiler);
      break;
    }
    case STMT_SWITCH: {
      emitByte(compiler, OP_BEGIN_SCOPE, noToken());
      compiler->scopeDepth++;

      int switchValue = emitTempNameConstant(compiler, "switch");
      compileExpr(compiler, stmt->as.switchStmt.value);
      emitDefineVarConstant(compiler, switchValue);

      BreakContext ctx;
      ctx.type = BREAK_SWITCH;
      ctx.enclosing = compiler->breakContext;
      ctx.scopeDepth = compiler->scopeDepth;
      initJumpList(&ctx.breaks);
      initJumpList(&ctx.continues);
      compiler->breakContext = &ctx;

      JumpList endJumps;
      initJumpList(&endJumps);

      int previousJump = -1;
      for (int i = 0; i < stmt->as.switchStmt.cases.count; i++) {
        SwitchCase* caseEntry = &stmt->as.switchStmt.cases.items[i];
        if (previousJump != -1) {
          patchJump(compiler, previousJump, stmt->as.switchStmt.keyword);
          emitByte(compiler, OP_POP, noToken());
        }

        emitGetVarConstant(compiler, switchValue);
        compileExpr(compiler, caseEntry->value);
        emitByte(compiler, OP_EQUAL, stmt->as.switchStmt.keyword);
        previousJump = emitJump(compiler, OP_JUMP_IF_FALSE, stmt->as.switchStmt.keyword);
        emitByte(compiler, OP_POP, noToken());

        for (int j = 0; j < caseEntry->statements.count; j++) {
          compileStmt(compiler, caseEntry->statements.items[j]);
        }

        int endJump = emitJump(compiler, OP_JUMP, stmt->as.switchStmt.keyword);
        writeJumpList(&endJumps, endJump);
      }

      if (previousJump != -1) {
        patchJump(compiler, previousJump, stmt->as.switchStmt.keyword);
        emitByte(compiler, OP_POP, noToken());
      }

      if (stmt->as.switchStmt.hasDefault) {
        for (int i = 0; i < stmt->as.switchStmt.defaultStatements.count; i++) {
          compileStmt(compiler, stmt->as.switchStmt.defaultStatements.items[i]);
        }
      }

      compiler->breakContext = ctx.enclosing;

      int switchEnd = compiler->chunk->count;
      patchJumpList(compiler, &endJumps, switchEnd, stmt->as.switchStmt.keyword);
      patchJumpList(compiler, &ctx.breaks, switchEnd, stmt->as.switchStmt.keyword);
      freeJumpList(&endJumps);
      freeJumpList(&ctx.breaks);
      freeJumpList(&ctx.continues);

      emitByte(compiler, OP_END_SCOPE, noToken());
      compiler->scopeDepth--;
      emitGc(compiler);
      break;
    }
    case STMT_BREAK: {
      if (!compiler->breakContext) {
        compilerErrorAt(compiler, stmt->as.breakStmt.keyword,
                        "Cannot use 'break' outside of a loop or switch.");
        break;
      }
      emitScopeExits(compiler, compiler->breakContext->scopeDepth);
      int jump = emitJump(compiler, OP_JUMP, stmt->as.breakStmt.keyword);
      writeJumpList(&compiler->breakContext->breaks, jump);
      break;
    }
    case STMT_CONTINUE: {
      BreakContext* loop = findLoopContext(compiler);
      if (!loop) {
        compilerErrorAt(compiler, stmt->as.continueStmt.keyword,
                        "Cannot use 'continue' outside of a loop.");
        break;
      }
      emitScopeExits(compiler, loop->scopeDepth);
      int jump = emitJump(compiler, OP_JUMP, stmt->as.continueStmt.keyword);
      writeJumpList(&loop->continues, jump);
      break;
    }
    case STMT_IMPORT: {
      compileExpr(compiler, stmt->as.importStmt.path);
      emitByte(compiler, OP_IMPORT, stmt->as.importStmt.keyword);
      emitByte(compiler, stmt->as.importStmt.hasAlias ? 1 : 0, stmt->as.importStmt.keyword);
      uint16_t alias = 0;
      if (stmt->as.importStmt.hasAlias) {
        alias = (uint16_t)emitStringConstant(compiler, stmt->as.importStmt.alias);
      }
      emitShort(compiler, alias, stmt->as.importStmt.keyword);
      emitGc(compiler);
      break;
    }
    case STMT_FUNCTION: {
      ObjFunction* function = compileFunction(compiler, stmt, false);
      if (!function) return;
      int constant = makeConstant(compiler, OBJ_VAL(function), stmt->as.function.name);
      emitByte(compiler, OP_CLOSURE, stmt->as.function.name);
      emitShort(compiler, (uint16_t)constant, stmt->as.function.name);
      int name = emitStringConstant(compiler, stmt->as.function.name);
      emitByte(compiler, OP_DEFINE_VAR, stmt->as.function.name);
      emitShort(compiler, (uint16_t)name, stmt->as.function.name);
      emitGc(compiler);
      break;
    }
    case STMT_RETURN: {
      if (stmt->as.ret.value) {
        compileExpr(compiler, stmt->as.ret.value);
      } else {
        emitByte(compiler, OP_NULL, noToken());
      }
      emitByte(compiler, OP_RETURN, stmt->as.ret.keyword);
      break;
    }
    case STMT_CLASS: {
      int nameConst = emitStringConstant(compiler, stmt->as.classStmt.name);
      emitByte(compiler, OP_NULL, noToken());
      emitByte(compiler, OP_DEFINE_VAR, stmt->as.classStmt.name);
      emitShort(compiler, (uint16_t)nameConst, stmt->as.classStmt.name);

      for (int i = 0; i < stmt->as.classStmt.methods.count; i++) {
        Stmt* methodStmt = stmt->as.classStmt.methods.items[i];
        bool isInitializer = methodStmt->as.function.name.length == 4 &&
                             memcmp(methodStmt->as.function.name.start, "init", 4) == 0;
        ObjFunction* method = compileFunction(compiler, methodStmt, isInitializer);
        if (!method) return;
        int constant = makeConstant(compiler, OBJ_VAL(method), methodStmt->as.function.name);
        emitByte(compiler, OP_CLOSURE, methodStmt->as.function.name);
        emitShort(compiler, (uint16_t)constant, methodStmt->as.function.name);
      }

      emitByte(compiler, OP_CLASS, stmt->as.classStmt.name);
      emitShort(compiler, (uint16_t)nameConst, stmt->as.classStmt.name);
      emitShort(compiler, (uint16_t)stmt->as.classStmt.methods.count, stmt->as.classStmt.name);
      emitGc(compiler);
      break;
    }
  }
}

ObjFunction* compileProgram(VM* vm, Program* program) {
  Chunk* chunk = (Chunk*)malloc(sizeof(Chunk));
  if (!chunk) {
    fprintf(stderr, "Out of memory.\n");
    exit(1);
  }
  initChunk(chunk);

  ObjFunction* function = newFunction(vm, NULL, 0, 0, false, NULL, chunk, vm->env, program);

  Compiler compiler;
  compiler.vm = vm;
  compiler.program = program;
  compiler.chunk = chunk;
  compiler.hadError = false;
  compiler.scopeDepth = 0;
  compiler.tempIndex = 0;
  compiler.breakContext = NULL;

  for (int i = 0; i < program->statements.count; i++) {
    compileStmt(&compiler, program->statements.items[i]);
  }

  emitByte(&compiler, OP_NULL, noToken());
  emitByte(&compiler, OP_RETURN, noToken());

  if (compiler.hadError) {
    return NULL;
  }

  optimizeChunk(chunk);
  return function;
}

static int opcodeSize(uint8_t opcode) {
  switch (opcode) {
    case OP_CONSTANT:
    case OP_GET_VAR:
    case OP_SET_VAR:
    case OP_DEFINE_VAR:
    case OP_GET_PROPERTY:
    case OP_SET_PROPERTY:
    case OP_GET_THIS:
    case OP_JUMP:
    case OP_JUMP_IF_FALSE:
    case OP_LOOP:
    case OP_CLOSURE:
    case OP_ARRAY:
    case OP_MAP:
      return 3;
    case OP_CALL:
      return 2;
    case OP_CLASS:
      return 5;
    case OP_IMPORT:
      return 4;
    case OP_ARG_COUNT:
    default:
      return 1;
  }
}

static uint16_t readShort(const uint8_t* code, int offset) {
  return (uint16_t)((code[offset + 1] << 8) | code[offset + 2]);
}

static void markReachable(const Chunk* chunk, const bool* isStart,
                          bool* reachable, bool* isTarget) {
  if (chunk->count == 0) return;

  int* stack = (int*)malloc(sizeof(int) * (size_t)chunk->count);
  if (!stack) {
    fprintf(stderr, "Out of memory.\n");
    exit(1);
  }
  int stackCount = 0;
  reachable[0] = true;
  stack[stackCount++] = 0;

  while (stackCount > 0) {
    int offset = stack[--stackCount];
    uint8_t opcode = chunk->code[offset];
    int size = opcodeSize(opcode);
    int next = offset + size;

    switch (opcode) {
      case OP_JUMP: {
        int target = next + (int)readShort(chunk->code, offset);
        if (target >= 0 && target < chunk->count && isStart[target]) {
          isTarget[target] = true;
          if (!reachable[target]) {
            reachable[target] = true;
            stack[stackCount++] = target;
          }
        }
        break;
      }
      case OP_JUMP_IF_FALSE: {
        int target = next + (int)readShort(chunk->code, offset);
        if (next < chunk->count && isStart[next] && !reachable[next]) {
          reachable[next] = true;
          stack[stackCount++] = next;
        }
        if (target >= 0 && target < chunk->count && isStart[target]) {
          isTarget[target] = true;
          if (!reachable[target]) {
            reachable[target] = true;
            stack[stackCount++] = target;
          }
        }
        break;
      }
      case OP_LOOP: {
        int target = next - (int)readShort(chunk->code, offset);
        if (target >= 0 && target < chunk->count && isStart[target]) {
          isTarget[target] = true;
          if (!reachable[target]) {
            reachable[target] = true;
            stack[stackCount++] = target;
          }
        }
        break;
      }
      case OP_RETURN:
        break;
      default:
        if (next < chunk->count && isStart[next] && !reachable[next]) {
          reachable[next] = true;
          stack[stackCount++] = next;
        }
        break;
    }
  }

  free(stack);
}

static void peepholePass(const Chunk* chunk, const bool* reachable,
                         const bool* isTarget, bool* remove) {
  for (int offset = 0; offset < chunk->count;) {
    uint8_t opcode = chunk->code[offset];
    int size = opcodeSize(opcode);

    if (!reachable[offset]) {
      offset += size;
      continue;
    }

    if (!isTarget[offset] &&
        (opcode == OP_NULL || opcode == OP_TRUE || opcode == OP_FALSE ||
         opcode == OP_CONSTANT)) {
      int next = offset + size;
      if (next < chunk->count && reachable[next] && !isTarget[next] &&
          chunk->code[next] == OP_POP) {
        remove[offset] = true;
        remove[next] = true;
      }
    }

    offset += size;
  }
}

static void optimizeChunk(Chunk* chunk) {
  if (!chunk || chunk->count == 0) return;

  int count = chunk->count;
  bool* isStart = (bool*)calloc((size_t)count, sizeof(bool));
  bool* reachable = (bool*)calloc((size_t)count, sizeof(bool));
  bool* isTarget = (bool*)calloc((size_t)count, sizeof(bool));
  bool* remove = (bool*)calloc((size_t)count, sizeof(bool));
  int* newOffsets = (int*)malloc(sizeof(int) * (size_t)count);

  if (!isStart || !reachable || !isTarget || !remove || !newOffsets) {
    fprintf(stderr, "Out of memory.\n");
    exit(1);
  }

  for (int offset = 0; offset < count;) {
    isStart[offset] = true;
    offset += opcodeSize(chunk->code[offset]);
  }

  markReachable(chunk, isStart, reachable, isTarget);
  peepholePass(chunk, reachable, isTarget, remove);

  for (int i = 0; i < count; i++) {
    newOffsets[i] = -1;
  }

  int newCount = 0;
  for (int offset = 0; offset < count;) {
    int size = opcodeSize(chunk->code[offset]);
    if (reachable[offset] && !remove[offset]) {
      newOffsets[offset] = newCount;
      newCount += size;
    }
    offset += size;
  }

  if (newCount == count) {
    free(isStart);
    free(reachable);
    free(isTarget);
    free(remove);
    free(newOffsets);
    return;
  }

  uint8_t* newCode = (uint8_t*)malloc((size_t)newCount);
  Token* newTokens = (Token*)malloc(sizeof(Token) * (size_t)newCount);
  InlineCache* newCaches = (InlineCache*)malloc(sizeof(InlineCache) * (size_t)newCount);
  if (!newCode || !newTokens || !newCaches) {
    fprintf(stderr, "Out of memory.\n");
    exit(1);
  }
  memset(newCaches, 0, sizeof(InlineCache) * (size_t)newCount);

  for (int offset = 0; offset < count;) {
    int size = opcodeSize(chunk->code[offset]);
    if (reachable[offset] && !remove[offset]) {
      int dest = newOffsets[offset];
      memcpy(newCode + dest, chunk->code + offset, (size_t)size);
      memcpy(newTokens + dest, chunk->tokens + offset, sizeof(Token) * (size_t)size);
    }
    offset += size;
  }

  for (int offset = 0; offset < count;) {
    int size = opcodeSize(chunk->code[offset]);
    if (reachable[offset] && !remove[offset]) {
      uint8_t opcode = chunk->code[offset];
      if (opcode == OP_JUMP || opcode == OP_JUMP_IF_FALSE || opcode == OP_LOOP) {
        int oldNext = offset + size;
        int oldTarget = 0;
        if (opcode == OP_LOOP) {
          oldTarget = oldNext - (int)readShort(chunk->code, offset);
        } else {
          oldTarget = oldNext + (int)readShort(chunk->code, offset);
        }
        int newOffset = newOffsets[offset];
        int newTarget = newOffsets[oldTarget];
        if (newTarget < 0) {
          fprintf(stderr, "Compile error: invalid jump target after optimization.\n");
          exit(1);
        }
        int newJump = opcode == OP_LOOP
                          ? (newOffset + size - newTarget)
                          : (newTarget - (newOffset + size));
        newCode[newOffset + 1] = (uint8_t)((newJump >> 8) & 0xff);
        newCode[newOffset + 2] = (uint8_t)(newJump & 0xff);
      }
    }
    offset += size;
  }

  uint8_t* oldCode = chunk->code;
  Token* oldTokens = chunk->tokens;
  InlineCache* oldCaches = chunk->caches;
  int oldCapacity = chunk->capacity;

  chunk->code = newCode;
  chunk->tokens = newTokens;
  chunk->caches = newCaches;
  chunk->count = newCount;
  chunk->capacity = newCount;

  FREE_ARRAY(uint8_t, oldCode, oldCapacity);
  FREE_ARRAY(Token, oldTokens, oldCapacity);
  FREE_ARRAY(InlineCache, oldCaches, oldCapacity);

  free(isStart);
  free(reachable);
  free(isTarget);
  free(remove);
  free(newOffsets);
}

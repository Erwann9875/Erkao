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
} Compiler;

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

static void emitGc(Compiler* compiler) {
  emitByte(compiler, OP_GC, noToken());
}

static void compileExpr(Compiler* compiler, Expr* expr);
static void compileStmt(Compiler* compiler, Stmt* stmt);

static void compileExprArray(Compiler* compiler, ExprArray* array) {
  for (int i = 0; i < array->count; i++) {
    compileExpr(compiler, array->items[i]);
  }
}

static void compileExpr(Compiler* compiler, Expr* expr) {
  if (!expr || compiler->hadError) return;

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
      emitByte(compiler, OP_ARRAY, noToken());
      for (int i = 0; i < expr->as.array.elements.count; i++) {
        compileExpr(compiler, expr->as.array.elements.items[i]);
        emitByte(compiler, OP_ARRAY_APPEND, noToken());
      }
      break;
    }
    case EXPR_MAP: {
      emitByte(compiler, OP_MAP, noToken());
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
  ObjString** params = NULL;
  if (arity > 0) {
    params = (ObjString**)malloc(sizeof(ObjString*) * (size_t)arity);
    if (!params) {
      fprintf(stderr, "Out of memory.\n");
      exit(1);
    }
    for (int i = 0; i < arity; i++) {
      params[i] = stringFromToken(vm, stmt->as.function.params.items[i]);
    }
  }

  Chunk* chunk = (Chunk*)malloc(sizeof(Chunk));
  if (!chunk) {
    fprintf(stderr, "Out of memory.\n");
    exit(1);
  }
  initChunk(chunk);

  ObjFunction* function = newFunction(vm, name, arity, isInitializer, params, chunk,
                                      NULL, compiler->program);

  Compiler fnCompiler;
  fnCompiler.vm = compiler->vm;
  fnCompiler.program = compiler->program;
  fnCompiler.chunk = chunk;
  fnCompiler.hadError = false;

  for (int i = 0; i < stmt->as.function.body.count; i++) {
    compileStmt(&fnCompiler, stmt->as.function.body.items[i]);
  }

  emitByte(&fnCompiler, OP_NULL, noToken());
  emitByte(&fnCompiler, OP_RETURN, noToken());

  if (fnCompiler.hadError) {
    compiler->hadError = true;
    return NULL;
  }

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
      for (int i = 0; i < stmt->as.block.statements.count; i++) {
        compileStmt(compiler, stmt->as.block.statements.items[i]);
      }
      emitByte(compiler, OP_END_SCOPE, noToken());
      emitGc(compiler);
      break;
    }
    case STMT_IF: {
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
      int loopStart = compiler->chunk->count;
      compileExpr(compiler, stmt->as.whileStmt.condition);
      int exitJump = emitJump(compiler, OP_JUMP_IF_FALSE, stmt->as.whileStmt.keyword);
      emitByte(compiler, OP_POP, noToken());
      compileStmt(compiler, stmt->as.whileStmt.body);
      emitGc(compiler);
      emitLoop(compiler, loopStart, stmt->as.whileStmt.keyword);
      patchJump(compiler, exitJump, stmt->as.whileStmt.keyword);
      emitByte(compiler, OP_POP, noToken());
      emitGc(compiler);
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

  ObjFunction* function = newFunction(vm, NULL, 0, false, NULL, chunk, vm->env, program);

  Compiler compiler;
  compiler.vm = vm;
  compiler.program = program;
  compiler.chunk = chunk;
  compiler.hadError = false;

  for (int i = 0; i < program->statements.count; i++) {
    compileStmt(&compiler, program->statements.items[i]);
  }

  emitByte(&compiler, OP_NULL, noToken());
  emitByte(&compiler, OP_RETURN, noToken());

  if (compiler.hadError) {
    return NULL;
  }

  return function;
}

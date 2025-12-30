#ifndef ERKAO_SINGLEPASS_H
#define ERKAO_SINGLEPASS_H

#include "value.h"
#include "lexer.h"
#include "chunk.h"

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

typedef struct TypeChecker TypeChecker;
typedef struct EnumInfo EnumInfo;
typedef struct StructInfo StructInfo;

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
  bool pendingOptionalCall;
  bool forbidCall;
  bool lastExprWasVar;
  Token lastExprVar;
  bool hasYield;
  int yieldName;
  int yieldFlagName;
  BreakContext* breakContext;
  struct Compiler* enclosing;
  TypeChecker* typecheck;
  EnumInfo* enums;
  int enumCount;
  int enumCapacity;
  StructInfo* structs;
  int structCount;
  int structCapacity;
} Compiler;

typedef enum {
  PREC_NONE,
  PREC_ASSIGNMENT,
  PREC_OR,
  PREC_AND,
  PREC_EQUALITY,
  PREC_COMPARISON,
  PREC_RANGE,
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

typedef struct {
  ParseRule* rules;
  int count;
} ParserRules;

typedef struct {
  void (*register_rules)(ParserRules* rules);
  bool (*parse_statement)(Compiler* c);
  bool (*parse_expression)(Compiler* c, bool canAssign);
  void (*type_hook)(TypeChecker* tc);
} CompilerPlugin;

void compiler_register_plugin(const CompilerPlugin* plugin);

ObjFunction* compile(VM* vm, const TokenArray* tokens, const char* source,
                     const char* path, bool* hadError);

#endif

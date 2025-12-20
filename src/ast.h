#ifndef ERKAO_AST_H
#define ERKAO_AST_H

#include "common.h"
#include "lexer.h"

typedef struct Expr Expr;
typedef struct Stmt Stmt;

typedef struct {
  Expr** items;
  int count;
  int capacity;
} ExprArray;

typedef struct {
  Stmt** items;
  int count;
  int capacity;
} StmtArray;

typedef struct {
  Token* items;
  int count;
  int capacity;
} ParamArray;

typedef enum {
  LIT_NUMBER,
  LIT_STRING,
  LIT_BOOL,
  LIT_NULL
} LiteralType;

typedef struct {
  LiteralType type;
  union {
    double number;
    char* string;
    bool boolean;
  } as;
} Literal;

typedef struct {
  Expr* key;
  Expr* value;
} MapEntry;

typedef struct {
  MapEntry* entries;
  int count;
  int capacity;
} MapEntryArray;

typedef enum {
  EXPR_LITERAL,
  EXPR_GROUPING,
  EXPR_UNARY,
  EXPR_BINARY,
  EXPR_VARIABLE,
  EXPR_ASSIGN,
  EXPR_LOGICAL,
  EXPR_CALL,
  EXPR_GET,
  EXPR_SET,
  EXPR_THIS,
  EXPR_ARRAY,
  EXPR_MAP,
  EXPR_INDEX,
  EXPR_SET_INDEX
} ExprType;

struct Expr {
  ExprType type;
  union {
    struct {
      Literal literal;
    } literal;
    struct {
      Expr* expression;
    } grouping;
    struct {
      Token op;
      Expr* right;
    } unary;
    struct {
      Expr* left;
      Token op;
      Expr* right;
    } binary;
    struct {
      Token name;
    } variable;
    struct {
      Token name;
      Expr* value;
    } assign;
    struct {
      Expr* left;
      Token op;
      Expr* right;
    } logical;
    struct {
      Expr* callee;
      Token paren;
      ExprArray args;
    } call;
    struct {
      Expr* object;
      Token name;
    } get;
    struct {
      Expr* object;
      Token name;
      Expr* value;
    } set;
    struct {
      Token keyword;
    } thisExpr;
    struct {
      ExprArray elements;
    } array;
    struct {
      MapEntryArray entries;
    } map;
    struct {
      Expr* object;
      Expr* index;
      Token bracket;
    } index;
    struct {
      Expr* object;
      Expr* index;
      Expr* value;
      Token equals;
    } setIndex;
  } as;
};

typedef enum {
  STMT_EXPR,
  STMT_VAR,
  STMT_BLOCK,
  STMT_IF,
  STMT_WHILE,
  STMT_IMPORT,
  STMT_FUNCTION,
  STMT_RETURN,
  STMT_CLASS
} StmtType;

struct Stmt {
  StmtType type;
  union {
    struct {
      Expr* expression;
    } expr;
    struct {
      Token name;
      Expr* initializer;
    } var;
    struct {
      StmtArray statements;
    } block;
    struct {
      Expr* condition;
      Stmt* thenBranch;
      Stmt* elseBranch;
    } ifStmt;
    struct {
      Expr* condition;
      Stmt* body;
    } whileStmt;
    struct {
      Token keyword;
      Expr* path;
    } importStmt;
    struct {
      Token name;
      ParamArray params;
      StmtArray body;
    } function;
    struct {
      Token keyword;
      Expr* value;
    } ret;
    struct {
      Token name;
      StmtArray methods;
    } classStmt;
  } as;
};

void initExprArray(ExprArray* array);
void writeExprArray(ExprArray* array, Expr* expr);
void freeExprArray(ExprArray* array);

void initStmtArray(StmtArray* array);
void writeStmtArray(StmtArray* array, Stmt* stmt);
void freeStmtArray(StmtArray* array);

void initParamArray(ParamArray* array);
void writeParamArray(ParamArray* array, Token param);
void freeParamArray(ParamArray* array);

void initMapEntryArray(MapEntryArray* array);
void writeMapEntryArray(MapEntryArray* array, MapEntry entry);
void freeMapEntryArray(MapEntryArray* array);

Expr* newLiteralExpr(Literal literal);
Expr* newGroupingExpr(Expr* expression);
Expr* newUnaryExpr(Token op, Expr* right);
Expr* newBinaryExpr(Expr* left, Token op, Expr* right);
Expr* newVariableExpr(Token name);
Expr* newAssignExpr(Token name, Expr* value);
Expr* newLogicalExpr(Expr* left, Token op, Expr* right);
Expr* newCallExpr(Expr* callee, Token paren, ExprArray args);
Expr* newGetExpr(Expr* object, Token name);
Expr* newSetExpr(Expr* object, Token name, Expr* value);
Expr* newThisExpr(Token keyword);
Expr* newArrayExpr(ExprArray elements);
Expr* newMapExpr(MapEntryArray entries);
Expr* newIndexExpr(Expr* object, Expr* index, Token bracket);
Expr* newSetIndexExpr(Expr* object, Expr* index, Expr* value, Token equals);

Stmt* newExprStmt(Expr* expression);
Stmt* newVarStmt(Token name, Expr* initializer);
Stmt* newBlockStmt(StmtArray statements);
Stmt* newIfStmt(Expr* condition, Stmt* thenBranch, Stmt* elseBranch);
Stmt* newWhileStmt(Expr* condition, Stmt* body);
Stmt* newImportStmt(Token keyword, Expr* path);
Stmt* newFunctionStmt(Token name, ParamArray params, StmtArray body);
Stmt* newReturnStmt(Token keyword, Expr* value);
Stmt* newClassStmt(Token name, StmtArray methods);

void freeExpr(Expr* expr);
void freeStmt(Stmt* stmt);

#endif

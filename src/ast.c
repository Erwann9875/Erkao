#include "ast.h"

static void* allocateNode(size_t size) {
  void* node = malloc(size);
  if (!node) {
    fprintf(stderr, "Out of memory.\n");
    exit(1);
  }
  memset(node, 0, size);
  return node;
}

void initExprArray(ExprArray* array) {
  array->items = NULL;
  array->count = 0;
  array->capacity = 0;
}

void writeExprArray(ExprArray* array, Expr* expr) {
  if (array->capacity < array->count + 1) {
    int oldCapacity = array->capacity;
    array->capacity = GROW_CAPACITY(oldCapacity);
    array->items = GROW_ARRAY(Expr*, array->items, oldCapacity, array->capacity);
  }
  array->items[array->count++] = expr;
}

void freeExprArray(ExprArray* array) {
  FREE_ARRAY(Expr*, array->items, array->capacity);
  initExprArray(array);
}

void initStmtArray(StmtArray* array) {
  array->items = NULL;
  array->count = 0;
  array->capacity = 0;
}

void writeStmtArray(StmtArray* array, Stmt* stmt) {
  if (array->capacity < array->count + 1) {
    int oldCapacity = array->capacity;
    array->capacity = GROW_CAPACITY(oldCapacity);
    array->items = GROW_ARRAY(Stmt*, array->items, oldCapacity, array->capacity);
  }
  array->items[array->count++] = stmt;
}

void freeStmtArray(StmtArray* array) {
  FREE_ARRAY(Stmt*, array->items, array->capacity);
  initStmtArray(array);
}

void initParamArray(ParamArray* array) {
  array->items = NULL;
  array->count = 0;
  array->capacity = 0;
}

void writeParamArray(ParamArray* array, Param param) {
  if (array->capacity < array->count + 1) {
    int oldCapacity = array->capacity;
    array->capacity = GROW_CAPACITY(oldCapacity);
    array->items = GROW_ARRAY(Param, array->items, oldCapacity, array->capacity);
  }
  array->items[array->count++] = param;
}

void freeParamArray(ParamArray* array) {
  FREE_ARRAY(Param, array->items, array->capacity);
  initParamArray(array);
}

void initMapEntryArray(MapEntryArray* array) {
  array->entries = NULL;
  array->count = 0;
  array->capacity = 0;
}

void writeMapEntryArray(MapEntryArray* array, MapEntry entry) {
  if (array->capacity < array->count + 1) {
    int oldCapacity = array->capacity;
    array->capacity = GROW_CAPACITY(oldCapacity);
    array->entries = GROW_ARRAY(MapEntry, array->entries, oldCapacity, array->capacity);
  }
  array->entries[array->count++] = entry;
}

void freeMapEntryArray(MapEntryArray* array) {
  FREE_ARRAY(MapEntry, array->entries, array->capacity);
  initMapEntryArray(array);
}

void initSwitchCaseArray(SwitchCaseArray* array) {
  array->items = NULL;
  array->count = 0;
  array->capacity = 0;
}

void writeSwitchCaseArray(SwitchCaseArray* array, SwitchCase entry) {
  if (array->capacity < array->count + 1) {
    int oldCapacity = array->capacity;
    array->capacity = GROW_CAPACITY(oldCapacity);
    array->items = GROW_ARRAY(SwitchCase, array->items, oldCapacity, array->capacity);
  }
  array->items[array->count++] = entry;
}

void freeSwitchCaseArray(SwitchCaseArray* array) {
  FREE_ARRAY(SwitchCase, array->items, array->capacity);
  initSwitchCaseArray(array);
}
Expr* newLiteralExpr(Literal literal) {
  Expr* expr = (Expr*)allocateNode(sizeof(Expr));
  expr->type = EXPR_LITERAL;
  expr->as.literal.literal = literal;
  return expr;
}

Expr* newGroupingExpr(Expr* expression) {
  Expr* expr = (Expr*)allocateNode(sizeof(Expr));
  expr->type = EXPR_GROUPING;
  expr->as.grouping.expression = expression;
  return expr;
}

Expr* newUnaryExpr(Token op, Expr* right) {
  Expr* expr = (Expr*)allocateNode(sizeof(Expr));
  expr->type = EXPR_UNARY;
  expr->as.unary.op = op;
  expr->as.unary.right = right;
  return expr;
}

Expr* newBinaryExpr(Expr* left, Token op, Expr* right) {
  Expr* expr = (Expr*)allocateNode(sizeof(Expr));
  expr->type = EXPR_BINARY;
  expr->as.binary.left = left;
  expr->as.binary.op = op;
  expr->as.binary.right = right;
  return expr;
}

Expr* newVariableExpr(Token name) {
  Expr* expr = (Expr*)allocateNode(sizeof(Expr));
  expr->type = EXPR_VARIABLE;
  expr->as.variable.name = name;
  return expr;
}

Expr* newAssignExpr(Token name, Expr* value) {
  Expr* expr = (Expr*)allocateNode(sizeof(Expr));
  expr->type = EXPR_ASSIGN;
  expr->as.assign.name = name;
  expr->as.assign.value = value;
  return expr;
}

Expr* newLogicalExpr(Expr* left, Token op, Expr* right) {
  Expr* expr = (Expr*)allocateNode(sizeof(Expr));
  expr->type = EXPR_LOGICAL;
  expr->as.logical.left = left;
  expr->as.logical.op = op;
  expr->as.logical.right = right;
  return expr;
}

Expr* newCallExpr(Expr* callee, Token paren, ExprArray args) {
  Expr* expr = (Expr*)allocateNode(sizeof(Expr));
  expr->type = EXPR_CALL;
  expr->as.call.callee = callee;
  expr->as.call.paren = paren;
  expr->as.call.args = args;
  return expr;
}

Expr* newGetExpr(Expr* object, Token name) {
  Expr* expr = (Expr*)allocateNode(sizeof(Expr));
  expr->type = EXPR_GET;
  expr->as.get.object = object;
  expr->as.get.name = name;
  return expr;
}

Expr* newSetExpr(Expr* object, Token name, Expr* value) {
  Expr* expr = (Expr*)allocateNode(sizeof(Expr));
  expr->type = EXPR_SET;
  expr->as.set.object = object;
  expr->as.set.name = name;
  expr->as.set.value = value;
  return expr;
}

Expr* newThisExpr(Token keyword) {
  Expr* expr = (Expr*)allocateNode(sizeof(Expr));
  expr->type = EXPR_THIS;
  expr->as.thisExpr.keyword = keyword;
  return expr;
}

Expr* newArrayExpr(ExprArray elements) {
  Expr* expr = (Expr*)allocateNode(sizeof(Expr));
  expr->type = EXPR_ARRAY;
  expr->as.array.elements = elements;
  return expr;
}

Expr* newMapExpr(MapEntryArray entries) {
  Expr* expr = (Expr*)allocateNode(sizeof(Expr));
  expr->type = EXPR_MAP;
  expr->as.map.entries = entries;
  return expr;
}

Expr* newIndexExpr(Expr* object, Expr* index, Token bracket) {
  Expr* expr = (Expr*)allocateNode(sizeof(Expr));
  expr->type = EXPR_INDEX;
  expr->as.index.object = object;
  expr->as.index.index = index;
  expr->as.index.bracket = bracket;
  return expr;
}

Expr* newSetIndexExpr(Expr* object, Expr* index, Expr* value, Token equals) {
  Expr* expr = (Expr*)allocateNode(sizeof(Expr));
  expr->type = EXPR_SET_INDEX;
  expr->as.setIndex.object = object;
  expr->as.setIndex.index = index;
  expr->as.setIndex.value = value;
  expr->as.setIndex.equals = equals;
  return expr;
}

Stmt* newExprStmt(Expr* expression) {
  Stmt* stmt = (Stmt*)allocateNode(sizeof(Stmt));
  stmt->type = STMT_EXPR;
  stmt->as.expr.expression = expression;
  return stmt;
}

Stmt* newVarStmt(Token name, Expr* initializer) {
  Stmt* stmt = (Stmt*)allocateNode(sizeof(Stmt));
  stmt->type = STMT_VAR;
  stmt->as.var.name = name;
  stmt->as.var.initializer = initializer;
  return stmt;
}

Stmt* newBlockStmt(StmtArray statements) {
  Stmt* stmt = (Stmt*)allocateNode(sizeof(Stmt));
  stmt->type = STMT_BLOCK;
  stmt->as.block.statements = statements;
  return stmt;
}

Stmt* newIfStmt(Token keyword, Expr* condition, Stmt* thenBranch, Stmt* elseBranch) {
  Stmt* stmt = (Stmt*)allocateNode(sizeof(Stmt));
  stmt->type = STMT_IF;
  stmt->as.ifStmt.keyword = keyword;
  stmt->as.ifStmt.condition = condition;
  stmt->as.ifStmt.thenBranch = thenBranch;
  stmt->as.ifStmt.elseBranch = elseBranch;
  return stmt;
}

Stmt* newWhileStmt(Token keyword, Expr* condition, Stmt* body) {
  Stmt* stmt = (Stmt*)allocateNode(sizeof(Stmt));
  stmt->type = STMT_WHILE;
  stmt->as.whileStmt.keyword = keyword;
  stmt->as.whileStmt.condition = condition;
  stmt->as.whileStmt.body = body;
  return stmt;
}

Stmt* newForStmt(Token keyword, Stmt* initializer, Expr* condition,
                 Expr* increment, Stmt* body) {
  Stmt* stmt = (Stmt*)allocateNode(sizeof(Stmt));
  stmt->type = STMT_FOR;
  stmt->as.forStmt.keyword = keyword;
  stmt->as.forStmt.initializer = initializer;
  stmt->as.forStmt.condition = condition;
  stmt->as.forStmt.increment = increment;
  stmt->as.forStmt.body = body;
  return stmt;
}

Stmt* newForeachStmt(Token keyword, Token key, Token value, bool hasKey,
                     Expr* iterable, Stmt* body) {
  Stmt* stmt = (Stmt*)allocateNode(sizeof(Stmt));
  stmt->type = STMT_FOREACH;
  stmt->as.foreachStmt.keyword = keyword;
  stmt->as.foreachStmt.key = key;
  stmt->as.foreachStmt.value = value;
  stmt->as.foreachStmt.hasKey = hasKey;
  stmt->as.foreachStmt.iterable = iterable;
  stmt->as.foreachStmt.body = body;
  return stmt;
}

Stmt* newSwitchStmt(Token keyword, Expr* value, SwitchCaseArray cases,
                    StmtArray defaultStatements, bool hasDefault) {
  Stmt* stmt = (Stmt*)allocateNode(sizeof(Stmt));
  stmt->type = STMT_SWITCH;
  stmt->as.switchStmt.keyword = keyword;
  stmt->as.switchStmt.value = value;
  stmt->as.switchStmt.cases = cases;
  stmt->as.switchStmt.defaultStatements = defaultStatements;
  stmt->as.switchStmt.hasDefault = hasDefault;
  return stmt;
}

Stmt* newBreakStmt(Token keyword) {
  Stmt* stmt = (Stmt*)allocateNode(sizeof(Stmt));
  stmt->type = STMT_BREAK;
  stmt->as.breakStmt.keyword = keyword;
  return stmt;
}

Stmt* newContinueStmt(Token keyword) {
  Stmt* stmt = (Stmt*)allocateNode(sizeof(Stmt));
  stmt->type = STMT_CONTINUE;
  stmt->as.continueStmt.keyword = keyword;
  return stmt;
}

Stmt* newImportStmt(Token keyword, Expr* path, Token alias, bool hasAlias) {
  Stmt* stmt = (Stmt*)allocateNode(sizeof(Stmt));
  stmt->type = STMT_IMPORT;
  stmt->as.importStmt.keyword = keyword;
  stmt->as.importStmt.path = path;
  stmt->as.importStmt.alias = alias;
  stmt->as.importStmt.hasAlias = hasAlias;
  return stmt;
}

Stmt* newFunctionStmt(Token name, ParamArray params, StmtArray body) {
  Stmt* stmt = (Stmt*)allocateNode(sizeof(Stmt));
  stmt->type = STMT_FUNCTION;
  stmt->as.function.name = name;
  stmt->as.function.params = params;
  stmt->as.function.body = body;
  return stmt;
}

Stmt* newReturnStmt(Token keyword, Expr* value) {
  Stmt* stmt = (Stmt*)allocateNode(sizeof(Stmt));
  stmt->type = STMT_RETURN;
  stmt->as.ret.keyword = keyword;
  stmt->as.ret.value = value;
  return stmt;
}

Stmt* newClassStmt(Token name, StmtArray methods) {
  Stmt* stmt = (Stmt*)allocateNode(sizeof(Stmt));
  stmt->type = STMT_CLASS;
  stmt->as.classStmt.name = name;
  stmt->as.classStmt.methods = methods;
  return stmt;
}

static void freeLiteral(Literal* literal) {
  if (literal->type == LIT_STRING) {
    free(literal->as.string);
    literal->as.string = NULL;
  }
}

void freeExpr(Expr* expr) {
  if (!expr) return;

  switch (expr->type) {
    case EXPR_LITERAL:
      freeLiteral(&expr->as.literal.literal);
      break;
    case EXPR_GROUPING:
      freeExpr(expr->as.grouping.expression);
      break;
    case EXPR_UNARY:
      freeExpr(expr->as.unary.right);
      break;
    case EXPR_BINARY:
      freeExpr(expr->as.binary.left);
      freeExpr(expr->as.binary.right);
      break;
    case EXPR_VARIABLE:
      break;
    case EXPR_ASSIGN:
      freeExpr(expr->as.assign.value);
      break;
    case EXPR_LOGICAL:
      freeExpr(expr->as.logical.left);
      freeExpr(expr->as.logical.right);
      break;
    case EXPR_CALL:
      freeExpr(expr->as.call.callee);
      for (int i = 0; i < expr->as.call.args.count; i++) {
        freeExpr(expr->as.call.args.items[i]);
      }
      freeExprArray(&expr->as.call.args);
      break;
    case EXPR_GET:
      freeExpr(expr->as.get.object);
      break;
    case EXPR_SET:
      freeExpr(expr->as.set.object);
      freeExpr(expr->as.set.value);
      break;
    case EXPR_THIS:
      break;
    case EXPR_ARRAY:
      for (int i = 0; i < expr->as.array.elements.count; i++) {
        freeExpr(expr->as.array.elements.items[i]);
      }
      freeExprArray(&expr->as.array.elements);
      break;
    case EXPR_MAP:
      for (int i = 0; i < expr->as.map.entries.count; i++) {
        freeExpr(expr->as.map.entries.entries[i].key);
        freeExpr(expr->as.map.entries.entries[i].value);
      }
      freeMapEntryArray(&expr->as.map.entries);
      break;
    case EXPR_INDEX:
      freeExpr(expr->as.index.object);
      freeExpr(expr->as.index.index);
      break;
    case EXPR_SET_INDEX:
      freeExpr(expr->as.setIndex.object);
      freeExpr(expr->as.setIndex.index);
      freeExpr(expr->as.setIndex.value);
      break;
  }

  free(expr);
}

void freeStmt(Stmt* stmt) {
  if (!stmt) return;

  switch (stmt->type) {
    case STMT_EXPR:
      freeExpr(stmt->as.expr.expression);
      break;
    case STMT_VAR:
      freeExpr(stmt->as.var.initializer);
      break;
    case STMT_BLOCK:
      for (int i = 0; i < stmt->as.block.statements.count; i++) {
        freeStmt(stmt->as.block.statements.items[i]);
      }
      freeStmtArray(&stmt->as.block.statements);
      break;
    case STMT_IF:
      freeExpr(stmt->as.ifStmt.condition);
      freeStmt(stmt->as.ifStmt.thenBranch);
      freeStmt(stmt->as.ifStmt.elseBranch);
      break;
    case STMT_WHILE:
      freeExpr(stmt->as.whileStmt.condition);
      freeStmt(stmt->as.whileStmt.body);
      break;
    case STMT_FOR:
      freeStmt(stmt->as.forStmt.initializer);
      freeExpr(stmt->as.forStmt.condition);
      freeExpr(stmt->as.forStmt.increment);
      freeStmt(stmt->as.forStmt.body);
      break;
    case STMT_FOREACH:
      freeExpr(stmt->as.foreachStmt.iterable);
      freeStmt(stmt->as.foreachStmt.body);
      break;
    case STMT_SWITCH:
      freeExpr(stmt->as.switchStmt.value);
      for (int i = 0; i < stmt->as.switchStmt.cases.count; i++) {
        SwitchCase* caseEntry = &stmt->as.switchStmt.cases.items[i];
        freeExpr(caseEntry->value);
        for (int j = 0; j < caseEntry->statements.count; j++) {
          freeStmt(caseEntry->statements.items[j]);
        }
        freeStmtArray(&caseEntry->statements);
      }
      freeSwitchCaseArray(&stmt->as.switchStmt.cases);
      if (stmt->as.switchStmt.hasDefault) {
        for (int i = 0; i < stmt->as.switchStmt.defaultStatements.count; i++) {
          freeStmt(stmt->as.switchStmt.defaultStatements.items[i]);
        }
        freeStmtArray(&stmt->as.switchStmt.defaultStatements);
      } else {
        freeStmtArray(&stmt->as.switchStmt.defaultStatements);
      }
      break;
    case STMT_BREAK:
      break;
    case STMT_CONTINUE:
      break;
    case STMT_IMPORT:
      freeExpr(stmt->as.importStmt.path);
      break;
    case STMT_FUNCTION:
      for (int i = 0; i < stmt->as.function.params.count; i++) {
        freeExpr(stmt->as.function.params.items[i].defaultValue);
      }
      freeParamArray(&stmt->as.function.params);
      for (int i = 0; i < stmt->as.function.body.count; i++) {
        freeStmt(stmt->as.function.body.items[i]);
      }
      freeStmtArray(&stmt->as.function.body);
      break;
    case STMT_RETURN:
      freeExpr(stmt->as.ret.value);
      break;
    case STMT_CLASS:
      for (int i = 0; i < stmt->as.classStmt.methods.count; i++) {
        freeStmt(stmt->as.classStmt.methods.items[i]);
      }
      freeStmtArray(&stmt->as.classStmt.methods);
      break;
  }

  free(stmt);
}

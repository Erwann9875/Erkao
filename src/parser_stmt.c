#include "parser_internal.h"

#include <string.h>

static Stmt* declaration(Parser* parser);
static Stmt* statement(Parser* parser);
static Stmt* classDeclaration(Parser* parser);
static Stmt* functionDeclaration(Parser* parser, const char* kind);
static Stmt* varDeclaration(Parser* parser);
static Stmt* ifStatement(Parser* parser);
static Stmt* whileStatement(Parser* parser);
static Stmt* forStatement(Parser* parser);
static Stmt* foreachStatement(Parser* parser);
static Stmt* switchStatement(Parser* parser);
static Stmt* breakStatement(Parser* parser);
static Stmt* continueStatement(Parser* parser);
static Stmt* importStatement(Parser* parser);
static Stmt* fromImportStatement(Parser* parser);
static Stmt* returnStatement(Parser* parser);
static Stmt* expressionStatement(Parser* parser);
static StmtArray block(Parser* parser);

static Stmt* declaration(Parser* parser) {
  if (match(parser, TOKEN_CLASS)) return classDeclaration(parser);
  if (match(parser, TOKEN_FUN)) return functionDeclaration(parser, "function");
  if (match(parser, TOKEN_LET)) return varDeclaration(parser);
  if (match(parser, TOKEN_IMPORT)) return importStatement(parser);
  if (match(parser, TOKEN_FROM)) return fromImportStatement(parser);

  Stmt* stmt = statement(parser);
  if (parser->panicMode) synchronize(parser);
  return stmt;
}

static Stmt* classDeclaration(Parser* parser) {
  Token name = consume(parser, TOKEN_IDENTIFIER, "Expect class name.");
  consume(parser, TOKEN_LEFT_BRACE, "Expect '{' before class body.");

  StmtArray methods;
  initStmtArray(&methods);

  while (!check(parser, TOKEN_RIGHT_BRACE) && !isAtEnd(parser)) {
    if (!match(parser, TOKEN_FUN)) {
      errorAtCurrent(parser, "Expect 'fun' before method declaration.");
      synchronize(parser);
      break;
    }
    Stmt* method = functionDeclaration(parser, "method");
    if (method) writeStmtArray(&methods, method);
  }

  consume(parser, TOKEN_RIGHT_BRACE, "Expect '}' after class body.");
  return newClassStmt(name, methods);
}

static Stmt* functionDeclaration(Parser* parser, const char* kind) {
  (void)kind;
  Token name = consume(parser, TOKEN_IDENTIFIER, "Expect function name.");
  consume(parser, TOKEN_LEFT_PAREN, "Expect '(' after function name.");

  ParamArray params;
  initParamArray(&params);
  bool sawDefault = false;

  if (!check(parser, TOKEN_RIGHT_PAREN)) {
    do {
      if (params.count >= ERK_MAX_ARGS) {
        errorAtCurrent(parser, "Too many parameters.");
      }
      Token paramName = consume(parser, TOKEN_IDENTIFIER, "Expect parameter name.");
      Expr* defaultValue = NULL;
      if (match(parser, TOKEN_EQUAL)) {
        defaultValue = expression(parser);
        sawDefault = true;
      } else if (sawDefault) {
        errorAt(parser, paramName, "Parameters with defaults must be last.");
      }
      Param param;
      param.name = paramName;
      param.defaultValue = defaultValue;
      writeParamArray(&params, param);
    } while (match(parser, TOKEN_COMMA));
  }

  consume(parser, TOKEN_RIGHT_PAREN, "Expect ')' after parameters.");
  consume(parser, TOKEN_LEFT_BRACE, "Expect '{' before function body.");
  StmtArray body = block(parser);
  return newFunctionStmt(name, params, body);
}

static Stmt* varDeclaration(Parser* parser) {
  Token name = consume(parser, TOKEN_IDENTIFIER, "Expect variable name.");

  Expr* initializer = NULL;
  if (match(parser, TOKEN_EQUAL)) {
    initializer = expression(parser);
  }

  consume(parser, TOKEN_SEMICOLON, "Expect ';' after variable declaration.");
  return newVarStmt(name, initializer);
}

static Stmt* statement(Parser* parser) {
  if (match(parser, TOKEN_IF)) return ifStatement(parser);
  if (match(parser, TOKEN_WHILE)) return whileStatement(parser);
  if (match(parser, TOKEN_FOR)) return forStatement(parser);
  if (match(parser, TOKEN_FOREACH)) return foreachStatement(parser);
  if (match(parser, TOKEN_SWITCH)) return switchStatement(parser);
  if (match(parser, TOKEN_RETURN)) return returnStatement(parser);
  if (match(parser, TOKEN_BREAK)) return breakStatement(parser);
  if (match(parser, TOKEN_CONTINUE)) return continueStatement(parser);
  if (match(parser, TOKEN_LEFT_BRACE)) {
    StmtArray statements = block(parser);
    return newBlockStmt(statements);
  }

  return expressionStatement(parser);
}

static Stmt* ifStatement(Parser* parser) {
  Token keyword = previous(parser);
  consume(parser, TOKEN_LEFT_PAREN, "Expect '(' after 'if'.");
  Expr* condition = expression(parser);
  consume(parser, TOKEN_RIGHT_PAREN, "Expect ')' after if condition.");

  Stmt* thenBranch = statement(parser);
  Stmt* elseBranch = NULL;
  if (match(parser, TOKEN_ELSE)) {
    elseBranch = statement(parser);
  }

  return newIfStmt(keyword, condition, thenBranch, elseBranch);
}

static Stmt* whileStatement(Parser* parser) {
  Token keyword = previous(parser);
  consume(parser, TOKEN_LEFT_PAREN, "Expect '(' after 'while'.");
  Expr* condition = expression(parser);
  consume(parser, TOKEN_RIGHT_PAREN, "Expect ')' after condition.");
  Stmt* body = statement(parser);
  return newWhileStmt(keyword, condition, body);
}

static Stmt* forStatement(Parser* parser) {
  Token keyword = previous(parser);
  consume(parser, TOKEN_LEFT_PAREN, "Expect '(' after 'for'.");

  Stmt* initializer = NULL;
  if (match(parser, TOKEN_SEMICOLON)) {
    initializer = NULL;
  } else if (match(parser, TOKEN_LET)) {
    initializer = varDeclaration(parser);
  } else {
    Expr* initExpr = expression(parser);
    consume(parser, TOKEN_SEMICOLON, "Expect ';' after loop initializer.");
    initializer = newExprStmt(initExpr);
  }

  Expr* condition = NULL;
  if (!check(parser, TOKEN_SEMICOLON)) {
    condition = expression(parser);
  }
  consume(parser, TOKEN_SEMICOLON, "Expect ';' after loop condition.");

  Expr* increment = NULL;
  if (!check(parser, TOKEN_RIGHT_PAREN)) {
    increment = expression(parser);
  }
  consume(parser, TOKEN_RIGHT_PAREN, "Expect ')' after for clauses.");

  Stmt* body = statement(parser);
  return newForStmt(keyword, initializer, condition, increment, body);
}

static Stmt* foreachStatement(Parser* parser) {
  Token keyword = previous(parser);
  consume(parser, TOKEN_LEFT_PAREN, "Expect '(' after 'foreach'.");

  Token first = consume(parser, TOKEN_IDENTIFIER, "Expect loop variable.");
  Token key;
  Token value;
  memset(&key, 0, sizeof(Token));
  value = first;
  bool hasKey = false;

  if (match(parser, TOKEN_COMMA)) {
    key = first;
    value = consume(parser, TOKEN_IDENTIFIER, "Expect value name after ','.");
    hasKey = true;
  }

  consume(parser, TOKEN_IN, "Expect 'in' after foreach variable.");
  Expr* iterable = expression(parser);
  consume(parser, TOKEN_RIGHT_PAREN, "Expect ')' after foreach iterable.");

  Stmt* body = statement(parser);
  return newForeachStmt(keyword, key, value, hasKey, iterable, body);
}

static Stmt* switchStatement(Parser* parser) {
  Token keyword = previous(parser);
  consume(parser, TOKEN_LEFT_PAREN, "Expect '(' after 'switch'.");
  Expr* value = expression(parser);
  consume(parser, TOKEN_RIGHT_PAREN, "Expect ')' after switch value.");
  consume(parser, TOKEN_LEFT_BRACE, "Expect '{' after switch value.");

  SwitchCaseArray cases;
  initSwitchCaseArray(&cases);

  StmtArray defaultStatements;
  initStmtArray(&defaultStatements);
  bool hasDefault = false;

  while (!check(parser, TOKEN_RIGHT_BRACE) && !isAtEnd(parser)) {
    if (match(parser, TOKEN_CASE)) {
      Expr* caseValue = expression(parser);
      consume(parser, TOKEN_COLON, "Expect ':' after case value.");

      StmtArray caseStatements;
      initStmtArray(&caseStatements);
      while (!check(parser, TOKEN_CASE) && !check(parser, TOKEN_DEFAULT) &&
             !check(parser, TOKEN_RIGHT_BRACE) && !isAtEnd(parser)) {
        Stmt* stmt = declaration(parser);
        if (stmt) writeStmtArray(&caseStatements, stmt);
      }

      SwitchCase entry;
      entry.value = caseValue;
      entry.statements = caseStatements;
      writeSwitchCaseArray(&cases, entry);
      continue;
    }

    if (match(parser, TOKEN_DEFAULT)) {
      if (hasDefault) {
        errorAt(parser, previous(parser), "Switch already has a default case.");
      }
      consume(parser, TOKEN_COLON, "Expect ':' after default.");
      hasDefault = true;
      while (!check(parser, TOKEN_CASE) && !check(parser, TOKEN_DEFAULT) &&
             !check(parser, TOKEN_RIGHT_BRACE) && !isAtEnd(parser)) {
        Stmt* stmt = declaration(parser);
        if (stmt) writeStmtArray(&defaultStatements, stmt);
      }
      continue;
    }

    errorAtCurrent(parser, "Expect 'case' or 'default' in switch.");
    synchronize(parser);
    break;
  }

  consume(parser, TOKEN_RIGHT_BRACE, "Expect '}' after switch cases.");
  return newSwitchStmt(keyword, value, cases, defaultStatements, hasDefault);
}

static Stmt* breakStatement(Parser* parser) {
  Token keyword = previous(parser);
  consume(parser, TOKEN_SEMICOLON, "Expect ';' after 'break'.");
  return newBreakStmt(keyword);
}

static Stmt* continueStatement(Parser* parser) {
  Token keyword = previous(parser);
  consume(parser, TOKEN_SEMICOLON, "Expect ';' after 'continue'.");
  return newContinueStmt(keyword);
}

static Stmt* importStatement(Parser* parser) {
  Token keyword = previous(parser);
  Expr* path = expression(parser);
  Token alias;
  memset(&alias, 0, sizeof(Token));
  bool hasAlias = false;
  if (match(parser, TOKEN_AS)) {
    alias = consume(parser, TOKEN_IDENTIFIER, "Expect name after 'as'.");
    hasAlias = true;
  }
  consume(parser, TOKEN_SEMICOLON, "Expect ';' after import.");
  return newImportStmt(keyword, path, alias, hasAlias);
}

static Stmt* fromImportStatement(Parser* parser) {
  Token keyword = previous(parser);
  Expr* path = expression(parser);
  consume(parser, TOKEN_IMPORT, "Expect 'import' after module path.");
  Token alias = consume(parser, TOKEN_IDENTIFIER, "Expect name after 'import'.");
  consume(parser, TOKEN_SEMICOLON, "Expect ';' after import.");
  return newImportStmt(keyword, path, alias, true);
}

static Stmt* returnStatement(Parser* parser) {
  Token keyword = previous(parser);
  Expr* value = NULL;
  if (!check(parser, TOKEN_SEMICOLON)) {
    value = expression(parser);
  }
  consume(parser, TOKEN_SEMICOLON, "Expect ';' after return value.");
  return newReturnStmt(keyword, value);
}

static Stmt* expressionStatement(Parser* parser) {
  Expr* expr = expression(parser);
  consume(parser, TOKEN_SEMICOLON, "Expect ';' after expression.");
  return newExprStmt(expr);
}

static StmtArray block(Parser* parser) {
  StmtArray statements;
  initStmtArray(&statements);

  while (!check(parser, TOKEN_RIGHT_BRACE) && !isAtEnd(parser)) {
    Stmt* decl = declaration(parser);
    if (decl) writeStmtArray(&statements, decl);
  }

  consume(parser, TOKEN_RIGHT_BRACE, "Expect '}' after block.");
  return statements;
}

bool parseTokens(const TokenArray* tokens, const char* source, const char* path,
                 StmtArray* outStatements) {
  Parser parser;
  parser.tokens = tokens;
  parser.source = source;
  parser.path = path;
  parser.current = 0;
  parser.hadError = false;
  parser.panicMode = false;

  initStmtArray(outStatements);

  while (!isAtEnd(&parser)) {
    Stmt* stmt = declaration(&parser);
    if (stmt) writeStmtArray(outStatements, stmt);
  }

  return !parser.hadError;
}

#include "parser_internal.h"

#include <string.h>

static Stmt* declaration(Parser* parser);
static Stmt* statement(Parser* parser);
static Stmt* classDeclaration(Parser* parser);
static Stmt* functionDeclaration(Parser* parser, const char* kind);
static Stmt* varDeclaration(Parser* parser);
static Stmt* ifStatement(Parser* parser);
static Stmt* whileStatement(Parser* parser);
static Stmt* importStatement(Parser* parser);
static Stmt* returnStatement(Parser* parser);
static Stmt* expressionStatement(Parser* parser);
static StmtArray block(Parser* parser);

static Stmt* declaration(Parser* parser) {
  if (match(parser, TOKEN_CLASS)) return classDeclaration(parser);
  if (match(parser, TOKEN_FUN)) return functionDeclaration(parser, "function");
  if (match(parser, TOKEN_LET)) return varDeclaration(parser);
  if (match(parser, TOKEN_IMPORT)) return importStatement(parser);

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

  if (!check(parser, TOKEN_RIGHT_PAREN)) {
    do {
      if (params.count >= ERK_MAX_ARGS) {
        errorAtCurrent(parser, "Too many parameters.");
      }
      Token param = consume(parser, TOKEN_IDENTIFIER, "Expect parameter name.");
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
  if (match(parser, TOKEN_RETURN)) return returnStatement(parser);
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

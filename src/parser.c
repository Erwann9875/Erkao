#include "parser.h"

typedef struct {
  const TokenArray* tokens;
  int current;
  bool hadError;
  bool panicMode;
} Parser;

static bool isAtEnd(Parser* parser) {
  return parser->tokens->tokens[parser->current].type == TOKEN_EOF;
}

static Token peek(Parser* parser) {
  return parser->tokens->tokens[parser->current];
}

static Token previous(Parser* parser) {
  return parser->tokens->tokens[parser->current - 1];
}

static Token advance(Parser* parser) {
  if (!isAtEnd(parser)) parser->current++;
  return previous(parser);
}

static bool check(Parser* parser, ErkaoTokenType type) {
  if (isAtEnd(parser)) return false;
  return peek(parser).type == type;
}

static bool match(Parser* parser, ErkaoTokenType type) {
  if (!check(parser, type)) return false;
  advance(parser);
  return true;
}

static void errorAt(Parser* parser, Token token, const char* message) {
  if (parser->panicMode) return;
  parser->panicMode = true;
  parser->hadError = true;

  fprintf(stderr, "[line %d:%d] Error", token.line, token.column);
  if (token.type == TOKEN_EOF) {
    fprintf(stderr, " at end");
  } else if (token.type == TOKEN_ERROR) {
    // nothing
  } else {
    fprintf(stderr, " at '%.*s'", token.length, token.start);
  }
  fprintf(stderr, ": %s\n", message);
}

static void errorAtCurrent(Parser* parser, const char* message) {
  errorAt(parser, peek(parser), message);
}

static Token consume(Parser* parser, ErkaoTokenType type, const char* message) {
  if (check(parser, type)) return advance(parser);
  errorAtCurrent(parser, message);
  return peek(parser);
}

static void synchronize(Parser* parser) {
  parser->panicMode = false;

  while (!isAtEnd(parser)) {
    if (previous(parser).type == TOKEN_SEMICOLON) return;

    switch (peek(parser).type) {
      case TOKEN_CLASS:
      case TOKEN_FUN:
      case TOKEN_LET:
      case TOKEN_IF:
      case TOKEN_WHILE:
      case TOKEN_RETURN:
        return;
      default:
        break;
    }

    advance(parser);
  }
}

static char* copyTokenLexeme(Token token) {
  char* buffer = (char*)malloc((size_t)token.length + 1);
  if (!buffer) {
    fprintf(stderr, "Out of memory.\n");
    exit(1);
  }
  memcpy(buffer, token.start, (size_t)token.length);
  buffer[token.length] = '\0';
  return buffer;
}

static char* parseStringLiteral(Token token) {
  int length = token.length - 2;
  if (length < 0) length = 0;
  const char* src = token.start + 1;

  char* buffer = (char*)malloc((size_t)length + 1);
  if (!buffer) {
    fprintf(stderr, "Out of memory.\n");
    exit(1);
  }

  int out = 0;
  for (int i = 0; i < length; i++) {
    char c = src[i];
    if (c == '\\' && i + 1 < length) {
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
      buffer[out++] = c;
    }
  }
  buffer[out] = '\0';
  return buffer;
}

static Literal makeNumberLiteral(double number) {
  Literal literal;
  literal.type = LIT_NUMBER;
  literal.as.number = number;
  return literal;
}

static Literal makeStringLiteral(char* string) {
  Literal literal;
  literal.type = LIT_STRING;
  literal.as.string = string;
  return literal;
}

static Literal makeBoolLiteral(bool value) {
  Literal literal;
  literal.type = LIT_BOOL;
  literal.as.boolean = value;
  return literal;
}

static Literal makeNullLiteral(void) {
  Literal literal;
  literal.type = LIT_NULL;
  return literal;
}

static Expr* expression(Parser* parser);
static Stmt* declaration(Parser* parser);
static Stmt* statement(Parser* parser);
static Stmt* classDeclaration(Parser* parser);
static Stmt* functionDeclaration(Parser* parser, const char* kind);
static Stmt* varDeclaration(Parser* parser);
static Stmt* ifStatement(Parser* parser);
static Stmt* whileStatement(Parser* parser);
static Stmt* returnStatement(Parser* parser);
static Stmt* expressionStatement(Parser* parser);
static StmtArray block(Parser* parser);

static Expr* assignment(Parser* parser);
static Expr* orExpr(Parser* parser);
static Expr* andExpr(Parser* parser);
static Expr* equality(Parser* parser);
static Expr* comparison(Parser* parser);
static Expr* term(Parser* parser);
static Expr* factor(Parser* parser);
static Expr* unary(Parser* parser);
static Expr* call(Parser* parser);
static Expr* finishCall(Parser* parser, Expr* callee);
static Expr* primary(Parser* parser);

static Stmt* declaration(Parser* parser) {
  if (match(parser, TOKEN_CLASS)) return classDeclaration(parser);
  if (match(parser, TOKEN_FUN)) return functionDeclaration(parser, "function");
  if (match(parser, TOKEN_LET)) return varDeclaration(parser);

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
  consume(parser, TOKEN_LEFT_PAREN, "Expect '(' after 'if'.");
  Expr* condition = expression(parser);
  consume(parser, TOKEN_RIGHT_PAREN, "Expect ')' after if condition.");

  Stmt* thenBranch = statement(parser);
  Stmt* elseBranch = NULL;
  if (match(parser, TOKEN_ELSE)) {
    elseBranch = statement(parser);
  }

  return newIfStmt(condition, thenBranch, elseBranch);
}

static Stmt* whileStatement(Parser* parser) {
  consume(parser, TOKEN_LEFT_PAREN, "Expect '(' after 'while'.");
  Expr* condition = expression(parser);
  consume(parser, TOKEN_RIGHT_PAREN, "Expect ')' after condition.");
  Stmt* body = statement(parser);
  return newWhileStmt(condition, body);
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

static Expr* expression(Parser* parser) {
  return assignment(parser);
}

static Expr* assignment(Parser* parser) {
  Expr* expr = orExpr(parser);

  if (match(parser, TOKEN_EQUAL)) {
    Token equals = previous(parser);
    Expr* value = assignment(parser);

    if (expr->type == EXPR_VARIABLE) {
      Token name = expr->as.variable.name;
      return newAssignExpr(name, value);
    } else if (expr->type == EXPR_GET) {
      return newSetExpr(expr->as.get.object, expr->as.get.name, value);
    } else if (expr->type == EXPR_INDEX) {
      return newSetIndexExpr(expr->as.index.object, expr->as.index.index, value, equals);
    }

    errorAt(parser, equals, "Invalid assignment target.");
  }

  return expr;
}

static Expr* orExpr(Parser* parser) {
  Expr* expr = andExpr(parser);

  while (match(parser, TOKEN_OR)) {
    Token op = previous(parser);
    Expr* right = andExpr(parser);
    expr = newLogicalExpr(expr, op, right);
  }

  return expr;
}

static Expr* andExpr(Parser* parser) {
  Expr* expr = equality(parser);

  while (match(parser, TOKEN_AND)) {
    Token op = previous(parser);
    Expr* right = equality(parser);
    expr = newLogicalExpr(expr, op, right);
  }

  return expr;
}

static Expr* equality(Parser* parser) {
  Expr* expr = comparison(parser);

  while (match(parser, TOKEN_BANG_EQUAL) || match(parser, TOKEN_EQUAL_EQUAL)) {
    Token op = previous(parser);
    Expr* right = comparison(parser);
    expr = newBinaryExpr(expr, op, right);
  }

  return expr;
}

static Expr* comparison(Parser* parser) {
  Expr* expr = term(parser);

  while (match(parser, TOKEN_GREATER) || match(parser, TOKEN_GREATER_EQUAL) ||
         match(parser, TOKEN_LESS) || match(parser, TOKEN_LESS_EQUAL)) {
    Token op = previous(parser);
    Expr* right = term(parser);
    expr = newBinaryExpr(expr, op, right);
  }

  return expr;
}

static Expr* term(Parser* parser) {
  Expr* expr = factor(parser);

  while (match(parser, TOKEN_MINUS) || match(parser, TOKEN_PLUS)) {
    Token op = previous(parser);
    Expr* right = factor(parser);
    expr = newBinaryExpr(expr, op, right);
  }

  return expr;
}

static Expr* factor(Parser* parser) {
  Expr* expr = unary(parser);

  while (match(parser, TOKEN_SLASH) || match(parser, TOKEN_STAR)) {
    Token op = previous(parser);
    Expr* right = unary(parser);
    expr = newBinaryExpr(expr, op, right);
  }

  return expr;
}

static Expr* unary(Parser* parser) {
  if (match(parser, TOKEN_BANG) || match(parser, TOKEN_MINUS)) {
    Token op = previous(parser);
    Expr* right = unary(parser);
    return newUnaryExpr(op, right);
  }

  return call(parser);
}

static Expr* call(Parser* parser) {
  Expr* expr = primary(parser);

  for (;;) {
    if (match(parser, TOKEN_LEFT_PAREN)) {
      expr = finishCall(parser, expr);
    } else if (match(parser, TOKEN_DOT)) {
      Token name = consume(parser, TOKEN_IDENTIFIER, "Expect property name after '.'.");
      expr = newGetExpr(expr, name);
    } else if (match(parser, TOKEN_LEFT_BRACKET)) {
      Token bracket = previous(parser);
      Expr* index = expression(parser);
      consume(parser, TOKEN_RIGHT_BRACKET, "Expect ']' after index.");
      expr = newIndexExpr(expr, index, bracket);
    } else {
      break;
    }
  }

  return expr;
}

static Expr* finishCall(Parser* parser, Expr* callee) {
  ExprArray args;
  initExprArray(&args);

  if (!check(parser, TOKEN_RIGHT_PAREN)) {
    do {
      if (args.count >= ERK_MAX_ARGS) {
        errorAtCurrent(parser, "Too many arguments.");
      }
      writeExprArray(&args, expression(parser));
    } while (match(parser, TOKEN_COMMA));
  }

  Token paren = consume(parser, TOKEN_RIGHT_PAREN, "Expect ')' after arguments.");
  return newCallExpr(callee, paren, args);
}

static Expr* primary(Parser* parser) {
  if (match(parser, TOKEN_FALSE)) return newLiteralExpr(makeBoolLiteral(false));
  if (match(parser, TOKEN_TRUE)) return newLiteralExpr(makeBoolLiteral(true));
  if (match(parser, TOKEN_NULL)) return newLiteralExpr(makeNullLiteral());

  if (match(parser, TOKEN_NUMBER)) {
    Token token = previous(parser);
    char* temp = copyTokenLexeme(token);
    double value = strtod(temp, NULL);
    free(temp);
    return newLiteralExpr(makeNumberLiteral(value));
  }

  if (match(parser, TOKEN_STRING)) {
    Token token = previous(parser);
    char* value = parseStringLiteral(token);
    return newLiteralExpr(makeStringLiteral(value));
  }

  if (match(parser, TOKEN_IDENTIFIER)) {
    return newVariableExpr(previous(parser));
  }

  if (match(parser, TOKEN_THIS)) {
    return newThisExpr(previous(parser));
  }

  if (match(parser, TOKEN_LEFT_PAREN)) {
    Expr* expr = expression(parser);
    consume(parser, TOKEN_RIGHT_PAREN, "Expect ')' after expression.");
    return newGroupingExpr(expr);
  }

  if (match(parser, TOKEN_LEFT_BRACKET)) {
    ExprArray elements;
    initExprArray(&elements);

    if (!check(parser, TOKEN_RIGHT_BRACKET)) {
      do {
        writeExprArray(&elements, expression(parser));
      } while (match(parser, TOKEN_COMMA));
    }

    consume(parser, TOKEN_RIGHT_BRACKET, "Expect ']' after array literal.");
    return newArrayExpr(elements);
  }

  if (match(parser, TOKEN_LEFT_BRACE)) {
    MapEntryArray entries;
    initMapEntryArray(&entries);

    if (!check(parser, TOKEN_RIGHT_BRACE)) {
      do {
        Expr* keyExpr = NULL;
        if (match(parser, TOKEN_IDENTIFIER)) {
          Token key = previous(parser);
          char* keyName = copyTokenLexeme(key);
          keyExpr = newLiteralExpr(makeStringLiteral(keyName));
        } else if (match(parser, TOKEN_STRING)) {
          Token key = previous(parser);
          char* keyName = parseStringLiteral(key);
          keyExpr = newLiteralExpr(makeStringLiteral(keyName));
        } else {
          errorAtCurrent(parser, "Map keys must be identifiers or strings.");
          break;
        }

        consume(parser, TOKEN_COLON, "Expect ':' after map key.");
        Expr* valueExpr = expression(parser);

        MapEntry entry;
        entry.key = keyExpr;
        entry.value = valueExpr;
        writeMapEntryArray(&entries, entry);
      } while (match(parser, TOKEN_COMMA));
    }

    consume(parser, TOKEN_RIGHT_BRACE, "Expect '}' after map literal.");
    return newMapExpr(entries);
  }

  errorAtCurrent(parser, "Expect expression.");
  return newLiteralExpr(makeNullLiteral());
}

bool parseTokens(const TokenArray* tokens, StmtArray* outStatements) {
  Parser parser;
  parser.tokens = tokens;
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

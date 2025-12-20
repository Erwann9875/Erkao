#include "parser_internal.h"

#include <stdlib.h>

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

Expr* expression(Parser* parser) {
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
  if (!isAtEnd(parser)) advance(parser);
  return newLiteralExpr(makeNullLiteral());
}

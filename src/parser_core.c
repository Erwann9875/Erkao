#include "parser_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool isAtEnd(Parser* parser) {
  return parser->tokens->tokens[parser->current].type == TOKEN_EOF;
}

Token peek(Parser* parser) {
  return parser->tokens->tokens[parser->current];
}

Token previous(Parser* parser) {
  return parser->tokens->tokens[parser->current - 1];
}

Token advance(Parser* parser) {
  if (!isAtEnd(parser)) parser->current++;
  return previous(parser);
}

bool check(Parser* parser, ErkaoTokenType type) {
  if (isAtEnd(parser)) return false;
  return peek(parser).type == type;
}

bool match(Parser* parser, ErkaoTokenType type) {
  if (!check(parser, type)) return false;
  advance(parser);
  return true;
}

static void errorAtInternal(Parser* parser, Token token, const char* message,
                            int underlineLength) {
  if (parser->panicMode) return;
  parser->panicMode = true;
  parser->hadError = true;

  const char* displayPath = parser->path ? parser->path : "<repl>";
  fprintf(stderr, "%s:%d:%d: Error", displayPath, token.line, token.column);
  if (token.type == TOKEN_EOF) {
    fprintf(stderr, " at end");
  } else if (token.type == TOKEN_ERROR) {
    // nothing
  } else {
    fprintf(stderr, " at '%.*s'", token.length, token.start);
  }
  fprintf(stderr, ": %s\n", message);
  int length = underlineLength > 0 ? underlineLength : (token.length > 0 ? token.length : 1);
  printErrorContext(parser->source, token.line, token.column, length);
}

void errorAt(Parser* parser, Token token, const char* message) {
  errorAtInternal(parser, token, message, token.length);
}

void errorAtCurrent(Parser* parser, const char* message) {
  errorAt(parser, peek(parser), message);
}

Token consume(Parser* parser, ErkaoTokenType type, const char* message) {
  if (check(parser, type)) return advance(parser);
  if (type == TOKEN_SEMICOLON && parser->current > 0) {
    Token token = previous(parser);
    if (token.length > 0) {
      token.column += token.length;
    }
    errorAtInternal(parser, token, message, 1);
  } else {
    errorAtCurrent(parser, message);
  }
  return peek(parser);
}

void synchronize(Parser* parser) {
  parser->panicMode = false;

  while (!isAtEnd(parser)) {
    if (previous(parser).type == TOKEN_SEMICOLON) return;

    switch (peek(parser).type) {
      case TOKEN_CLASS:
      case TOKEN_FUN:
      case TOKEN_LET:
      case TOKEN_IMPORT:
      case TOKEN_FROM:
      case TOKEN_IF:
      case TOKEN_WHILE:
      case TOKEN_FOR:
      case TOKEN_FOREACH:
      case TOKEN_SWITCH:
      case TOKEN_RETURN:
      case TOKEN_BREAK:
      case TOKEN_CONTINUE:
      case TOKEN_CASE:
      case TOKEN_DEFAULT:
        return;
      default:
        break;
    }

    advance(parser);
  }
}

char* copyTokenLexeme(Token token) {
  char* buffer = (char*)malloc((size_t)token.length + 1);
  if (!buffer) {
    fprintf(stderr, "Out of memory.\n");
    exit(1);
  }
  memcpy(buffer, token.start, (size_t)token.length);
  buffer[token.length] = '\0';
  return buffer;
}

char* parseStringLiteral(Token token) {
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

Literal makeNumberLiteral(double number) {
  Literal literal;
  literal.type = LIT_NUMBER;
  literal.as.number = number;
  return literal;
}

Literal makeStringLiteral(char* string) {
  Literal literal;
  literal.type = LIT_STRING;
  literal.as.string = string;
  return literal;
}

Literal makeBoolLiteral(bool value) {
  Literal literal;
  literal.type = LIT_BOOL;
  literal.as.boolean = value;
  return literal;
}

Literal makeNullLiteral(void) {
  Literal literal;
  literal.type = LIT_NULL;
  return literal;
}

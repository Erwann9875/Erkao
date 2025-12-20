#ifndef ERKAO_PARSER_INTERNAL_H
#define ERKAO_PARSER_INTERNAL_H

#include "parser.h"

typedef struct {
  const TokenArray* tokens;
  const char* source;
  const char* path;
  int current;
  bool hadError;
  bool panicMode;
} Parser;

bool isAtEnd(Parser* parser);
Token peek(Parser* parser);
Token previous(Parser* parser);
Token advance(Parser* parser);
bool check(Parser* parser, ErkaoTokenType type);
bool match(Parser* parser, ErkaoTokenType type);

void errorAt(Parser* parser, Token token, const char* message);
void errorAtCurrent(Parser* parser, const char* message);
Token consume(Parser* parser, ErkaoTokenType type, const char* message);
void synchronize(Parser* parser);

char* copyTokenLexeme(Token token);
char* parseStringLiteral(Token token);
Literal makeNumberLiteral(double number);
Literal makeStringLiteral(char* string);
Literal makeBoolLiteral(bool value);
Literal makeNullLiteral(void);

Expr* expression(Parser* parser);

#endif

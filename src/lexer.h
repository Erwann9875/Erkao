#ifndef ERKAO_LEXER_H
#define ERKAO_LEXER_H

#include "common.h"

typedef enum {
  // Single-character tokens
  TOKEN_LEFT_PAREN,
  TOKEN_RIGHT_PAREN,
  TOKEN_LEFT_BRACE,
  TOKEN_RIGHT_BRACE,
  TOKEN_LEFT_BRACKET,
  TOKEN_RIGHT_BRACKET,
  TOKEN_COMMA,
  TOKEN_DOT,
  TOKEN_QUESTION_DOT,
  TOKEN_MINUS,
  TOKEN_PLUS,
  TOKEN_SEMICOLON,
  TOKEN_SLASH,
  TOKEN_STAR,
  TOKEN_COLON,

  // One or two character tokens
  TOKEN_BANG,
  TOKEN_BANG_EQUAL,
  TOKEN_EQUAL,
  TOKEN_EQUAL_EQUAL,
  TOKEN_GREATER,
  TOKEN_GREATER_EQUAL,
  TOKEN_LESS,
  TOKEN_LESS_EQUAL,

  // Literals
  TOKEN_IDENTIFIER,
  TOKEN_STRING,
  TOKEN_STRING_SEGMENT,
  TOKEN_NUMBER,
  TOKEN_INTERP_START,
  TOKEN_INTERP_END,

  // Keywords
  TOKEN_AND,
  TOKEN_AS,
  TOKEN_CLASS,
  TOKEN_CONST,
  TOKEN_ELSE,
  TOKEN_ENUM,
  TOKEN_EXPORT,
  TOKEN_FALSE,
  TOKEN_FUN,
  TOKEN_IF,
  TOKEN_IMPORT,
  TOKEN_MATCH,
  TOKEN_NULL,
  TOKEN_OR,
  TOKEN_RETURN,
  TOKEN_THIS,
  TOKEN_TRUE,
  TOKEN_LET,
  TOKEN_WHILE,
  TOKEN_FOR,
  TOKEN_FOREACH,
  TOKEN_SWITCH,
  TOKEN_CASE,
  TOKEN_DEFAULT,
  TOKEN_BREAK,
  TOKEN_CONTINUE,
  TOKEN_IN,
  TOKEN_FROM,

  TOKEN_ERROR,
  TOKEN_EOF
} ErkaoTokenType;

typedef struct {
  ErkaoTokenType type;
  const char* start;
  int length;
  int line;
  int column;
} Token;

typedef struct {
  Token* tokens;
  int count;
  int capacity;
} TokenArray;

void initTokenArray(TokenArray* array);
void writeTokenArray(TokenArray* array, Token token);
void freeTokenArray(TokenArray* array);

TokenArray scanTokens(const char* source, const char* path, bool* hadError);

const char* tokenTypeName(ErkaoTokenType type);

#endif

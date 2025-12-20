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
  TOKEN_NUMBER,

  // Keywords
  TOKEN_AND,
  TOKEN_AS,
  TOKEN_CLASS,
  TOKEN_ELSE,
  TOKEN_FALSE,
  TOKEN_FUN,
  TOKEN_IF,
  TOKEN_IMPORT,
  TOKEN_NULL,
  TOKEN_OR,
  TOKEN_RETURN,
  TOKEN_THIS,
  TOKEN_TRUE,
  TOKEN_LET,
  TOKEN_WHILE,

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

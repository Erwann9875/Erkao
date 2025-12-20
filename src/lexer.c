#include "lexer.h"

typedef struct {
  const char* start;
  const char* current;
  int line;
  int column;
  int startLine;
  int startColumn;
} Scanner;

static void initScanner(Scanner* scanner, const char* source) {
  scanner->start = source;
  scanner->current = source;
  scanner->line = 1;
  scanner->column = 1;
  scanner->startLine = 1;
  scanner->startColumn = 1;
}

void initTokenArray(TokenArray* array) {
  array->tokens = NULL;
  array->count = 0;
  array->capacity = 0;
}

void writeTokenArray(TokenArray* array, Token token) {
  if (array->capacity < array->count + 1) {
    int oldCapacity = array->capacity;
    array->capacity = GROW_CAPACITY(oldCapacity);
    array->tokens = GROW_ARRAY(Token, array->tokens, oldCapacity, array->capacity);
  }
  array->tokens[array->count++] = token;
}

void freeTokenArray(TokenArray* array) {
  FREE_ARRAY(Token, array->tokens, array->capacity);
  initTokenArray(array);
}

static bool isAtEnd(Scanner* scanner) {
  return *scanner->current == '\0';
}

static char advance(Scanner* scanner) {
  char c = *scanner->current++;
  if (c == '\n') {
    scanner->line++;
    scanner->column = 1;
  } else {
    scanner->column++;
  }
  return c;
}

static char peek(Scanner* scanner) {
  return *scanner->current;
}

static char peekNext(Scanner* scanner) {
  if (isAtEnd(scanner)) return '\0';
  return scanner->current[1];
}

static bool match(Scanner* scanner, char expected) {
  if (isAtEnd(scanner)) return false;
  if (*scanner->current != expected) return false;
  advance(scanner);
  return true;
}

static Token makeToken(Scanner* scanner, ErkaoTokenType type) {
  Token token;
  token.type = type;
  token.start = scanner->start;
  token.length = (int)(scanner->current - scanner->start);
  token.line = scanner->startLine;
  token.column = scanner->startColumn;
  return token;
}

static Token errorToken(Scanner* scanner, const char* message) {
  Token token;
  token.type = TOKEN_ERROR;
  token.start = message;
  token.length = (int)strlen(message);
  token.line = scanner->startLine;
  token.column = scanner->startColumn;
  return token;
}

static void skipWhitespace(Scanner* scanner) {
  for (;;) {
    char c = peek(scanner);
    switch (c) {
      case ' ':
      case '\r':
      case '\t':
        advance(scanner);
        break;
      case '\n':
        advance(scanner);
        break;
      case '/':
        if (peekNext(scanner) == '/') {
          while (peek(scanner) != '\n' && !isAtEnd(scanner)) {
            advance(scanner);
          }
        } else if (peekNext(scanner) == '*') {
          advance(scanner);
          advance(scanner);
          while (!isAtEnd(scanner)) {
            if (peek(scanner) == '*' && peekNext(scanner) == '/') {
              advance(scanner);
              advance(scanner);
              break;
            }
            advance(scanner);
          }
        } else {
          return;
        }
        break;
      default:
        return;
    }
  }
}

static bool isDigit(char c) {
  return c >= '0' && c <= '9';
}

static bool isAlpha(char c) {
  return (c >= 'a' && c <= 'z') ||
         (c >= 'A' && c <= 'Z') ||
         c == '_';
}

static ErkaoTokenType checkKeyword(Scanner* scanner, int start, int length,
                                   const char* rest, ErkaoTokenType type) {
  int tokenLength = (int)(scanner->current - scanner->start);
  if (tokenLength == start + length &&
      memcmp(scanner->start + start, rest, length) == 0) {
    return type;
  }
  return TOKEN_IDENTIFIER;
}

static ErkaoTokenType identifierType(Scanner* scanner) {
  switch (scanner->start[0]) {
    case 'a':
      return checkKeyword(scanner, 1, 2, "nd", TOKEN_AND);
    case 'c':
      return checkKeyword(scanner, 1, 4, "lass", TOKEN_CLASS);
    case 'e':
      return checkKeyword(scanner, 1, 3, "lse", TOKEN_ELSE);
    case 'f':
      if ((int)(scanner->current - scanner->start) > 1) {
        switch (scanner->start[1]) {
          case 'a': return checkKeyword(scanner, 2, 3, "lse", TOKEN_FALSE);
          case 'u': return checkKeyword(scanner, 2, 1, "n", TOKEN_FUN);
        }
      }
      break;
    case 'i':
      if ((int)(scanner->current - scanner->start) > 1) {
        switch (scanner->start[1]) {
          case 'f': return checkKeyword(scanner, 1, 1, "f", TOKEN_IF);
          case 'm': return checkKeyword(scanner, 2, 4, "port", TOKEN_IMPORT);
        }
      }
      return TOKEN_IDENTIFIER;
    case 'l':
      return checkKeyword(scanner, 1, 2, "et", TOKEN_LET);
    case 'n':
      return checkKeyword(scanner, 1, 3, "ull", TOKEN_NULL);
    case 'o':
      return checkKeyword(scanner, 1, 1, "r", TOKEN_OR);
    case 'r':
      return checkKeyword(scanner, 1, 5, "eturn", TOKEN_RETURN);
    case 't':
      if ((int)(scanner->current - scanner->start) > 1) {
        switch (scanner->start[1]) {
          case 'h': return checkKeyword(scanner, 2, 2, "is", TOKEN_THIS);
          case 'r': return checkKeyword(scanner, 2, 2, "ue", TOKEN_TRUE);
        }
      }
      break;
    case 'w':
      return checkKeyword(scanner, 1, 4, "hile", TOKEN_WHILE);
  }
  return TOKEN_IDENTIFIER;
}

static Token identifier(Scanner* scanner) {
  while (isAlpha(peek(scanner)) || isDigit(peek(scanner))) {
    advance(scanner);
  }
  return makeToken(scanner, identifierType(scanner));
}

static Token number(Scanner* scanner) {
  while (isDigit(peek(scanner))) advance(scanner);

  if (peek(scanner) == '.' && isDigit(peekNext(scanner))) {
    advance(scanner);
    while (isDigit(peek(scanner))) advance(scanner);
  }

  return makeToken(scanner, TOKEN_NUMBER);
}

static Token string(Scanner* scanner) {
  while (!isAtEnd(scanner) && peek(scanner) != '"') {
    if (peek(scanner) == '\n') {
      return errorToken(scanner, "Unterminated string.");
    }
    if (peek(scanner) == '\\' && peekNext(scanner) != '\0') {
      advance(scanner);
      advance(scanner);
      continue;
    }
    advance(scanner);
  }

  if (isAtEnd(scanner)) return errorToken(scanner, "Unterminated string.");

  advance(scanner);
  return makeToken(scanner, TOKEN_STRING);
}

static Token scanToken(Scanner* scanner) {
  skipWhitespace(scanner);
  scanner->start = scanner->current;
  scanner->startLine = scanner->line;
  scanner->startColumn = scanner->column;

  if (isAtEnd(scanner)) return makeToken(scanner, TOKEN_EOF);

  char c = advance(scanner);

  if (isAlpha(c)) return identifier(scanner);
  if (isDigit(c)) return number(scanner);

  switch (c) {
    case '(':
      return makeToken(scanner, TOKEN_LEFT_PAREN);
    case ')':
      return makeToken(scanner, TOKEN_RIGHT_PAREN);
    case '{':
      return makeToken(scanner, TOKEN_LEFT_BRACE);
    case '}':
      return makeToken(scanner, TOKEN_RIGHT_BRACE);
    case '[':
      return makeToken(scanner, TOKEN_LEFT_BRACKET);
    case ']':
      return makeToken(scanner, TOKEN_RIGHT_BRACKET);
    case ',':
      return makeToken(scanner, TOKEN_COMMA);
    case '.':
      return makeToken(scanner, TOKEN_DOT);
    case '-':
      return makeToken(scanner, TOKEN_MINUS);
    case '+':
      return makeToken(scanner, TOKEN_PLUS);
    case ';':
      return makeToken(scanner, TOKEN_SEMICOLON);
    case '*':
      return makeToken(scanner, TOKEN_STAR);
    case ':':
      return makeToken(scanner, TOKEN_COLON);
    case '!':
      return makeToken(scanner, match(scanner, '=') ? TOKEN_BANG_EQUAL : TOKEN_BANG);
    case '=':
      return makeToken(scanner, match(scanner, '=') ? TOKEN_EQUAL_EQUAL : TOKEN_EQUAL);
    case '<':
      return makeToken(scanner, match(scanner, '=') ? TOKEN_LESS_EQUAL : TOKEN_LESS);
    case '>':
      return makeToken(scanner, match(scanner, '=') ? TOKEN_GREATER_EQUAL : TOKEN_GREATER);
    case '/':
      return makeToken(scanner, TOKEN_SLASH);
    case '"':
      return string(scanner);
  }

  return errorToken(scanner, "Unexpected character.");
}

TokenArray scanTokens(const char* source, bool* hadError) {
  Scanner scanner;
  initScanner(&scanner, source);

  TokenArray array;
  initTokenArray(&array);

  *hadError = false;

  for (;;) {
    Token token = scanToken(&scanner);
    if (token.type == TOKEN_ERROR) {
      fprintf(stderr, "[line %d:%d] Error: %.*s\n",
              token.line, token.column, token.length, token.start);
      printErrorContext(source, token.line, token.column);
      *hadError = true;
      continue;
    }

    writeTokenArray(&array, token);
    if (token.type == TOKEN_EOF) break;
  }

  return array;
}

const char* tokenTypeName(ErkaoTokenType type) {
  switch (type) {
    case TOKEN_LEFT_PAREN: return "LEFT_PAREN";
    case TOKEN_RIGHT_PAREN: return "RIGHT_PAREN";
    case TOKEN_LEFT_BRACE: return "LEFT_BRACE";
    case TOKEN_RIGHT_BRACE: return "RIGHT_BRACE";
    case TOKEN_LEFT_BRACKET: return "LEFT_BRACKET";
    case TOKEN_RIGHT_BRACKET: return "RIGHT_BRACKET";
    case TOKEN_COMMA: return "COMMA";
    case TOKEN_DOT: return "DOT";
    case TOKEN_MINUS: return "MINUS";
    case TOKEN_PLUS: return "PLUS";
    case TOKEN_SEMICOLON: return "SEMICOLON";
    case TOKEN_SLASH: return "SLASH";
    case TOKEN_STAR: return "STAR";
    case TOKEN_COLON: return "COLON";
    case TOKEN_BANG: return "BANG";
    case TOKEN_BANG_EQUAL: return "BANG_EQUAL";
    case TOKEN_EQUAL: return "EQUAL";
    case TOKEN_EQUAL_EQUAL: return "EQUAL_EQUAL";
    case TOKEN_GREATER: return "GREATER";
    case TOKEN_GREATER_EQUAL: return "GREATER_EQUAL";
    case TOKEN_LESS: return "LESS";
    case TOKEN_LESS_EQUAL: return "LESS_EQUAL";
    case TOKEN_IDENTIFIER: return "IDENTIFIER";
    case TOKEN_STRING: return "STRING";
    case TOKEN_NUMBER: return "NUMBER";
    case TOKEN_AND: return "AND";
    case TOKEN_CLASS: return "CLASS";
    case TOKEN_ELSE: return "ELSE";
    case TOKEN_FALSE: return "FALSE";
    case TOKEN_FUN: return "FUN";
    case TOKEN_IF: return "IF";
    case TOKEN_IMPORT: return "IMPORT";
    case TOKEN_NULL: return "NULL";
    case TOKEN_OR: return "OR";
    case TOKEN_RETURN: return "RETURN";
    case TOKEN_THIS: return "THIS";
    case TOKEN_TRUE: return "TRUE";
    case TOKEN_LET: return "LET";
    case TOKEN_WHILE: return "WHILE";
    case TOKEN_ERROR: return "ERROR";
    case TOKEN_EOF: return "EOF";
    default: return "UNKNOWN";
  }
}

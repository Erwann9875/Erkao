#include "lexer.h"

typedef struct {
  const char* start;
  const char* current;
  int line;
  int column;
  int startLine;
  int startColumn;
  bool inString;
  bool stringIsMultiline;
  bool inInterpolation;
  int interpolationDepth;
  bool hasPendingToken;
  Token pendingToken;
} Scanner;

static void initScanner(Scanner* scanner, const char* source) {
  scanner->start = source;
  scanner->current = source;
  scanner->line = 1;
  scanner->column = 1;
  scanner->startLine = 1;
  scanner->startColumn = 1;
  scanner->inString = false;
  scanner->stringIsMultiline = false;
  scanner->inInterpolation = false;
  scanner->interpolationDepth = 0;
  scanner->hasPendingToken = false;
  memset(&scanner->pendingToken, 0, sizeof(Token));
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

static char peekNextNext(Scanner* scanner) {
  if (isAtEnd(scanner)) return '\0';
  if (scanner->current[1] == '\0') return '\0';
  return scanner->current[2];
}

static bool match(Scanner* scanner, char expected) {
  if (isAtEnd(scanner)) return false;
  if (*scanner->current != expected) return false;
  advance(scanner);
  return true;
}

static Token makeTokenFromSpan(const char* start, const char* end,
                               int line, int column, ErkaoTokenType type) {
  Token token;
  token.type = type;
  token.start = start;
  token.length = (int)(end - start);
  token.line = line;
  token.column = column;
  return token;
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
  token.length = (int)(scanner->current - scanner->start);
  if (token.length <= 0) token.length = 1;
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
      if ((int)(scanner->current - scanner->start) > 1) {
        switch (scanner->start[1]) {
          case 'n': return checkKeyword(scanner, 1, 2, "nd", TOKEN_AND);
          case 's': return checkKeyword(scanner, 1, 1, "s", TOKEN_AS);
        }
      }
      return TOKEN_IDENTIFIER;
    case 'b':
      return checkKeyword(scanner, 1, 4, "reak", TOKEN_BREAK);
    case 'c':
      if ((int)(scanner->current - scanner->start) > 1) {
        switch (scanner->start[1]) {
          case 'a': {
            ErkaoTokenType type = checkKeyword(scanner, 1, 3, "ase", TOKEN_CASE);
            if (type != TOKEN_IDENTIFIER) return type;
            return checkKeyword(scanner, 1, 4, "atch", TOKEN_CATCH);
          }
          case 'l': return checkKeyword(scanner, 1, 4, "lass", TOKEN_CLASS);
          case 'o': {
            ErkaoTokenType type = checkKeyword(scanner, 1, 4, "onst", TOKEN_CONST);
            if (type != TOKEN_IDENTIFIER) return type;
            return checkKeyword(scanner, 1, 7, "ontinue", TOKEN_CONTINUE);
          }
        }
      }
      return TOKEN_IDENTIFIER;
    case 'd':
      return checkKeyword(scanner, 1, 6, "efault", TOKEN_DEFAULT);
    case 'e':
      if ((int)(scanner->current - scanner->start) > 1) {
        switch (scanner->start[1]) {
          case 'l': return checkKeyword(scanner, 1, 3, "lse", TOKEN_ELSE);
          case 'n': return checkKeyword(scanner, 1, 3, "num", TOKEN_ENUM);
          case 'x': return checkKeyword(scanner, 1, 5, "xport", TOKEN_EXPORT);
        }
      }
      return TOKEN_IDENTIFIER;
    case 'f':
      if ((int)(scanner->current - scanner->start) > 1) {
        switch (scanner->start[1]) {
          case 'a': return checkKeyword(scanner, 2, 3, "lse", TOKEN_FALSE);
          case 'o': {
            ErkaoTokenType type = checkKeyword(scanner, 1, 6, "oreach", TOKEN_FOREACH);
            if (type != TOKEN_IDENTIFIER) return type;
            return checkKeyword(scanner, 1, 2, "or", TOKEN_FOR);
          }
          case 'r': return checkKeyword(scanner, 1, 3, "rom", TOKEN_FROM);
          case 'u': return checkKeyword(scanner, 2, 1, "n", TOKEN_FUN);
        }
      }
      break;
      case 'i':
        if ((int)(scanner->current - scanner->start) > 1) {
          switch (scanner->start[1]) {
            case 'f': return checkKeyword(scanner, 1, 1, "f", TOKEN_IF);
            case 'm': {
              ErkaoTokenType type = checkKeyword(scanner, 2, 4, "port", TOKEN_IMPORT);
              if (type != TOKEN_IDENTIFIER) return type;
              return checkKeyword(scanner, 1, 9, "mplements", TOKEN_IMPLEMENTS);
            }
            case 'n': {
              ErkaoTokenType type = checkKeyword(scanner, 1, 1, "n", TOKEN_IN);
              if (type != TOKEN_IDENTIFIER) return type;
              return checkKeyword(scanner, 1, 8, "nterface", TOKEN_INTERFACE);
            }
          }
        }
        return TOKEN_IDENTIFIER;
    case 'l':
      return checkKeyword(scanner, 1, 2, "et", TOKEN_LET);
    case 'm':
      return checkKeyword(scanner, 1, 4, "atch", TOKEN_MATCH);
    case 'n':
      return checkKeyword(scanner, 1, 3, "ull", TOKEN_NULL);
    case 'o':
      return checkKeyword(scanner, 1, 1, "r", TOKEN_OR);
    case 'p':
      return checkKeyword(scanner, 1, 6, "rivate", TOKEN_PRIVATE);
    case 'r':
      return checkKeyword(scanner, 1, 5, "eturn", TOKEN_RETURN);
    case 's':
      return checkKeyword(scanner, 1, 5, "witch", TOKEN_SWITCH);
    case 't':
      if ((int)(scanner->current - scanner->start) > 1) {
        switch (scanner->start[1]) {
          case 'h': {
            ErkaoTokenType type = checkKeyword(scanner, 2, 2, "is", TOKEN_THIS);
            if (type != TOKEN_IDENTIFIER) return type;
            return checkKeyword(scanner, 2, 3, "row", TOKEN_THROW);
          }
          case 'y':
            return checkKeyword(scanner, 2, 2, "pe", TOKEN_TYPE);
          case 'r': {
            ErkaoTokenType type = checkKeyword(scanner, 2, 2, "ue", TOKEN_TRUE);
            if (type != TOKEN_IDENTIFIER) return type;
            return checkKeyword(scanner, 2, 1, "y", TOKEN_TRY);
          }
        }
      }
      break;
    case 'y':
      return checkKeyword(scanner, 1, 4, "ield", TOKEN_YIELD);
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

static Token scanStringSegment(Scanner* scanner) {
  const char* segmentStart = scanner->current;
  int segmentLine = scanner->line;
  int segmentColumn = scanner->column;

  for (;;) {
    if (isAtEnd(scanner)) {
      scanner->inString = false;
      scanner->stringIsMultiline = false;
      scanner->inInterpolation = false;
      scanner->interpolationDepth = 0;
      return errorToken(scanner, "Unterminated string.");
    }

    char c = peek(scanner);
    if (!scanner->stringIsMultiline && c == '\n') {
      scanner->inString = false;
      scanner->stringIsMultiline = false;
      scanner->inInterpolation = false;
      scanner->interpolationDepth = 0;
      return errorToken(scanner, "Unterminated string.");
    }

    if (c == '\\' && peekNext(scanner) != '\0') {
      advance(scanner);
      advance(scanner);
      continue;
    }

    if (c == '$' && peekNext(scanner) == '{') {
      Token segment = makeTokenFromSpan(
          segmentStart, scanner->current, segmentLine, segmentColumn, TOKEN_STRING_SEGMENT);
      const char* interpStart = scanner->current;
      int interpLine = scanner->line;
      int interpColumn = scanner->column;
      advance(scanner);
      advance(scanner);
      scanner->pendingToken = makeTokenFromSpan(
          interpStart, scanner->current, interpLine, interpColumn, TOKEN_INTERP_START);
      scanner->hasPendingToken = true;
      scanner->inInterpolation = true;
      scanner->interpolationDepth = 0;
      scanner->start = scanner->current;
      scanner->startLine = scanner->line;
      scanner->startColumn = scanner->column;
      return segment;
    }

    if (c == '"' &&
        (!scanner->stringIsMultiline ||
         (peekNext(scanner) == '"' && peekNextNext(scanner) == '"'))) {
      Token segment = makeTokenFromSpan(
          segmentStart, scanner->current, segmentLine, segmentColumn, TOKEN_STRING_SEGMENT);
      if (scanner->stringIsMultiline) {
        advance(scanner);
        advance(scanner);
        advance(scanner);
      } else {
        advance(scanner);
      }
      scanner->inString = false;
      scanner->stringIsMultiline = false;
      return segment;
    }

    advance(scanner);
  }
}

static Token scanStringLiteral(Scanner* scanner, bool multiline, bool allowInterpolation) {
  const char* literalStart = scanner->start;
  int literalLine = scanner->startLine;
  int literalColumn = scanner->startColumn;

  if (multiline) {
    advance(scanner);
    advance(scanner);
  }

  const char* segmentStart = scanner->current;
  int segmentLine = scanner->line;
  int segmentColumn = scanner->column;

  for (;;) {
    if (isAtEnd(scanner)) {
      scanner->inString = false;
      scanner->stringIsMultiline = false;
      scanner->inInterpolation = false;
      scanner->interpolationDepth = 0;
      return errorToken(scanner, "Unterminated string.");
    }

    char c = peek(scanner);
    if (!multiline && c == '\n') {
      scanner->inString = false;
      scanner->stringIsMultiline = false;
      scanner->inInterpolation = false;
      scanner->interpolationDepth = 0;
      return errorToken(scanner, "Unterminated string.");
    }

    if (c == '\\' && peekNext(scanner) != '\0') {
      advance(scanner);
      advance(scanner);
      continue;
    }

    if (allowInterpolation && c == '$' && peekNext(scanner) == '{') {
      Token segment = makeTokenFromSpan(
          segmentStart, scanner->current, segmentLine, segmentColumn, TOKEN_STRING_SEGMENT);
      const char* interpStart = scanner->current;
      int interpLine = scanner->line;
      int interpColumn = scanner->column;
      advance(scanner);
      advance(scanner);
      scanner->pendingToken = makeTokenFromSpan(
          interpStart, scanner->current, interpLine, interpColumn, TOKEN_INTERP_START);
      scanner->hasPendingToken = true;
      scanner->inInterpolation = true;
      scanner->interpolationDepth = 0;
      scanner->inString = true;
      scanner->stringIsMultiline = multiline;
      scanner->start = scanner->current;
      scanner->startLine = scanner->line;
      scanner->startColumn = scanner->column;
      return segment;
    }

    if (c == '"' &&
        (!multiline || (peekNext(scanner) == '"' && peekNextNext(scanner) == '"'))) {
      if (multiline) {
        advance(scanner);
        advance(scanner);
        advance(scanner);
      } else {
        advance(scanner);
      }
      scanner->start = literalStart;
      scanner->startLine = literalLine;
      scanner->startColumn = literalColumn;
      return makeToken(scanner, TOKEN_STRING);
    }

    advance(scanner);
  }
}

static Token scanToken(Scanner* scanner) {
  if (scanner->hasPendingToken) {
    scanner->hasPendingToken = false;
    return scanner->pendingToken;
  }

  if (!scanner->inInterpolation && scanner->inString) {
    return scanStringSegment(scanner);
  }

  skipWhitespace(scanner);
  scanner->start = scanner->current;
  scanner->startLine = scanner->line;
  scanner->startColumn = scanner->column;

  if (isAtEnd(scanner)) return makeToken(scanner, TOKEN_EOF);

  char c = advance(scanner);

  if (scanner->inInterpolation) {
    if (c == '{') {
      scanner->interpolationDepth++;
      return makeToken(scanner, TOKEN_LEFT_BRACE);
    }
    if (c == '}') {
      if (scanner->interpolationDepth == 0) {
        scanner->inInterpolation = false;
        scanner->inString = true;
        scanner->start = scanner->current;
        scanner->startLine = scanner->line;
        scanner->startColumn = scanner->column;
        return makeToken(scanner, TOKEN_INTERP_END);
      }
      scanner->interpolationDepth--;
      return makeToken(scanner, TOKEN_RIGHT_BRACE);
    }
  }

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
      if (match(scanner, '.')) {
        if (match(scanner, '.')) return makeToken(scanner, TOKEN_ELLIPSIS);
        return makeToken(scanner, TOKEN_DOT_DOT);
      }
      return makeToken(scanner, TOKEN_DOT);
    case '-':
      return makeToken(scanner, TOKEN_MINUS);
    case '+':
      return makeToken(scanner, TOKEN_PLUS);
    case '?':
      if (match(scanner, '.')) return makeToken(scanner, TOKEN_QUESTION_DOT);
      return makeToken(scanner, TOKEN_QUESTION);
    case ';':
      return makeToken(scanner, TOKEN_SEMICOLON);
    case '*':
      return makeToken(scanner, TOKEN_STAR);
    case ':':
      return makeToken(scanner, TOKEN_COLON);
    case '^':
      return makeToken(scanner, TOKEN_CARET);
    case '|':
      return makeToken(scanner, TOKEN_PIPE);
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
      if (peek(scanner) == '"' && peekNext(scanner) == '"') {
        return scanStringLiteral(scanner, true, !scanner->inInterpolation);
      }
      return scanStringLiteral(scanner, false, !scanner->inInterpolation);
  }

  return errorToken(scanner, "Unexpected character.");
}

TokenArray scanTokens(const char* source, const char* path, bool* hadError) {
  Scanner scanner;
  initScanner(&scanner, source);

  TokenArray array;
  initTokenArray(&array);

#ifdef ERKAO_FUZZING
  (void)path;
#endif

  *hadError = false;

  for (;;) {
    Token token = scanToken(&scanner);
    if (token.type == TOKEN_ERROR) {
#ifndef ERKAO_FUZZING
      const char* displayPath = path ? path : "<repl>";
      fprintf(stderr, "%s:%d:%d: Error: %s\n",
              displayPath, token.line, token.column, token.start);
      printErrorContext(source, token.line, token.column, token.length);
#endif
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
    case TOKEN_DOT_DOT: return "DOT_DOT";
    case TOKEN_ELLIPSIS: return "ELLIPSIS";
    case TOKEN_QUESTION: return "QUESTION";
    case TOKEN_QUESTION_DOT: return "QUESTION_DOT";
    case TOKEN_MINUS: return "MINUS";
    case TOKEN_PLUS: return "PLUS";
    case TOKEN_SEMICOLON: return "SEMICOLON";
    case TOKEN_SLASH: return "SLASH";
    case TOKEN_STAR: return "STAR";
    case TOKEN_COLON: return "COLON";
    case TOKEN_CARET: return "CARET";
    case TOKEN_PIPE: return "PIPE";
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
    case TOKEN_STRING_SEGMENT: return "STRING_SEGMENT";
    case TOKEN_NUMBER: return "NUMBER";
    case TOKEN_INTERP_START: return "INTERP_START";
    case TOKEN_INTERP_END: return "INTERP_END";
    case TOKEN_AND: return "AND";
    case TOKEN_AS: return "AS";
    case TOKEN_CLASS: return "CLASS";
    case TOKEN_CONST: return "CONST";
    case TOKEN_ELSE: return "ELSE";
    case TOKEN_ENUM: return "ENUM";
    case TOKEN_EXPORT: return "EXPORT";
    case TOKEN_FALSE: return "FALSE";
    case TOKEN_FUN: return "FUN";
    case TOKEN_IF: return "IF";
    case TOKEN_IMPORT: return "IMPORT";
    case TOKEN_IMPLEMENTS: return "IMPLEMENTS";
    case TOKEN_INTERFACE: return "INTERFACE";
    case TOKEN_MATCH: return "MATCH";
    case TOKEN_NULL: return "NULL";
    case TOKEN_OR: return "OR";
    case TOKEN_PRIVATE: return "PRIVATE";
    case TOKEN_RETURN: return "RETURN";
    case TOKEN_TYPE: return "TYPE";
    case TOKEN_TRY: return "TRY";
    case TOKEN_CATCH: return "CATCH";
    case TOKEN_THROW: return "THROW";
    case TOKEN_THIS: return "THIS";
    case TOKEN_TRUE: return "TRUE";
    case TOKEN_YIELD: return "YIELD";
    case TOKEN_LET: return "LET";
    case TOKEN_WHILE: return "WHILE";
    case TOKEN_FOR: return "FOR";
    case TOKEN_FOREACH: return "FOREACH";
    case TOKEN_SWITCH: return "SWITCH";
    case TOKEN_CASE: return "CASE";
    case TOKEN_DEFAULT: return "DEFAULT";
    case TOKEN_BREAK: return "BREAK";
    case TOKEN_CONTINUE: return "CONTINUE";
    case TOKEN_IN: return "IN";
    case TOKEN_FROM: return "FROM";
    case TOKEN_ERROR: return "ERROR";
    case TOKEN_EOF: return "EOF";
    default: return "UNKNOWN";
  }
}

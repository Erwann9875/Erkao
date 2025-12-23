#include "tooling.h"
#include "lexer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FORMAT_INDENT_SPACES 2
#define LINT_MAX_LINE 120

typedef struct {
  char* data;
  size_t length;
  size_t capacity;
} StringBuilder;

static void sbInit(StringBuilder* sb) {
  sb->data = NULL;
  sb->length = 0;
  sb->capacity = 0;
}

static void sbEnsure(StringBuilder* sb, size_t needed) {
  if (sb->capacity >= needed) return;
  size_t oldCapacity = sb->capacity;
  size_t newCapacity = oldCapacity == 0 ? 256 : oldCapacity;
  while (newCapacity < needed) {
    newCapacity *= 2;
  }
  char* next = (char*)realloc(sb->data, newCapacity);
  if (!next) {
    fprintf(stderr, "Out of memory.\n");
    exit(1);
  }
  sb->data = next;
  sb->capacity = newCapacity;
}

static void sbAppendN(StringBuilder* sb, const char* text, size_t length) {
  sbEnsure(sb, sb->length + length + 1);
  memcpy(sb->data + sb->length, text, length);
  sb->length += length;
  sb->data[sb->length] = '\0';
}

static void sbAppendChar(StringBuilder* sb, char c) {
  sbEnsure(sb, sb->length + 2);
  sb->data[sb->length++] = c;
  sb->data[sb->length] = '\0';
}

static void sbAppendIndent(StringBuilder* sb, int level) {
  int count = level * FORMAT_INDENT_SPACES;
  for (int i = 0; i < count; i++) {
    sbAppendChar(sb, ' ');
  }
}

static char* readFile(const char* path) {
  FILE* file = fopen(path, "rb");
  if (!file) {
    fprintf(stderr, "Could not open file '%s'.\n", path);
    return NULL;
  }

  fseek(file, 0L, SEEK_END);
  long size = ftell(file);
  rewind(file);

  char* buffer = (char*)malloc((size_t)size + 1);
  if (!buffer) {
    fprintf(stderr, "Not enough memory to read '%s'.\n", path);
    fclose(file);
    return NULL;
  }

  size_t read = fread(buffer, 1, (size_t)size, file);
  buffer[read] = '\0';
  fclose(file);
  return buffer;
}

static bool writeFile(const char* path, const char* data, size_t length) {
  FILE* file = fopen(path, "wb");
  if (!file) {
    fprintf(stderr, "Could not write file '%s'.\n", path);
    return false;
  }
  size_t written = fwrite(data, 1, length, file);
  fclose(file);
  return written == length;
}

static bool startsWithKeyword(const char* line, const char* keyword) {
  size_t length = strlen(keyword);
  if (strncmp(line, keyword, length) != 0) return false;
  char next = line[length];
  return next == '\0' || next == ' ' || next == ':' || next == '(';
}

static int countBraceDelta(const char* line, bool* inBlockComment) {
  int delta = 0;
  bool inString = false;
  for (size_t i = 0; line[i] != '\0'; i++) {
    char c = line[i];
    char next = line[i + 1];

    if (*inBlockComment) {
      if (c == '*' && next == '/') {
        *inBlockComment = false;
        i++;
      }
      continue;
    }

    if (inString) {
      if (c == '\\' && next != '\0') {
        i++;
        continue;
      }
      if (c == '"') {
        inString = false;
      }
      continue;
    }

    if (c == '/' && next == '/') {
      break;
    }
    if (c == '/' && next == '*') {
      *inBlockComment = true;
      i++;
      continue;
    }
    if (c == '"') {
      inString = true;
      continue;
    }
    if (c == '{') delta++;
    if (c == '}') delta--;
  }
  return delta;
}

static char* formatSource(const char* source) {
  StringBuilder out;
  sbInit(&out);

  bool inBlockComment = false;
  int indent = 0;
  int caseIndent = 0;

  const char* lineStart = source;
  for (const char* cursor = source;; cursor++) {
    if (*cursor != '\n' && *cursor != '\0') continue;

    size_t lineLength = (size_t)(cursor - lineStart);
    while (lineLength > 0 && (lineStart[lineLength - 1] == '\r' ||
                              lineStart[lineLength - 1] == ' ' ||
                              lineStart[lineLength - 1] == '\t')) {
      lineLength--;
    }

    const char* trim = lineStart;
    size_t trimLength = lineLength;
    while (trimLength > 0 && (*trim == ' ' || *trim == '\t')) {
      trim++;
      trimLength--;
    }

    if (trimLength == 0) {
      sbAppendChar(&out, '\n');
    } else {
      bool startsWithClose = trim[0] == '}';
      bool startsWithCase = startsWithKeyword(trim, "case") ||
                            startsWithKeyword(trim, "default");

      if (startsWithClose) {
        indent = indent > 0 ? indent - 1 : 0;
        caseIndent = 0;
      }

      int outIndent = indent + caseIndent;
      if (startsWithCase) {
        outIndent = indent;
      }

      sbAppendIndent(&out, outIndent);
      sbAppendN(&out, trim, trimLength);
      sbAppendChar(&out, '\n');

      int delta = countBraceDelta(trim, &inBlockComment);
      indent += delta;
      if (indent < 0) indent = 0;
      if (delta < 0) {
        caseIndent = 0;
      } else if (startsWithCase) {
        caseIndent = 1;
      }
    }

    if (*cursor == '\0') break;
    lineStart = cursor + 1;
  }

  return out.data;
}

bool formatFile(const char* path, bool checkOnly, bool* changed) {
  char* source = readFile(path);
  if (!source) return false;

  char* formatted = formatSource(source);
  if (!formatted) {
    free(source);
    return false;
  }

  bool didChange = strcmp(source, formatted) != 0;
  if (changed) *changed = didChange;

  bool ok = true;
  if (didChange && !checkOnly) {
    ok = writeFile(path, formatted, strlen(formatted));
  }

  free(source);
  free(formatted);
  return ok;
}

static void reportLint(const char* path, int line, int column, const char* message) {
  fprintf(stderr, "%s:%d:%d: Lint: %s\n", path, line, column, message);
}

int lintFile(const char* path) {
  char* source = readFile(path);
  if (!source) return -1;

  int issues = 0;
  int lineNo = 1;
  const char* lineStart = source;
  for (const char* cursor = source;; cursor++) {
    if (*cursor != '\n' && *cursor != '\0') continue;

    size_t lineLength = (size_t)(cursor - lineStart);
    if (lineLength > 0 && lineStart[lineLength - 1] == '\r') {
      lineLength--;
    }

    size_t trailing = lineLength;
    while (trailing > 0 &&
           (lineStart[trailing - 1] == ' ' || lineStart[trailing - 1] == '\t')) {
      trailing--;
    }
    if (trailing != lineLength) {
      issues++;
      reportLint(path, lineNo, (int)trailing + 1, "Trailing whitespace.");
    }

    size_t leading = 0;
    while (leading < trailing &&
           (lineStart[leading] == ' ' || lineStart[leading] == '\t')) {
      if (lineStart[leading] == '\t') {
        issues++;
        reportLint(path, lineNo, (int)leading + 1, "Tab indentation.");
      }
      leading++;
    }
    if (leading > 0 && (leading % FORMAT_INDENT_SPACES) != 0) {
      issues++;
      reportLint(path, lineNo, 1, "Indentation is not a multiple of 2 spaces.");
    }

    for (size_t i = 0; i < trailing; i++) {
      if (lineStart[i] == '\t') {
        issues++;
        reportLint(path, lineNo, (int)i + 1, "Tab character.");
      }
    }

    if ((int)lineLength > LINT_MAX_LINE) {
      issues++;
      reportLint(path, lineNo, LINT_MAX_LINE + 1, "Line exceeds 120 characters.");
    }

    if (*cursor == '\0') break;
    lineStart = cursor + 1;
    lineNo++;
  }

  bool lexError = false;
  TokenArray tokens = scanTokens(source, path, &lexError);
  if (lexError) {
    freeTokenArray(&tokens);
    free(source);
    return issues + 1;
  }

  int loopDepth = 0;
  int switchDepth = 0;
  for (int i = 0; i < tokens.count; i++) {
    Token t = tokens.tokens[i];
    switch (t.type) {
      case TOKEN_WHILE:
      case TOKEN_FOR:
      case TOKEN_FOREACH:
        loopDepth++;
        break;
      case TOKEN_SWITCH:
        switchDepth++;
        break;
      case TOKEN_LEFT_BRACE:
        break;
      case TOKEN_RIGHT_BRACE:
        break;
      case TOKEN_BREAK:
        if (loopDepth == 0 && switchDepth == 0) {
          issues++;
          reportLint(path, t.line, t.column, "Cannot use 'break' outside of a loop or switch.");
        }
        break;
      case TOKEN_CONTINUE:
        if (loopDepth == 0) {
          issues++;
          reportLint(path, t.line, t.column, "Cannot use 'continue' outside of a loop.");
        }
        break;
      default:
        break;
    }
    
    if (t.type == TOKEN_SEMICOLON && i > 0 && tokens.tokens[i-1].type == TOKEN_RIGHT_BRACE) {
      if (loopDepth > 0) loopDepth--;
    }
  }

  freeTokenArray(&tokens);
  free(source);
  return issues;
}

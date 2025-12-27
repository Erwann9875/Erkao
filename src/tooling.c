#include "tooling.h"
#include "lexer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEFAULT_FORMAT_INDENT 2
#define DEFAULT_LINT_MAX_LINE 120
#define DEFAULT_LINT_RULES (LINT_RULE_TRAILING_WS | LINT_RULE_TABS | LINT_RULE_INDENT | \
                            LINT_RULE_LINE_LENGTH | LINT_RULE_FLOW | LINT_RULE_LEX)

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

static void sbAppendIndent(StringBuilder* sb, int level, int indentSpaces) {
  int count = level * indentSpaces;
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

void toolingConfigInit(ToolingConfig* config) {
  if (!config) return;
  config->formatIndent = DEFAULT_FORMAT_INDENT;
  config->lintMaxLine = DEFAULT_LINT_MAX_LINE;
  config->lintRules = DEFAULT_LINT_RULES;
}

static bool applyFormatRuleset(ToolingConfig* config, const char* name) {
  if (!config || !name) return false;
  if (strcmp(name, "standard") == 0) {
    config->formatIndent = DEFAULT_FORMAT_INDENT;
    return true;
  }
  if (strcmp(name, "wide") == 0) {
    config->formatIndent = 4;
    return true;
  }
  return false;
}

static bool applyLintRuleset(ToolingConfig* config, const char* name) {
  if (!config || !name) return false;
  if (strcmp(name, "basic") == 0) {
    config->lintRules = LINT_RULE_TRAILING_WS | LINT_RULE_TABS |
                        LINT_RULE_INDENT | LINT_RULE_LINE_LENGTH;
    return true;
  }
  if (strcmp(name, "default") == 0) {
    config->lintRules = DEFAULT_LINT_RULES;
    config->lintMaxLine = DEFAULT_LINT_MAX_LINE;
    return true;
  }
  if (strcmp(name, "strict") == 0) {
    config->lintRules = DEFAULT_LINT_RULES;
    config->lintMaxLine = 100;
    return true;
  }
  return false;
}

static unsigned int lintRuleFromName(const char* name) {
  if (!name) return 0;
  if (strcmp(name, "trailing") == 0) return LINT_RULE_TRAILING_WS;
  if (strcmp(name, "tabs") == 0) return LINT_RULE_TABS;
  if (strcmp(name, "indent") == 0) return LINT_RULE_INDENT;
  if (strcmp(name, "line-length") == 0) return LINT_RULE_LINE_LENGTH;
  if (strcmp(name, "flow") == 0) return LINT_RULE_FLOW;
  if (strcmp(name, "lex") == 0) return LINT_RULE_LEX;
  return 0;
}

static void trimWhitespace(char** start, char** end) {
  while (*start < *end && (**start == ' ' || **start == '\t')) {
    (*start)++;
  }
  while (*end > *start && ((*end)[-1] == ' ' || (*end)[-1] == '\t')) {
    (*end)--;
  }
}

bool toolingApplyFormatRuleset(ToolingConfig* config, const char* name) {
  return applyFormatRuleset(config, name);
}

bool toolingApplyLintRuleset(ToolingConfig* config, const char* name) {
  return applyLintRuleset(config, name);
}

bool toolingApplyLintRules(ToolingConfig* config, const char* rules) {
  if (!config || !rules) return false;
  if (strcmp(rules, "all") == 0) {
    config->lintRules = DEFAULT_LINT_RULES;
    return true;
  }
  if (strcmp(rules, "none") == 0) {
    config->lintRules = 0;
    return true;
  }
  unsigned int mask = 0;
  const char* cursor = rules;
  while (*cursor) {
    while (*cursor == ' ' || *cursor == '\t' || *cursor == ',') cursor++;
    const char* start = cursor;
    while (*cursor && *cursor != ',' && *cursor != ' ' && *cursor != '\t') {
      cursor++;
    }
    size_t len = (size_t)(cursor - start);
    if (len > 0) {
      char name[32];
      size_t copyLen = len < sizeof(name) - 1 ? len : sizeof(name) - 1;
      memcpy(name, start, copyLen);
      name[copyLen] = '\0';
      unsigned int rule = lintRuleFromName(name);
      if (rule == 0) return false;
      mask |= rule;
    }
  }
  config->lintRules = mask;
  return true;
}

bool toolingLoadConfig(const char* path, ToolingConfig* config) {
  if (!path || !config) return false;
  char* source = readFile(path);
  if (!source) {
    fprintf(stderr, "Could not read tooling config '%s'.\n", path);
    return false;
  }

  char* line = source;
  while (*line) {
    char* lineEnd = strchr(line, '\n');
    if (!lineEnd) {
      lineEnd = line + strlen(line);
    }
    char* cursor = line;
    char* end = lineEnd;
    if (end > cursor && end[-1] == '\r') {
      end--;
    }
    trimWhitespace(&cursor, &end);
    if (cursor < end) {
      if (*cursor != '#' && !(cursor[0] == '/' && cursor[1] == '/')) {
        char* comment = strstr(cursor, "//");
        if (comment && comment < end) {
          end = comment;
          trimWhitespace(&cursor, &end);
        }
        char* sep = memchr(cursor, '=', (size_t)(end - cursor));
        if (!sep) {
          sep = memchr(cursor, ':', (size_t)(end - cursor));
        }
        if (sep) {
          char* keyStart = cursor;
          char* keyEnd = sep;
          char* valueStart = sep + 1;
          char* valueEnd = end;
          trimWhitespace(&keyStart, &keyEnd);
          trimWhitespace(&valueStart, &valueEnd);
          if (keyStart < keyEnd && valueStart <= valueEnd) {
            char key[64];
            char value[64];
            size_t keyLen = (size_t)(keyEnd - keyStart);
            size_t valueLen = (size_t)(valueEnd - valueStart);
            if (keyLen >= sizeof(key)) keyLen = sizeof(key) - 1;
            if (valueLen >= sizeof(value)) valueLen = sizeof(value) - 1;
            memcpy(key, keyStart, keyLen);
            key[keyLen] = '\0';
            memcpy(value, valueStart, valueLen);
            value[valueLen] = '\0';

            if (strcmp(key, "format.indent") == 0) {
              int indent = atoi(value);
              if (indent > 0) config->formatIndent = indent;
            } else if (strcmp(key, "format.ruleset") == 0) {
              if (!applyFormatRuleset(config, value)) {
                fprintf(stderr, "Unknown format ruleset '%s'.\n", value);
              }
            } else if (strcmp(key, "lint.maxLine") == 0) {
              int maxLine = atoi(value);
              if (maxLine > 0) config->lintMaxLine = maxLine;
            } else if (strcmp(key, "lint.ruleset") == 0) {
              if (!applyLintRuleset(config, value)) {
                fprintf(stderr, "Unknown lint ruleset '%s'.\n", value);
              }
            } else if (strcmp(key, "lint.rules") == 0) {
              if (!toolingApplyLintRules(config, value)) {
                fprintf(stderr, "Unknown lint rules in '%s'.\n", value);
              }
            }
          }
        }
      }
    }
    if (*lineEnd == '\0') break;
    line = lineEnd + 1;
  }

  free(source);
  return true;
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

static char* formatSource(const char* source, int indentSpaces) {
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

      sbAppendIndent(&out, outIndent, indentSpaces);
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

bool formatFileWithConfig(const char* path, bool checkOnly, bool* changed,
                          const ToolingConfig* config) {
  char* source = readFile(path);
  if (!source) return false;

  int indentSpaces = DEFAULT_FORMAT_INDENT;
  if (config && config->formatIndent > 0) {
    indentSpaces = config->formatIndent;
  }
  char* formatted = formatSource(source, indentSpaces);
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

bool formatFile(const char* path, bool checkOnly, bool* changed) {
  ToolingConfig config;
  toolingConfigInit(&config);
  return formatFileWithConfig(path, checkOnly, changed, &config);
}

static void reportLint(const char* path, int line, int column, const char* message) {
  fprintf(stderr, "%s:%d:%d: Lint: %s\n", path, line, column, message);
}

int lintFileWithConfig(const char* path, const ToolingConfig* config) {
  char* source = readFile(path);
  if (!source) return -1;

  unsigned int rules = config ? config->lintRules : DEFAULT_LINT_RULES;
  int indentSpaces = config && config->formatIndent > 0 ? config->formatIndent : DEFAULT_FORMAT_INDENT;
  int maxLine = config && config->lintMaxLine > 0 ? config->lintMaxLine : DEFAULT_LINT_MAX_LINE;

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
    if ((rules & LINT_RULE_TRAILING_WS) != 0 && trailing != lineLength) {
      issues++;
      reportLint(path, lineNo, (int)trailing + 1, "Trailing whitespace.");
    }

    size_t leading = 0;
    while (leading < trailing &&
           (lineStart[leading] == ' ' || lineStart[leading] == '\t')) {
      if (lineStart[leading] == '\t') {
        if ((rules & LINT_RULE_TABS) != 0) {
          issues++;
          reportLint(path, lineNo, (int)leading + 1, "Tab indentation.");
        }
      }
      leading++;
    }
    if ((rules & LINT_RULE_INDENT) != 0 && leading > 0 && indentSpaces > 0 &&
        (leading % (size_t)indentSpaces) != 0) {
      char message[64];
      snprintf(message, sizeof(message),
               "Indentation is not a multiple of %d spaces.", indentSpaces);
      issues++;
      reportLint(path, lineNo, 1, message);
    }

    for (size_t i = 0; i < trailing; i++) {
      if (lineStart[i] == '\t') {
        if ((rules & LINT_RULE_TABS) != 0) {
          issues++;
          reportLint(path, lineNo, (int)i + 1, "Tab character.");
        }
      }
    }

    if ((rules & LINT_RULE_LINE_LENGTH) != 0 && (int)lineLength > maxLine) {
      char message[64];
      snprintf(message, sizeof(message), "Line exceeds %d characters.", maxLine);
      issues++;
      reportLint(path, lineNo, maxLine + 1, message);
    }

    if (*cursor == '\0') break;
    lineStart = cursor + 1;
    lineNo++;
  }

  if ((rules & (LINT_RULE_FLOW | LINT_RULE_LEX)) != 0) {
    bool lexError = false;
    TokenArray tokens = scanTokens(source, path, &lexError);
    if (lexError) {
      freeTokenArray(&tokens);
      free(source);
      return issues + ((rules & LINT_RULE_LEX) != 0 ? 1 : 0);
    }

    if ((rules & LINT_RULE_FLOW) != 0) {
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
          case TOKEN_MATCH:
            switchDepth++;
            break;
          case TOKEN_BREAK:
            if (loopDepth == 0 && switchDepth == 0) {
              issues++;
              reportLint(path, t.line, t.column,
                         "Cannot use 'break' outside of a loop or switch.");
            }
            break;
          case TOKEN_CONTINUE:
            if (loopDepth == 0) {
              issues++;
              reportLint(path, t.line, t.column,
                         "Cannot use 'continue' outside of a loop.");
            }
            break;
          default:
            break;
        }
        if (t.type == TOKEN_SEMICOLON && i > 0 &&
            tokens.tokens[i - 1].type == TOKEN_RIGHT_BRACE) {
          if (loopDepth > 0) loopDepth--;
        }
      }
    }

    freeTokenArray(&tokens);
  }
  free(source);
  return issues;
}

int lintFile(const char* path) {
  ToolingConfig config;
  toolingConfigInit(&config);
  return lintFileWithConfig(path, &config);
}

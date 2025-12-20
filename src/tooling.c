#include "tooling.h"
#include "parser.h"

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

typedef struct {
  Token name;
  bool readEver;
  bool pendingWrite;
  Token lastWrite;
  Token* unusedWrites;
  int unusedCount;
  int unusedCapacity;
} LintLocal;

typedef struct {
  LintLocal* locals;
  int count;
  int capacity;
} LintScope;

typedef struct {
  const char* path;
  int issues;
  int scopeDepth;
  int loopDepth;
  int switchDepth;
  LintScope* scopes;
  int scopeCount;
  int scopeCapacity;
} LintState;

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

static void freeStatements(StmtArray* statements) {
  for (int i = 0; i < statements->count; i++) {
    freeStmt(statements->items[i]);
  }
  freeStmtArray(statements);
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

static Token noToken(void) {
  Token token;
  memset(&token, 0, sizeof(Token));
  return token;
}

static bool tokenMatches(Token a, Token b) {
  if (a.length != b.length || a.length <= 0) return false;
  return memcmp(a.start, b.start, (size_t)a.length) == 0;
}

static void reportLintToken(LintState* state, Token token, const char* message) {
  int line = token.line > 0 ? token.line : 1;
  int column = token.column > 0 ? token.column : 1;
  reportLint(state->path, line, column, message);
  state->issues++;
}

static void pushScope(LintState* state) {
  if (state->scopeCapacity < state->scopeCount + 1) {
    int oldCapacity = state->scopeCapacity;
    state->scopeCapacity = oldCapacity == 0 ? 4 : oldCapacity * 2;
    state->scopes = (LintScope*)realloc(state->scopes,
                                        sizeof(LintScope) * (size_t)state->scopeCapacity);
    if (!state->scopes) {
      fprintf(stderr, "Out of memory.\n");
      exit(1);
    }
  }
  LintScope* scope = &state->scopes[state->scopeCount++];
  scope->locals = NULL;
  scope->count = 0;
  scope->capacity = 0;
  state->scopeDepth++;
}

static void popScope(LintState* state) {
  if (state->scopeCount <= 0) return;
  LintScope* scope = &state->scopes[state->scopeCount - 1];
  for (int i = 0; i < scope->count; i++) {
    LintLocal* local = &scope->locals[i];
    if (state->scopeDepth > 0) {
      if (!local->readEver) {
        reportLintToken(state, local->name, "Unused local.");
      } else {
        for (int j = 0; j < local->unusedCount; j++) {
          reportLintToken(state, local->unusedWrites[j], "Assigned value is never read.");
        }
        if (local->pendingWrite) {
          reportLintToken(state, local->lastWrite, "Assigned value is never read.");
        }
      }
    }
    free(local->unusedWrites);
  }
  free(scope->locals);
  state->scopeCount--;
  state->scopeDepth--;
}

static void declareLocal(LintState* state, Token name) {
  if (state->scopeCount <= 0) return;
  LintScope* scope = &state->scopes[state->scopeCount - 1];
  if (scope->capacity < scope->count + 1) {
    int oldCapacity = scope->capacity;
    scope->capacity = oldCapacity == 0 ? 8 : oldCapacity * 2;
    scope->locals = (LintLocal*)realloc(scope->locals,
                                        sizeof(LintLocal) * (size_t)scope->capacity);
    if (!scope->locals) {
      fprintf(stderr, "Out of memory.\n");
      exit(1);
    }
  }
  scope->locals[scope->count].name = name;
  scope->locals[scope->count].readEver = false;
  scope->locals[scope->count].pendingWrite = false;
  scope->locals[scope->count].lastWrite = noToken();
  scope->locals[scope->count].unusedWrites = NULL;
  scope->locals[scope->count].unusedCount = 0;
  scope->locals[scope->count].unusedCapacity = 0;
  scope->count++;
}

static void addUnusedWrite(LintLocal* local, Token token) {
  if (token.length <= 0) return;
  if (local->unusedCapacity < local->unusedCount + 1) {
    int oldCapacity = local->unusedCapacity;
    local->unusedCapacity = oldCapacity == 0 ? 4 : oldCapacity * 2;
    local->unusedWrites = (Token*)realloc(
        local->unusedWrites, sizeof(Token) * (size_t)local->unusedCapacity);
    if (!local->unusedWrites) {
      fprintf(stderr, "Out of memory.\n");
      exit(1);
    }
  }
  local->unusedWrites[local->unusedCount++] = token;
}

static LintLocal* findLocal(LintState* state, Token name) {
  for (int s = state->scopeCount - 1; s >= 0; s--) {
    LintScope* scope = &state->scopes[s];
    for (int i = 0; i < scope->count; i++) {
      if (tokenMatches(scope->locals[i].name, name)) {
        return &scope->locals[i];
      }
    }
  }
  return NULL;
}

static void markLocalRead(LintState* state, Token name) {
  LintLocal* local = findLocal(state, name);
  if (!local) return;
  local->readEver = true;
  if (local->pendingWrite) {
    local->pendingWrite = false;
  }
}

static void markLocalWrite(LintState* state, Token name) {
  LintLocal* local = findLocal(state, name);
  if (!local) return;
  if (local->pendingWrite) {
    addUnusedWrite(local, local->lastWrite);
  }
  local->pendingWrite = true;
  local->lastWrite = name;
}

static Token tokenForExpr(const Expr* expr);
static void lintExpr(LintState* state, Expr* expr);
static bool lintStmtArray(LintState* state, StmtArray* statements);
static bool lintStmt(LintState* state, Stmt* stmt);

static Token tokenForExpr(const Expr* expr) {
  if (!expr) return noToken();
  switch (expr->type) {
    case EXPR_VARIABLE:
      return expr->as.variable.name;
    case EXPR_ASSIGN:
      return expr->as.assign.name;
    case EXPR_UNARY:
      return expr->as.unary.op;
    case EXPR_BINARY:
      return expr->as.binary.op;
    case EXPR_LOGICAL:
      return expr->as.logical.op;
    case EXPR_CALL:
      return expr->as.call.paren;
    case EXPR_GET:
      return expr->as.get.name;
    case EXPR_SET:
      return expr->as.set.name;
    case EXPR_THIS:
      return expr->as.thisExpr.keyword;
    case EXPR_INDEX:
      return expr->as.index.bracket;
    case EXPR_SET_INDEX:
      return expr->as.setIndex.equals;
    case EXPR_GROUPING:
      return tokenForExpr(expr->as.grouping.expression);
    case EXPR_LITERAL:
    case EXPR_ARRAY:
    case EXPR_MAP:
      break;
  }
  return noToken();
}

static Token tokenForStmt(const Stmt* stmt) {
  if (!stmt) return noToken();
  switch (stmt->type) {
    case STMT_EXPR:
      return tokenForExpr(stmt->as.expr.expression);
    case STMT_VAR:
      return stmt->as.var.name;
    case STMT_BLOCK:
      return noToken();
    case STMT_IF:
      return stmt->as.ifStmt.keyword;
    case STMT_WHILE:
      return stmt->as.whileStmt.keyword;
    case STMT_FOR:
      return stmt->as.forStmt.keyword;
    case STMT_FOREACH:
      return stmt->as.foreachStmt.keyword;
    case STMT_SWITCH:
      return stmt->as.switchStmt.keyword;
    case STMT_BREAK:
      return stmt->as.breakStmt.keyword;
    case STMT_CONTINUE:
      return stmt->as.continueStmt.keyword;
    case STMT_IMPORT:
      return stmt->as.importStmt.keyword;
    case STMT_FUNCTION:
      return stmt->as.function.name;
    case STMT_RETURN:
      return stmt->as.ret.keyword;
    case STMT_CLASS:
      return stmt->as.classStmt.name;
  }
  return noToken();
}

static void lintExpr(LintState* state, Expr* expr) {
  if (!expr) return;

  switch (expr->type) {
    case EXPR_LITERAL:
      break;
    case EXPR_GROUPING:
      lintExpr(state, expr->as.grouping.expression);
      break;
    case EXPR_UNARY:
      lintExpr(state, expr->as.unary.right);
      break;
    case EXPR_BINARY:
      lintExpr(state, expr->as.binary.left);
      lintExpr(state, expr->as.binary.right);
      break;
    case EXPR_VARIABLE:
      markLocalRead(state, expr->as.variable.name);
      break;
    case EXPR_ASSIGN:
      lintExpr(state, expr->as.assign.value);
      markLocalWrite(state, expr->as.assign.name);
      break;
    case EXPR_LOGICAL:
      lintExpr(state, expr->as.logical.left);
      lintExpr(state, expr->as.logical.right);
      break;
    case EXPR_CALL:
      lintExpr(state, expr->as.call.callee);
      for (int i = 0; i < expr->as.call.args.count; i++) {
        lintExpr(state, expr->as.call.args.items[i]);
      }
      break;
    case EXPR_GET:
      lintExpr(state, expr->as.get.object);
      break;
    case EXPR_SET:
      lintExpr(state, expr->as.set.object);
      lintExpr(state, expr->as.set.value);
      break;
    case EXPR_THIS:
      break;
    case EXPR_ARRAY:
      for (int i = 0; i < expr->as.array.elements.count; i++) {
        lintExpr(state, expr->as.array.elements.items[i]);
      }
      break;
    case EXPR_MAP:
      for (int i = 0; i < expr->as.map.entries.count; i++) {
        lintExpr(state, expr->as.map.entries.entries[i].key);
        lintExpr(state, expr->as.map.entries.entries[i].value);
      }
      break;
    case EXPR_INDEX:
      lintExpr(state, expr->as.index.object);
      lintExpr(state, expr->as.index.index);
      break;
    case EXPR_SET_INDEX:
      lintExpr(state, expr->as.setIndex.object);
      lintExpr(state, expr->as.setIndex.index);
      lintExpr(state, expr->as.setIndex.value);
      break;
  }
}

static void lintFunctionBody(LintState* state, Stmt* stmt) {
  if (!stmt || stmt->type != STMT_FUNCTION) return;
  int savedLoopDepth = state->loopDepth;
  int savedSwitchDepth = state->switchDepth;
  state->loopDepth = 0;
  state->switchDepth = 0;
  pushScope(state);
  for (int i = 0; i < stmt->as.function.params.count; i++) {
    Param* param = &stmt->as.function.params.items[i];
    declareLocal(state, param->name);
  }
  for (int i = 0; i < stmt->as.function.params.count; i++) {
    Param* param = &stmt->as.function.params.items[i];
    if (param->defaultValue) {
      lintExpr(state, param->defaultValue);
    }
  }
  lintStmtArray(state, &stmt->as.function.body);
  popScope(state);
  state->loopDepth = savedLoopDepth;
  state->switchDepth = savedSwitchDepth;
}

static bool lintStmtArray(LintState* state, StmtArray* statements) {
  bool terminated = false;
  for (int i = 0; i < statements->count; i++) {
    Stmt* stmt = statements->items[i];
    if (terminated) {
      Token token = tokenForStmt(stmt);
      if (token.line > 0) {
        reportLintToken(state, token, "Unreachable code.");
      }
      continue;
    }
    if (lintStmt(state, stmt)) {
      terminated = true;
    }
  }
  return terminated;
}

static bool lintStmt(LintState* state, Stmt* stmt) {
  if (!stmt) return false;

  switch (stmt->type) {
    case STMT_EXPR:
      lintExpr(state, stmt->as.expr.expression);
      return false;
    case STMT_VAR:
      lintExpr(state, stmt->as.var.initializer);
      declareLocal(state, stmt->as.var.name);
      if (stmt->as.var.initializer) {
        markLocalWrite(state, stmt->as.var.name);
      }
      return false;
    case STMT_BLOCK: {
      pushScope(state);
      bool terminates = lintStmtArray(state, &stmt->as.block.statements);
      popScope(state);
      return terminates;
    }
    case STMT_IF: {
      lintExpr(state, stmt->as.ifStmt.condition);
      bool thenTerm = lintStmt(state, stmt->as.ifStmt.thenBranch);
      bool elseTerm = false;
      if (stmt->as.ifStmt.elseBranch) {
        elseTerm = lintStmt(state, stmt->as.ifStmt.elseBranch);
      }
      return stmt->as.ifStmt.elseBranch != NULL && thenTerm && elseTerm;
    }
    case STMT_WHILE:
      lintExpr(state, stmt->as.whileStmt.condition);
      state->loopDepth++;
      lintStmt(state, stmt->as.whileStmt.body);
      state->loopDepth--;
      return false;
    case STMT_FOR:
      pushScope(state);
      lintStmt(state, stmt->as.forStmt.initializer);
      lintExpr(state, stmt->as.forStmt.condition);
      lintExpr(state, stmt->as.forStmt.increment);
      state->loopDepth++;
      lintStmt(state, stmt->as.forStmt.body);
      state->loopDepth--;
      popScope(state);
      return false;
    case STMT_FOREACH:
      pushScope(state);
      lintExpr(state, stmt->as.foreachStmt.iterable);
      if (stmt->as.foreachStmt.hasKey) {
        declareLocal(state, stmt->as.foreachStmt.key);
        markLocalWrite(state, stmt->as.foreachStmt.key);
      }
      declareLocal(state, stmt->as.foreachStmt.value);
      markLocalWrite(state, stmt->as.foreachStmt.value);
      state->loopDepth++;
      lintStmt(state, stmt->as.foreachStmt.body);
      state->loopDepth--;
      popScope(state);
      return false;
    case STMT_SWITCH: {
      pushScope(state);
      lintExpr(state, stmt->as.switchStmt.value);
      state->switchDepth++;
      bool allCasesTerminate = true;
      for (int i = 0; i < stmt->as.switchStmt.cases.count; i++) {
        SwitchCase* entry = &stmt->as.switchStmt.cases.items[i];
        if (!lintStmtArray(state, &entry->statements)) {
          allCasesTerminate = false;
        }
      }
      bool defaultTerminates = false;
      if (stmt->as.switchStmt.hasDefault) {
        defaultTerminates = lintStmtArray(state, &stmt->as.switchStmt.defaultStatements);
      }
      state->switchDepth--;
      popScope(state);
      return stmt->as.switchStmt.hasDefault && allCasesTerminate && defaultTerminates;
    }
    case STMT_BREAK:
      if (state->loopDepth == 0 && state->switchDepth == 0) {
        reportLintToken(state, stmt->as.breakStmt.keyword,
                        "Cannot use 'break' outside of a loop or switch.");
      }
      return true;
    case STMT_CONTINUE:
      if (state->loopDepth == 0) {
        reportLintToken(state, stmt->as.continueStmt.keyword,
                        "Cannot use 'continue' outside of a loop.");
      }
      return true;
    case STMT_IMPORT:
      lintExpr(state, stmt->as.importStmt.path);
      return false;
    case STMT_FUNCTION:
      declareLocal(state, stmt->as.function.name);
      markLocalWrite(state, stmt->as.function.name);
      lintFunctionBody(state, stmt);
      return false;
    case STMT_RETURN:
      lintExpr(state, stmt->as.ret.value);
      return true;
    case STMT_CLASS:
      declareLocal(state, stmt->as.classStmt.name);
      markLocalWrite(state, stmt->as.classStmt.name);
      for (int i = 0; i < stmt->as.classStmt.methods.count; i++) {
        lintFunctionBody(state, stmt->as.classStmt.methods.items[i]);
      }
      return false;
  }

  return false;
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

  StmtArray statements;
  bool parseOk = parseTokens(&tokens, source, path, &statements);
  freeTokenArray(&tokens);
  if (!parseOk) {
    freeStatements(&statements);
    free(source);
    return issues + 1;
  }

  LintState state;
  state.path = path;
  state.issues = 0;
  state.scopeDepth = -1;
  state.loopDepth = 0;
  state.switchDepth = 0;
  state.scopes = NULL;
  state.scopeCount = 0;
  state.scopeCapacity = 0;

  pushScope(&state);
  lintStmtArray(&state, &statements);
  popScope(&state);
  free(state.scopes);

  freeStatements(&statements);
  free(source);
  return issues + state.issues;
}

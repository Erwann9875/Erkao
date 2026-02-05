#include "pipeline_sema.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  Token token;
  FrontendNodeKind kind;
} SemaTopLevelName;

static bool semaFail(const FrontendUnit* frontend, const Token* token,
                     bool* hadError, const char* message) {
  if (hadError) *hadError = true;
  if (!message) return false;

  const char* path = frontend && frontend->path && frontend->path[0] != '\0'
      ? frontend->path
      : "<unknown>";
  if (token && token->line > 0 && token->column > 0) {
    fprintf(stderr, "[%s:%d:%d] Sema pipeline error: %s\n",
            path, token->line, token->column, message);
  } else {
    fprintf(stderr, "[%s] Sema pipeline error: %s\n", path, message);
  }
  return false;
}

static bool semaFailDuplicateName(const FrontendUnit* frontend, const Token* token,
                                  bool* hadError) {
  if (hadError) *hadError = true;
  const char* path = frontend && frontend->path && frontend->path[0] != '\0'
      ? frontend->path
      : "<unknown>";
  if (token && token->line > 0 && token->column > 0 && token->start) {
    fprintf(stderr,
            "[%s:%d:%d] Sema pipeline error: Duplicate top-level declaration '%.*s'.\n",
            path, token->line, token->column, token->length, token->start);
  } else {
    fprintf(stderr, "[%s] Sema pipeline error: Duplicate top-level declaration.\n", path);
  }
  return false;
}

static FrontendNodeKind semaNodeKindFromToken(ErkaoTokenType type) {
  switch (type) {
    case TOKEN_IMPORT: return FRONTEND_NODE_IMPORT;
    case TOKEN_EXPORT: return FRONTEND_NODE_EXPORT;
    case TOKEN_PRIVATE: return FRONTEND_NODE_PRIVATE;
    case TOKEN_LET: return FRONTEND_NODE_LET;
    case TOKEN_CONST: return FRONTEND_NODE_CONST;
    case TOKEN_FUN: return FRONTEND_NODE_FUNCTION;
    case TOKEN_CLASS: return FRONTEND_NODE_CLASS;
    case TOKEN_STRUCT: return FRONTEND_NODE_STRUCT;
    case TOKEN_ENUM: return FRONTEND_NODE_ENUM;
    case TOKEN_INTERFACE: return FRONTEND_NODE_INTERFACE;
    case TOKEN_TYPE_KW: return FRONTEND_NODE_TYPE_ALIAS;
    default: return FRONTEND_NODE_STATEMENT;
  }
}

static bool semaTokenEquals(Token a, Token b) {
  if (a.length != b.length) return false;
  if (a.length <= 0) return true;
  if (!a.start || !b.start) return false;
  return memcmp(a.start, b.start, (size_t)a.length) == 0;
}

static bool semaExtractDeclaredName(const FrontendUnit* frontend,
                                    const FrontendNode* node, Token* out) {
  if (!frontend || !node || !out) return false;
  switch (node->kind) {
    case FRONTEND_NODE_LET:
    case FRONTEND_NODE_CONST:
    case FRONTEND_NODE_FUNCTION:
    case FRONTEND_NODE_CLASS:
    case FRONTEND_NODE_STRUCT:
    case FRONTEND_NODE_ENUM:
    case FRONTEND_NODE_INTERFACE:
    case FRONTEND_NODE_TYPE_ALIAS:
      break;
    default:
      return false;
  }
  if (!frontend->tokens || !frontend->tokens->tokens) return false;
  int nameIndex = node->startToken + 1;
  if (nameIndex < 0 || nameIndex >= frontend->tokens->count) return false;
  Token token = frontend->tokens->tokens[nameIndex];
  if (token.type != TOKEN_IDENTIFIER) return false;
  *out = token;
  return true;
}

static bool semaValidateAstShape(const FrontendUnit* frontend, bool* hadError) {
  if (!frontend || !frontend->tokens || !frontend->tokens->tokens) {
    return semaFail(frontend, NULL, hadError,
                    "Frontend AST validation requires token stream.");
  }

  int previousEnd = -1;
  for (int i = 0; i < frontend->ast.count; i++) {
    FrontendNode* node = &frontend->ast.nodes[i];
    if (node->startToken < 0 || node->endToken < node->startToken ||
        node->endToken >= frontend->tokens->count) {
      return semaFail(frontend, &node->anchor, hadError,
                      "Frontend AST contains invalid token span.");
    }
    if (node->startToken <= previousEnd) {
      return semaFail(frontend, &node->anchor, hadError,
                      "Frontend AST contains overlapping nodes.");
    }

    Token startToken = frontend->tokens->tokens[node->startToken];
    FrontendNodeKind expectedKind = semaNodeKindFromToken(startToken.type);
    if (node->kind != expectedKind) {
      return semaFail(frontend, &startToken, hadError,
                      "Frontend AST node kind does not match token.");
    }

    previousEnd = node->endToken;
  }
  return true;
}

static bool semaCheckVisibilityPlacement(const FrontendUnit* frontend,
                                         bool* hadError) {
  if (!frontend || !frontend->tokens || !frontend->tokens->tokens) {
    return semaFail(frontend, NULL, hadError,
                    "Sema visibility checks require token stream.");
  }

  int braceDepth = 0;
  for (int i = 0; i < frontend->tokens->count; i++) {
    Token token = frontend->tokens->tokens[i];
    if (token.type == TOKEN_EXPORT && braceDepth > 0) {
      return semaFail(frontend, &token, hadError,
                      "Export declarations must be top level.");
    }
    if (token.type == TOKEN_PRIVATE && braceDepth > 0) {
      return semaFail(frontend, &token, hadError,
                      "Private declarations must be at top level.");
    }
    if (token.type == TOKEN_LEFT_BRACE) {
      braceDepth++;
    } else if (token.type == TOKEN_RIGHT_BRACE && braceDepth > 0) {
      braceDepth--;
    }
  }
  return true;
}

static bool semaCheckDuplicateTopLevelNames(const FrontendUnit* frontend,
                                            bool* hadError) {
  SemaTopLevelName* names = NULL;
  int count = 0;
  int capacity = 0;

  for (int i = 0; i < frontend->ast.count; i++) {
    FrontendNode* node = &frontend->ast.nodes[i];
    Token nameToken;
    if (!semaExtractDeclaredName(frontend, node, &nameToken)) {
      continue;
    }

    for (int j = 0; j < count; j++) {
      if (semaTokenEquals(names[j].token, nameToken)) {
        free(names);
        return semaFailDuplicateName(frontend, &nameToken, hadError);
      }
    }

    if (capacity < count + 1) {
      int nextCapacity = capacity == 0 ? 16 : capacity * 2;
      SemaTopLevelName* next = (SemaTopLevelName*)realloc(
          names, sizeof(SemaTopLevelName) * (size_t)nextCapacity);
      if (!next) {
        free(names);
        return semaFail(frontend, &nameToken, hadError,
                        "Out of memory while analyzing declarations.");
      }
      names = next;
      capacity = nextCapacity;
    }

    names[count].token = nameToken;
    names[count].kind = node->kind;
    count++;
  }

  free(names);
  return true;
}

static void semaCountTopLevelNode(FrontendNodeKind kind,
                                  int* imports,
                                  int* exports,
                                  int* privates,
                                  int* lets,
                                  int* consts,
                                  int* functions,
                                  int* classes,
                                  int* structs,
                                  int* enums,
                                  int* interfaces,
                                  int* typeAliases,
                                  int* statements) {
  switch (kind) {
    case FRONTEND_NODE_IMPORT: (*imports)++; break;
    case FRONTEND_NODE_EXPORT: (*exports)++; break;
    case FRONTEND_NODE_PRIVATE: (*privates)++; break;
    case FRONTEND_NODE_LET: (*lets)++; break;
    case FRONTEND_NODE_CONST: (*consts)++; break;
    case FRONTEND_NODE_FUNCTION: (*functions)++; break;
    case FRONTEND_NODE_CLASS: (*classes)++; break;
    case FRONTEND_NODE_STRUCT: (*structs)++; break;
    case FRONTEND_NODE_ENUM: (*enums)++; break;
    case FRONTEND_NODE_INTERFACE: (*interfaces)++; break;
    case FRONTEND_NODE_TYPE_ALIAS: (*typeAliases)++; break;
    case FRONTEND_NODE_STATEMENT: (*statements)++; break;
    default: break;
  }
}

bool semaBuildUnit(const FrontendUnit* frontend, SemaUnit* out, bool* hadError) {
  if (hadError) *hadError = false;
  if (!frontend || !out) {
    if (hadError) *hadError = true;
    return false;
  }

  if (!semaValidateAstShape(frontend, hadError)) return false;
  if (!semaCheckVisibilityPlacement(frontend, hadError)) return false;
  if (!semaCheckDuplicateTopLevelNames(frontend, hadError)) return false;

  memset(out, 0, sizeof(*out));
  out->frontend = *frontend;
  out->featureFlags = frontend->featureFlags;
  out->astNodeCount = frontend->ast.count;

  int lets = 0;
  int consts = 0;
  int functions = 0;
  int classes = 0;
  int structs = 0;
  int enums = 0;
  int interfaces = 0;
  int typeAliases = 0;
  for (int i = 0; i < frontend->ast.count; i++) {
    semaCountTopLevelNode(frontend->ast.nodes[i].kind,
                          &out->importCount,
                          &out->exportCount,
                          &out->privateCount,
                          &lets,
                          &consts,
                          &functions,
                          &classes,
                          &structs,
                          &enums,
                          &interfaces,
                          &typeAliases,
                          &out->topLevelStatementCount);
  }

  out->topLevelValueDeclCount =
      lets +
      consts +
      functions +
      classes +
      structs +
      enums;
  out->topLevelTypeDeclCount =
      typeAliases +
      interfaces +
      enums +
      structs +
      classes;
  out->topLevelDeclarationCount =
      out->importCount +
      out->exportCount +
      out->privateCount +
      lets +
      consts +
      functions +
      classes +
      structs +
      enums +
      interfaces +
      typeAliases;

  out->moduleKind = (out->importCount > 0 || out->exportCount > 0)
      ? SEMA_MODULE_IMPORT_EXPORT
      : SEMA_MODULE_SCRIPT;
  out->hasControlFlow =
      (out->featureFlags &
       (FRONTEND_FEATURE_MATCH |
        FRONTEND_FEATURE_SWITCH |
        FRONTEND_FEATURE_LOOP |
        FRONTEND_FEATURE_TRY)) != 0;
  out->hasVisibilityModifiers =
      out->exportCount > 0 || out->privateCount > 0;
  return true;
}

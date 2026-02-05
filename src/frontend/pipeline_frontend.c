#include "pipeline_frontend.h"

#include <stdio.h>
#include <string.h>

static bool frontendFail(const char* path, const Token* token, bool* hadError,
                         const char* message) {
  if (hadError) *hadError = true;
  if (!message) return false;

  const char* displayPath = (path && path[0] != '\0') ? path : "<unknown>";
  if (token && token->line > 0 && token->column > 0) {
    fprintf(stderr, "[%s:%d:%d] Frontend pipeline error: %s\n",
            displayPath, token->line, token->column, message);
  } else {
    fprintf(stderr, "[%s] Frontend pipeline error: %s\n", displayPath, message);
  }
  return false;
}

static void frontendAstInit(FrontendAst* ast) {
  if (!ast) return;
  ast->nodes = NULL;
  ast->count = 0;
  ast->capacity = 0;
}

static void frontendAstFree(FrontendAst* ast) {
  if (!ast) return;
  free(ast->nodes);
  frontendAstInit(ast);
}

static bool frontendAstReserve(FrontendAst* ast, int needed) {
  if (!ast || needed <= 0) return false;
  if (ast->capacity >= needed) return true;

  int capacity = ast->capacity > 0 ? ast->capacity : 16;
  while (capacity < needed) {
    capacity *= 2;
  }
  FrontendNode* grown = (FrontendNode*)realloc(ast->nodes,
                                               sizeof(FrontendNode) *
                                               (size_t)capacity);
  if (!grown) return false;
  ast->nodes = grown;
  ast->capacity = capacity;
  return true;
}

static bool frontendAstAddNode(FrontendAst* ast, FrontendNode node) {
  if (!ast) return false;
  if (!frontendAstReserve(ast, ast->count + 1)) return false;
  ast->nodes[ast->count++] = node;
  return true;
}

static void frontendTrackFeature(FrontendUnit* out, ErkaoTokenType type) {
  switch (type) {
    case TOKEN_IMPORT: out->featureFlags |= FRONTEND_FEATURE_IMPORT; break;
    case TOKEN_EXPORT: out->featureFlags |= FRONTEND_FEATURE_EXPORT; break;
    case TOKEN_PRIVATE: out->featureFlags |= FRONTEND_FEATURE_PRIVATE; break;
    case TOKEN_FUN: out->featureFlags |= FRONTEND_FEATURE_FUNCTION; break;
    case TOKEN_CLASS: out->featureFlags |= FRONTEND_FEATURE_CLASS; break;
    case TOKEN_STRUCT: out->featureFlags |= FRONTEND_FEATURE_STRUCT; break;
    case TOKEN_ENUM: out->featureFlags |= FRONTEND_FEATURE_ENUM; break;
    case TOKEN_INTERFACE: out->featureFlags |= FRONTEND_FEATURE_INTERFACE; break;
    case TOKEN_TYPE_KW: out->featureFlags |= FRONTEND_FEATURE_TYPE_ALIAS; break;
    case TOKEN_MATCH: out->featureFlags |= FRONTEND_FEATURE_MATCH; break;
    case TOKEN_SWITCH: out->featureFlags |= FRONTEND_FEATURE_SWITCH; break;
    case TOKEN_FOR:
    case TOKEN_FOREACH:
    case TOKEN_WHILE:
      out->featureFlags |= FRONTEND_FEATURE_LOOP;
      break;
    case TOKEN_TRY: out->featureFlags |= FRONTEND_FEATURE_TRY; break;
    case TOKEN_YIELD: out->featureFlags |= FRONTEND_FEATURE_YIELD; break;
    default: break;
  }
}

static FrontendNodeKind frontendNodeKindFromToken(ErkaoTokenType type) {
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

static void frontendTrackTopLevelNode(FrontendUnit* out, FrontendNodeKind kind) {
  switch (kind) {
    case FRONTEND_NODE_IMPORT: out->topLevel.imports++; break;
    case FRONTEND_NODE_EXPORT: out->topLevel.exports++; break;
    case FRONTEND_NODE_PRIVATE: out->topLevel.privates++; break;
    case FRONTEND_NODE_LET: out->topLevel.lets++; break;
    case FRONTEND_NODE_CONST: out->topLevel.consts++; break;
    case FRONTEND_NODE_FUNCTION: out->topLevel.functions++; break;
    case FRONTEND_NODE_CLASS: out->topLevel.classes++; break;
    case FRONTEND_NODE_STRUCT: out->topLevel.structs++; break;
    case FRONTEND_NODE_ENUM: out->topLevel.enums++; break;
    case FRONTEND_NODE_INTERFACE: out->topLevel.interfaces++; break;
    case FRONTEND_NODE_TYPE_ALIAS: out->topLevel.typeAliases++; break;
    case FRONTEND_NODE_STATEMENT:
    default: break;
  }
}

static int frontendScanTopLevelFormEnd(const TokenArray* tokens, int startIndex) {
  int parenDepth = 0;
  int braceDepth = 0;
  int bracketDepth = 0;
  int interpDepth = 0;

  for (int i = startIndex; i < tokens->count; i++) {
    ErkaoTokenType type = tokens->tokens[i].type;
    switch (type) {
      case TOKEN_LEFT_PAREN: parenDepth++; break;
      case TOKEN_RIGHT_PAREN:
        if (parenDepth > 0) parenDepth--;
        break;
      case TOKEN_LEFT_BRACE: braceDepth++; break;
      case TOKEN_RIGHT_BRACE:
        if (braceDepth > 0) braceDepth--;
        break;
      case TOKEN_LEFT_BRACKET: bracketDepth++; break;
      case TOKEN_RIGHT_BRACKET:
        if (bracketDepth > 0) bracketDepth--;
        break;
      case TOKEN_INTERP_START: interpDepth++; break;
      case TOKEN_INTERP_END:
        if (interpDepth > 0) interpDepth--;
        break;
      default:
        break;
    }

    if (type == TOKEN_EOF) {
      return i > startIndex ? i - 1 : startIndex;
    }
    if (parenDepth != 0 || braceDepth != 0 || bracketDepth != 0 || interpDepth != 0) {
      continue;
    }
    if (type == TOKEN_SEMICOLON) {
      return i;
    }
    if (i > startIndex && type == TOKEN_RIGHT_BRACE) {
      int nextIndex = i + 1;
      while (nextIndex < tokens->count &&
             tokens->tokens[nextIndex].type == TOKEN_SEMICOLON) {
        nextIndex++;
      }
      if (nextIndex < tokens->count &&
          tokens->tokens[nextIndex].type == TOKEN_ELSE) {
        continue;
      }
      return i;
    }
  }
  return tokens->count - 1;
}

static bool frontendBuildAst(const TokenArray* tokens, FrontendUnit* out,
                             bool* hadError) {
  int index = 0;
  while (index < tokens->count) {
    Token token = tokens->tokens[index];
    if (token.type == TOKEN_EOF) break;
    if (token.type == TOKEN_SEMICOLON) {
      index++;
      continue;
    }

    FrontendNode node;
    node.kind = frontendNodeKindFromToken(token.type);
    node.startToken = index;
    node.endToken = frontendScanTopLevelFormEnd(tokens, index);
    if (node.endToken < node.startToken) {
      node.endToken = node.startToken;
    }
    node.anchor = token;

    if (!frontendAstAddNode(&out->ast, node)) {
      return frontendFail(out->path, &token, hadError,
                          "Out of memory while building frontend AST.");
    }

    frontendTrackTopLevelNode(out, node.kind);
    index = node.endToken + 1;
  }
  return true;
}

bool frontendBuildUnit(const TokenArray* tokens, const char* source,
                       const char* path, FrontendUnit* out, bool* hadError) {
  if (hadError) *hadError = false;
  if (!out || !tokens || !source) {
    return frontendFail(path, NULL, hadError, "Invalid frontend stage input.");
  }

  memset(out, 0, sizeof(*out));
  frontendAstInit(&out->ast);
  out->tokens = tokens;
  out->source = source;
  out->path = path;
  if (!tokens->tokens || tokens->count <= 0) {
    frontendAstFree(&out->ast);
    return frontendFail(path, NULL, hadError, "Token stream is empty.");
  }

  out->tokenCount = tokens->count;
  out->firstToken = tokens->tokens[0];
  out->lastToken = tokens->tokens[tokens->count - 1];
  if (out->lastToken.type != TOKEN_EOF) {
    frontendAstFree(&out->ast);
    return frontendFail(path, &out->lastToken, hadError,
                        "Token stream must terminate with EOF.");
  }

  int parenDepth = 0;
  int braceDepth = 0;
  int bracketDepth = 0;
  int interpDepth = 0;
  for (int i = 0; i < tokens->count; i++) {
    Token token = tokens->tokens[i];
    if (token.type == TOKEN_ERROR) {
      frontendAstFree(&out->ast);
      return frontendFail(path, &token, hadError,
                          "Unexpected TOKEN_ERROR in token stream.");
    }
    if (token.type == TOKEN_EOF && i != tokens->count - 1) {
      frontendAstFree(&out->ast);
      return frontendFail(path, &token, hadError,
                          "EOF token can only appear at stream end.");
    }

    frontendTrackFeature(out, token.type);
    if (token.type != TOKEN_EOF) {
      out->nonEofTokenCount++;
    }

    switch (token.type) {
      case TOKEN_LEFT_PAREN:
        parenDepth++;
        if (parenDepth > out->depthStats.maxParenDepth) {
          out->depthStats.maxParenDepth = parenDepth;
        }
        break;
      case TOKEN_RIGHT_PAREN:
        if (parenDepth > 0) parenDepth--;
        break;
      case TOKEN_LEFT_BRACE:
        braceDepth++;
        if (braceDepth > out->depthStats.maxBraceDepth) {
          out->depthStats.maxBraceDepth = braceDepth;
        }
        break;
      case TOKEN_RIGHT_BRACE:
        if (braceDepth > 0) braceDepth--;
        break;
      case TOKEN_LEFT_BRACKET:
        bracketDepth++;
        if (bracketDepth > out->depthStats.maxBracketDepth) {
          out->depthStats.maxBracketDepth = bracketDepth;
        }
        break;
      case TOKEN_RIGHT_BRACKET:
        if (bracketDepth > 0) bracketDepth--;
        break;
      case TOKEN_INTERP_START:
        interpDepth++;
        if (interpDepth > out->depthStats.maxInterpolationDepth) {
          out->depthStats.maxInterpolationDepth = interpDepth;
        }
        break;
      case TOKEN_INTERP_END:
        if (interpDepth > 0) interpDepth--;
        break;
      default:
        break;
    }
  }

  if (!frontendBuildAst(tokens, out, hadError)) {
    frontendAstFree(&out->ast);
    return false;
  }
  return true;
}

void frontendFreeUnit(FrontendUnit* unit) {
  if (!unit) return;
  frontendAstFree(&unit->ast);
}

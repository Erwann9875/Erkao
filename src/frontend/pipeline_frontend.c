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

static void frontendTrackTopLevel(FrontendUnit* out, ErkaoTokenType type) {
  switch (type) {
    case TOKEN_IMPORT: out->topLevel.imports++; break;
    case TOKEN_EXPORT: out->topLevel.exports++; break;
    case TOKEN_PRIVATE: out->topLevel.privates++; break;
    case TOKEN_LET: out->topLevel.lets++; break;
    case TOKEN_CONST: out->topLevel.consts++; break;
    case TOKEN_FUN: out->topLevel.functions++; break;
    case TOKEN_CLASS: out->topLevel.classes++; break;
    case TOKEN_STRUCT: out->topLevel.structs++; break;
    case TOKEN_ENUM: out->topLevel.enums++; break;
    case TOKEN_INTERFACE: out->topLevel.interfaces++; break;
    case TOKEN_TYPE_KW: out->topLevel.typeAliases++; break;
    default: break;
  }
}

bool frontendBuildUnit(const TokenArray* tokens, const char* source,
                       const char* path, FrontendUnit* out, bool* hadError) {
  if (hadError) *hadError = false;
  if (!out || !tokens || !source) {
    return frontendFail(path, NULL, hadError, "Invalid frontend stage input.");
  }

  memset(out, 0, sizeof(*out));
  out->tokens = tokens;
  out->source = source;
  out->path = path;
  if (!tokens->tokens || tokens->count <= 0) {
    return frontendFail(path, NULL, hadError, "Token stream is empty.");
  }

  out->tokenCount = tokens->count;
  out->firstToken = tokens->tokens[0];
  out->lastToken = tokens->tokens[tokens->count - 1];
  if (out->lastToken.type != TOKEN_EOF) {
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
      return frontendFail(path, &token, hadError,
                          "Unexpected TOKEN_ERROR in token stream.");
    }
    if (token.type == TOKEN_EOF && i != tokens->count - 1) {
      return frontendFail(path, &token, hadError,
                          "EOF token can only appear at stream end.");
    }

    frontendTrackFeature(out, token.type);
    if (token.type != TOKEN_EOF) {
      out->nonEofTokenCount++;
    }

    if (parenDepth == 0 && braceDepth == 0 &&
        bracketDepth == 0 && interpDepth == 0) {
      frontendTrackTopLevel(out, token.type);
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
  return true;
}

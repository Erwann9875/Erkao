#ifndef ERKAO_PIPELINE_FRONTEND_H
#define ERKAO_PIPELINE_FRONTEND_H

#include <stdbool.h>
#include <stdint.h>

#include "lexer.h"

typedef enum {
  FRONTEND_FEATURE_IMPORT = 1u << 0,
  FRONTEND_FEATURE_EXPORT = 1u << 1,
  FRONTEND_FEATURE_PRIVATE = 1u << 2,
  FRONTEND_FEATURE_FUNCTION = 1u << 3,
  FRONTEND_FEATURE_CLASS = 1u << 4,
  FRONTEND_FEATURE_STRUCT = 1u << 5,
  FRONTEND_FEATURE_ENUM = 1u << 6,
  FRONTEND_FEATURE_INTERFACE = 1u << 7,
  FRONTEND_FEATURE_TYPE_ALIAS = 1u << 8,
  FRONTEND_FEATURE_MATCH = 1u << 9,
  FRONTEND_FEATURE_SWITCH = 1u << 10,
  FRONTEND_FEATURE_LOOP = 1u << 11,
  FRONTEND_FEATURE_TRY = 1u << 12,
  FRONTEND_FEATURE_YIELD = 1u << 13
} FrontendFeatureFlags;

typedef enum {
  FRONTEND_NODE_IMPORT,
  FRONTEND_NODE_EXPORT,
  FRONTEND_NODE_PRIVATE,
  FRONTEND_NODE_LET,
  FRONTEND_NODE_CONST,
  FRONTEND_NODE_FUNCTION,
  FRONTEND_NODE_CLASS,
  FRONTEND_NODE_STRUCT,
  FRONTEND_NODE_ENUM,
  FRONTEND_NODE_INTERFACE,
  FRONTEND_NODE_TYPE_ALIAS,
  FRONTEND_NODE_STATEMENT
} FrontendNodeKind;

typedef struct {
  FrontendNodeKind kind;
  int startToken;
  int endToken;
  Token anchor;
} FrontendNode;

typedef struct {
  FrontendNode* nodes;
  int count;
  int capacity;
} FrontendAst;

typedef struct {
  int maxParenDepth;
  int maxBraceDepth;
  int maxBracketDepth;
  int maxInterpolationDepth;
} FrontendDepthStats;

typedef struct {
  int imports;
  int exports;
  int privates;
  int lets;
  int consts;
  int functions;
  int classes;
  int structs;
  int enums;
  int interfaces;
  int typeAliases;
} FrontendTopLevelStats;

typedef struct {
  const TokenArray* tokens;
  const char* source;
  const char* path;
  Token firstToken;
  Token lastToken;
  int tokenCount;
  int nonEofTokenCount;
  uint32_t featureFlags;
  FrontendDepthStats depthStats;
  FrontendTopLevelStats topLevel;
  FrontendAst ast;
} FrontendUnit;

bool frontendBuildUnit(const TokenArray* tokens, const char* source,
                       const char* path, FrontendUnit* out, bool* hadError);
void frontendFreeUnit(FrontendUnit* unit);

#endif

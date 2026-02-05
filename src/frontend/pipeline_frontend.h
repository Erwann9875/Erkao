#ifndef ERKAO_PIPELINE_FRONTEND_H
#define ERKAO_PIPELINE_FRONTEND_H

#include <stdbool.h>

#include "lexer.h"

typedef struct {
  const TokenArray* tokens;
  const char* source;
  const char* path;
} FrontendUnit;

bool frontendBuildUnit(const TokenArray* tokens, const char* source,
                       const char* path, FrontendUnit* out, bool* hadError);

#endif

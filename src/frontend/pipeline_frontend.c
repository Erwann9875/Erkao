#include "pipeline_frontend.h"

bool frontendBuildUnit(const TokenArray* tokens, const char* source,
                       const char* path, FrontendUnit* out, bool* hadError) {
  if (hadError) *hadError = false;
  if (!out || !tokens || !source) {
    if (hadError) *hadError = true;
    return false;
  }

  out->tokens = tokens;
  out->source = source;
  out->path = path;
  return true;
}

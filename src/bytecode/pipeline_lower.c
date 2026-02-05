#include "pipeline_lower.h"

#include "singlepass_legacy.h"

#include <stdio.h>
#include <string.h>

static bool lowerFail(const SemaUnit* sema, const Token* token, bool* hadError,
                      const char* message) {
  if (hadError) *hadError = true;
  if (!message) return false;

  const char* path = "<unknown>";
  if (sema && sema->frontend.path && sema->frontend.path[0] != '\0') {
    path = sema->frontend.path;
  }
  if (token && token->line > 0 && token->column > 0) {
    fprintf(stderr, "[%s:%d:%d] Lower pipeline error: %s\n",
            path, token->line, token->column, message);
  } else {
    fprintf(stderr, "[%s] Lower pipeline error: %s\n", path, message);
  }
  return false;
}

bool lowerBuildUnit(const SemaUnit* sema, LowerUnit* out, bool* hadError) {
  if (hadError) *hadError = false;
  if (!sema || !out) {
    if (hadError) *hadError = true;
    return false;
  }
  if (!sema->frontend.tokens || !sema->frontend.source) {
    return lowerFail(sema, NULL, hadError,
                     "Sema unit is missing frontend token/source payload.");
  }
  if (sema->frontend.ast.count < 0) {
    return lowerFail(sema, NULL, hadError,
                     "Sema unit contains invalid AST node count.");
  }
  memset(out, 0, sizeof(*out));
  out->sema = *sema;
  return true;
}

ObjFunction* lowerEmitBytecode(VM* vm, const LowerUnit* lower, bool* hadError) {
  bool localHadError = false;
  bool* hadErrorOut = hadError ? hadError : &localHadError;
  *hadErrorOut = false;

  if (!vm || !lower) {
    *hadErrorOut = true;
    return NULL;
  }

  ObjFunction* function = compileSinglePassLegacyBody(
      vm,
      lower->sema.frontend.tokens,
      lower->sema.frontend.source,
      lower->sema.frontend.path,
      hadErrorOut);
  if (!function || *hadErrorOut) return function;

  compileSinglePassLegacyFinalize(function);
  compileSinglePassLegacyOptimize(vm, function);
  return function;
}

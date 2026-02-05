#include "singlepass.h"
#include "singlepass_legacy.h"
#include "pipeline_frontend.h"
#include "pipeline_sema.h"

ObjFunction* compile(VM* vm, const TokenArray* tokens, const char* source,
                     const char* path, bool* hadError) {
  bool localHadError = false;
  bool* hadErrorOut = hadError ? hadError : &localHadError;
  *hadErrorOut = false;
  if (!vm || !tokens || !source) {
    *hadErrorOut = true;
    return NULL;
  }

  bool stageError = false;
  FrontendUnit frontend;
  if (!frontendBuildUnit(tokens, source, path, &frontend, &stageError)) {
    *hadErrorOut = stageError;
    return NULL;
  }

  SemaUnit sema;
  if (!semaBuildUnit(&frontend, &sema, &stageError)) {
    *hadErrorOut = stageError;
    return NULL;
  }

  return compileSinglePassLegacy(vm, sema.frontend.tokens, sema.frontend.source,
                                 sema.frontend.path, hadErrorOut);
}

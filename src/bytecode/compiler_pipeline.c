#include "singlepass.h"
#include "singlepass_legacy.h"
#include "pipeline_frontend.h"
#include "pipeline_sema.h"

ObjFunction* compile(VM* vm, const TokenArray* tokens, const char* source,
                     const char* path, bool* hadError) {
  bool localHadError = false;
  bool* hadErrorOut = hadError ? hadError : &localHadError;
  *hadErrorOut = false;

  bool stageError = false;
  FrontendUnit frontend;
  if (!frontendBuildUnit(tokens, source, path, &frontend, &stageError)) {
    *hadErrorOut = true;
    return NULL;
  }

  SemaUnit sema;
  if (!semaBuildUnit(&frontend, &sema, &stageError)) {
    *hadErrorOut = true;
    return NULL;
  }

  return compileSinglePassLegacy(vm, sema.unit.tokens, sema.unit.source,
                                 sema.unit.path, hadErrorOut);
}

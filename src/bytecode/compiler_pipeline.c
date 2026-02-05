#include "singlepass.h"
#include "pipeline_frontend.h"
#include "pipeline_sema.h"
#include "pipeline_lower.h"

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
    frontendFreeUnit(&frontend);
    *hadErrorOut = stageError;
    return NULL;
  }

  LowerUnit lower;
  if (!lowerBuildUnit(&sema, &lower, &stageError)) {
    frontendFreeUnit(&frontend);
    *hadErrorOut = stageError;
    return NULL;
  }

  ObjFunction* function = lowerEmitBytecode(vm, &lower, hadErrorOut);
  frontendFreeUnit(&frontend);
  return function;
}

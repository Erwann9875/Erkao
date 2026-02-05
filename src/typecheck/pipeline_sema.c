#include "pipeline_sema.h"

bool semaBuildUnit(const FrontendUnit* frontend, SemaUnit* out, bool* hadError) {
  if (hadError) *hadError = false;
  if (!frontend || !out) {
    if (hadError) *hadError = true;
    return false;
  }

  out->unit = *frontend;
  return true;
}

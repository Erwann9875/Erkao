#ifndef ERKAO_PIPELINE_LOWER_H
#define ERKAO_PIPELINE_LOWER_H

#include <stdbool.h>

#include "singlepass.h"
#include "pipeline_sema.h"

typedef struct {
  SemaUnit sema;
} LowerUnit;

bool lowerBuildUnit(const SemaUnit* sema, LowerUnit* out, bool* hadError);
ObjFunction* lowerEmitBytecode(VM* vm, const LowerUnit* lower, bool* hadError);

#endif

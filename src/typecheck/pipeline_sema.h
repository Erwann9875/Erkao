#ifndef ERKAO_PIPELINE_SEMA_H
#define ERKAO_PIPELINE_SEMA_H

#include <stdbool.h>

#include "pipeline_frontend.h"

typedef struct {
  FrontendUnit unit;
} SemaUnit;

bool semaBuildUnit(const FrontendUnit* frontend, SemaUnit* out, bool* hadError);

#endif

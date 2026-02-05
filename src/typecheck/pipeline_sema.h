#ifndef ERKAO_PIPELINE_SEMA_H
#define ERKAO_PIPELINE_SEMA_H

#include <stdbool.h>
#include <stdint.h>

#include "pipeline_frontend.h"

typedef enum {
  SEMA_MODULE_SCRIPT = 0,
  SEMA_MODULE_IMPORT_EXPORT = 1
} SemaModuleKind;

typedef struct {
  FrontendUnit frontend;
  SemaModuleKind moduleKind;
  uint32_t featureFlags;
  int astNodeCount;
  int topLevelStatementCount;
  int topLevelDeclarationCount;
  int topLevelValueDeclCount;
  int topLevelTypeDeclCount;
  int importCount;
  int exportCount;
  int privateCount;
  bool hasControlFlow;
  bool hasVisibilityModifiers;
} SemaUnit;

bool semaBuildUnit(const FrontendUnit* frontend, SemaUnit* out, bool* hadError);

#endif

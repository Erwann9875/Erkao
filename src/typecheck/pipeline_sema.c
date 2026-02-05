#include "pipeline_sema.h"

#include <string.h>

bool semaBuildUnit(const FrontendUnit* frontend, SemaUnit* out, bool* hadError) {
  if (hadError) *hadError = false;
  if (!frontend || !out) {
    if (hadError) *hadError = true;
    return false;
  }

  memset(out, 0, sizeof(*out));
  out->frontend = *frontend;
  out->featureFlags = frontend->featureFlags;
  out->importCount = frontend->topLevel.imports;
  out->exportCount = frontend->topLevel.exports;
  out->privateCount = frontend->topLevel.privates;

  out->topLevelValueDeclCount =
      frontend->topLevel.lets +
      frontend->topLevel.consts +
      frontend->topLevel.functions +
      frontend->topLevel.classes +
      frontend->topLevel.structs +
      frontend->topLevel.enums;
  out->topLevelTypeDeclCount =
      frontend->topLevel.typeAliases +
      frontend->topLevel.interfaces +
      frontend->topLevel.enums +
      frontend->topLevel.structs +
      frontend->topLevel.classes;
  out->topLevelDeclarationCount =
      out->importCount +
      out->exportCount +
      out->privateCount +
      frontend->topLevel.lets +
      frontend->topLevel.consts +
      frontend->topLevel.functions +
      frontend->topLevel.classes +
      frontend->topLevel.structs +
      frontend->topLevel.enums +
      frontend->topLevel.interfaces +
      frontend->topLevel.typeAliases;

  out->moduleKind = (out->importCount > 0 || out->exportCount > 0)
      ? SEMA_MODULE_IMPORT_EXPORT
      : SEMA_MODULE_SCRIPT;
  out->hasControlFlow =
      (out->featureFlags &
       (FRONTEND_FEATURE_MATCH |
        FRONTEND_FEATURE_SWITCH |
        FRONTEND_FEATURE_LOOP |
        FRONTEND_FEATURE_TRY)) != 0;
  out->hasVisibilityModifiers =
      out->exportCount > 0 || out->privateCount > 0;
  return true;
}

#ifndef ERKAO_SINGLEPASS_LEGACY_H
#define ERKAO_SINGLEPASS_LEGACY_H

#include "singlepass.h"

ObjFunction* compileSinglePassLegacy(VM* vm, const TokenArray* tokens,
                                     const char* source, const char* path,
                                     bool* hadError);
ObjFunction* compileSinglePassLegacyBody(VM* vm, const TokenArray* tokens,
                                         const char* source, const char* path,
                                         bool* hadError);
void compileSinglePassLegacyFinalize(ObjFunction* function);
ObjFunction* compileSinglePassLegacyUnoptimized(VM* vm, const TokenArray* tokens,
                                                const char* source,
                                                const char* path,
                                                bool* hadError);
void compileSinglePassLegacyOptimize(VM* vm, ObjFunction* function);

#endif

#ifndef ERKAO_SINGLEPASS_H
#define ERKAO_SINGLEPASS_H

#include "value.h"
#include "lexer.h"

ObjFunction* compile(VM* vm, const TokenArray* tokens, const char* source,
                     const char* path, bool* hadError);

#endif

#ifndef ERKAO_PARSER_H
#define ERKAO_PARSER_H

#include "ast.h"
#include "lexer.h"

bool parseTokens(const TokenArray* tokens, const char* source, const char* path,
                 StmtArray* outStatements);

#endif

#ifndef ERKAO_PARSER_H
#define ERKAO_PARSER_H

#include "ast.h"
#include "lexer.h"

bool parseTokens(const TokenArray* tokens, StmtArray* outStatements);

#endif

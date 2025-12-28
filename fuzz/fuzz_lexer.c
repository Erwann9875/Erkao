#include "lexer.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  if (!data) return 0;
  char* source = (char*)malloc(size + 1);
  if (!source) return 0;
  memcpy(source, data, size);
  source[size] = '\0';

  bool hadError = false;
  TokenArray tokens = scanTokens(source, "<fuzz>", &hadError);
  freeTokenArray(&tokens);
  free(source);
  return 0;
}

#include "interpreter.h"
#include "lexer.h"
#include "singlepass.h"

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

  VM vm;
  vmInit(&vm);

  bool lexError = false;
  TokenArray tokens = scanTokens(source, "<fuzz>", &lexError);
  if (!lexError) {
    bool compileError = false;
    (void)compile(&vm, &tokens, source, "<fuzz>", &compileError);
  }

  freeTokenArray(&tokens);
  free(source);
  vmFree(&vm);
  return 0;
}

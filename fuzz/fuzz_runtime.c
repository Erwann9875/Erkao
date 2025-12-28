#include "interpreter.h"
#include "lexer.h"
#include "program.h"
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
  vm.instructionBudget = 5000;
  vm.maxHeapBytes = 4 * 1024 * 1024;
  vm.maxFrames = 32;
  vm.maxStackSlots = 2048;
  defineGlobal(&vm, "http", NULL_VAL);

  bool lexError = false;
  TokenArray tokens = scanTokens(source, "<fuzz>", &lexError);
  if (!lexError) {
    bool compileError = false;
    ObjFunction* function = compile(&vm, &tokens, source, "<fuzz>", &compileError);
    if (!compileError && function) {
      Program* program = programCreate(&vm, source, "<fuzz>", function);
      function->program = program;
      programRetain(program);
      interpret(&vm, program);
    } else {
      free(source);
    }
  } else {
    free(source);
  }

  freeTokenArray(&tokens);
  vmFree(&vm);
  return 0;
}

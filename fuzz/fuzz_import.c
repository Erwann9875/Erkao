#include "interpreter_internal.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static char* copyBytes(const uint8_t* data, size_t size) {
  char* out = (char*)malloc(size + 1);
  if (!out) return NULL;
  if (size > 0) {
    memcpy(out, data, size);
  }
  out[size] = '\0';
  return out;
}

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  if (!data) return 0;

  size_t split = size == 0 ? 0 : ((size_t)data[0] % (size + 1));
  const uint8_t* leftData = data + (size > 0 ? 1 : 0);
  size_t leftSize = split > 0 ? split - 1 : 0;
  if (leftSize > size) leftSize = size;
  size_t rightOffset = leftSize + (size > 0 ? 1 : 0);
  if (rightOffset > size) rightOffset = size;
  size_t rightSize = size - rightOffset;

  char* currentPath = copyBytes(leftData, leftSize);
  char* importPath = copyBytes(data + rightOffset, rightSize);
  if (!currentPath || !importPath) {
    free(currentPath);
    free(importPath);
    return 0;
  }

  VM vm;
  vmInit(&vm);

  const char* currentArg = currentPath[0] == '\0' ? NULL : currentPath;
  const char* importArg = importPath[0] == '\0' ? NULL : importPath;
  char* resolved = resolveImportPath(&vm, currentArg, importArg);
  free(resolved);

  vmFree(&vm);
  free(currentPath);
  free(importPath);
  return 0;
}

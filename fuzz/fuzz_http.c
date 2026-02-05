#include "http_internal.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  char* text = (char*)malloc(size + 1);
  if (!text) return 0;
  if (size > 0 && data) {
    memcpy(text, data, size);
  }
  text[size] = '\0';

  size_t headerEnd = 0;
  (void)erkaoHttpFindHeaderEnd(text, size, &headerEnd);

  const char* method = NULL;
  size_t methodLen = 0;
  const char* path = NULL;
  size_t pathLen = 0;
  if (headerEnd == 0 || headerEnd > size) {
    headerEnd = size;
  }
  (void)erkaoHttpParseRequestLine(text, headerEnd, &method, &methodLen, &path, &pathLen);

  if (method && methodLen > 0) {
    char* methodCopy = (char*)malloc(methodLen + 1);
    if (methodCopy) {
      memcpy(methodCopy, method, methodLen);
      methodCopy[methodLen] = '\0';
      (void)erkaoHttpHeaderNameSafe(methodCopy);
      free(methodCopy);
    }
  }

  if (path && pathLen > 0) {
    char* pathCopy = (char*)malloc(pathLen + 1);
    if (pathCopy) {
      memcpy(pathCopy, path, pathLen);
      pathCopy[pathLen] = '\0';
      (void)erkaoHttpHeaderValueSafe(pathCopy);
      free(pathCopy);
    }
  }

  (void)erkaoHttpStringEqualsIgnoreCaseN("Content-Type", 12, "content-type");

  free(text);
  return 0;
}

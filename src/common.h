#ifndef ERKAO_COMMON_H
#define ERKAO_COMMON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <ctype.h>

#define ERK_MAX_ARGS 255

#define GROW_CAPACITY(capacity) ((capacity) < 8 ? 8 : (capacity) * 2)

#define GROW_ARRAY(type, pointer, oldCount, newCount) \
  (type*)realloc((pointer), sizeof(type) * (newCount))

#define FREE_ARRAY(type, pointer, oldCount) \
  do { (void)(oldCount); free(pointer); } while (0)

static inline void printErrorContext(const char* source, int line, int column, int length) {
  if (!source || line <= 0 || column <= 0) return;

  const char* lineStart = source;
  int currentLine = 1;
  while (*lineStart && currentLine < line) {
    if (*lineStart == '\n') currentLine++;
    lineStart++;
  }

  if (currentLine != line) return;

  const char* lineEnd = lineStart;
  while (*lineEnd && *lineEnd != '\n') lineEnd++;
  if (lineEnd > lineStart && lineEnd[-1] == '\r') lineEnd--;

  int lineLength = (int)(lineEnd - lineStart);
  int underlineStart = column;
  if (underlineStart < 1) underlineStart = 1;
  if (underlineStart > lineLength + 1) underlineStart = lineLength + 1;

  int underlineLength = length > 0 ? length : 1;
  if (lineLength == 0) {
    underlineLength = 1;
  } else if (underlineStart + underlineLength - 1 > lineLength) {
    underlineLength = lineLength - underlineStart + 1;
    if (underlineLength < 1) underlineLength = 1;
  }

  fprintf(stderr, "  %.*s\n", lineLength, lineStart);
  fprintf(stderr, "  %*s", underlineStart - 1, "");
  fputc('^', stderr);
  for (int i = 1; i < underlineLength; i++) {
    fputc('~', stderr);
  }
  fputc('\n', stderr);
}

#endif

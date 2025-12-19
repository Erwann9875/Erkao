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

static inline void printErrorContext(const char* source, int line, int column) {
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
  int caretColumn = column;
  if (caretColumn < 1) caretColumn = 1;
  if (caretColumn > lineLength + 1) caretColumn = lineLength + 1;

  fprintf(stderr, "  %.*s\n", lineLength, lineStart);
  fprintf(stderr, "  %*s^\n", caretColumn - 1, "");
}

#endif

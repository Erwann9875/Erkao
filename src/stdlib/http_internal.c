#include "http_internal.h"

#include <ctype.h>
#include <string.h>

bool erkaoHttpFindHeaderEnd(const char* data, size_t length, size_t* outIndex) {
  if (length < 2) return false;
  for (size_t i = 3; i < length; i++) {
    if (data[i - 3] == '\r' && data[i - 2] == '\n' &&
        data[i - 1] == '\r' && data[i] == '\n') {
      *outIndex = i + 1;
      return true;
    }
  }
  for (size_t i = 1; i < length; i++) {
    if (data[i - 1] == '\n' && data[i] == '\n') {
      *outIndex = i + 1;
      return true;
    }
  }
  return false;
}

bool erkaoHttpParseRequestLine(const char* data, size_t headerEnd,
                               const char** method, size_t* methodLen,
                               const char** path, size_t* pathLen) {
  if (!data || headerEnd == 0) return false;
  const char* lineEnd = memchr(data, '\n', headerEnd);
  if (!lineEnd) return false;
  const char* lineEndClean = lineEnd;
  if (lineEndClean > data && lineEndClean[-1] == '\r') {
    lineEndClean--;
  }
  const char* space1 = memchr(data, ' ', (size_t)(lineEndClean - data));
  if (!space1) return false;
  const char* space2 = memchr(space1 + 1, ' ', (size_t)(lineEndClean - (space1 + 1)));
  if (!space2) return false;
  if (space1 == data || space2 == space1 + 1) return false;
  *method = data;
  *methodLen = (size_t)(space1 - data);
  *path = space1 + 1;
  *pathLen = (size_t)(space2 - (space1 + 1));
  return true;
}

bool erkaoHttpStringEqualsIgnoreCaseN(const char* left, int leftLen, const char* right) {
  int rightLen = (int)strlen(right);
  if (leftLen != rightLen) return false;
  for (int i = 0; i < leftLen; i++) {
    unsigned char a = (unsigned char)left[i];
    unsigned char b = (unsigned char)right[i];
    if (tolower(a) != tolower(b)) return false;
  }
  return true;
}

static bool httpHeaderTokenChar(char c) {
  if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
    return true;
  }
  switch (c) {
    case '!':
    case '#':
    case '$':
    case '%':
    case '&':
    case '\'':
    case '*':
    case '+':
    case '-':
    case '.':
    case '^':
    case '_':
    case '`':
    case '|':
    case '~':
      return true;
    default:
      return false;
  }
}

bool erkaoHttpHeaderNameSafe(const char* name) {
  if (!name || name[0] == '\0') return false;
  for (const char* c = name; *c; c++) {
    if (!httpHeaderTokenChar(*c)) return false;
  }
  return true;
}

bool erkaoHttpHeaderValueSafe(const char* value) {
  if (!value) return false;
  for (const char* c = value; *c; c++) {
    if (*c == '\r' || *c == '\n') return false;
  }
  return true;
}

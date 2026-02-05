#ifndef ERKAO_HTTP_INTERNAL_H
#define ERKAO_HTTP_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>

bool erkaoHttpFindHeaderEnd(const char* data, size_t length, size_t* outIndex);
bool erkaoHttpParseRequestLine(const char* data, size_t headerEnd,
                               const char** method, size_t* methodLen,
                               const char** path, size_t* pathLen);
bool erkaoHttpStringEqualsIgnoreCaseN(const char* left, int leftLen, const char* right);
bool erkaoHttpHeaderNameSafe(const char* name);
bool erkaoHttpHeaderValueSafe(const char* value);

#endif

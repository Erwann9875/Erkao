#ifndef ERKAO_STDLIB_INTERNAL_H
#define ERKAO_STDLIB_INTERNAL_H

#include "interpreter_internal.h"

typedef struct {
  char* data;
  size_t length;
  size_t capacity;
  bool failed;
} ByteBuffer;

Value runtimeErrorValue(VM* vm, const char* message);

ObjInstance* makeModule(VM* vm, const char* name);
void moduleAdd(VM* vm, ObjInstance* module, const char* name, NativeFn fn, int arity);
void moduleAddValue(VM* vm, ObjInstance* module, const char* name, Value value);

const char* findLastSeparator(const char* path);
bool isAbsolutePathString(const char* path);
char pickSeparator(const char* left, const char* right);

bool pathExists(const char* path);
bool pathIsDir(const char* path);
bool pathIsFile(const char* path);

Value stdlibCwdValue(VM* vm);

void bufferInit(ByteBuffer* buffer);
void bufferEnsure(ByteBuffer* buffer, size_t needed);
void bufferAppendN(ByteBuffer* buffer, const char* data, size_t length);
void bufferAppendChar(ByteBuffer* buffer, char c);
void bufferFree(ByteBuffer* buffer);

char* copyCString(const char* src, size_t length);

typedef struct {
  char** items;
  int count;
  int capacity;
  bool failed;
} StringList;

void stringListInit(StringList* list);
void stringListFree(StringList* list);
void stringListAdd(StringList* list, const char* value);
void stringListAddWithLength(StringList* list, const char* value, size_t length);
void stringListSort(StringList* list);

bool numberIsFinite(double value);
bool stdlibUnsafeEnabled(VM* vm, unsigned int featureFlag, const char* featureEnv);

#endif

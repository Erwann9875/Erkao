#include "stdlib_internal.h"
#include "platform.h"

#include <float.h>
#include <math.h>
#include <string.h>

Value runtimeErrorValue(VM* vm, const char* message) {
  Token token;
  memset(&token, 0, sizeof(Token));
  runtimeError(vm, token, message);
  return NULL_VAL;
}

ObjInstance* makeModule(VM* vm, const char* name) {
  ObjString* className = copyString(vm, name);
  ObjMap* methods = newMap(vm);
  ObjClass* klass = newClass(vm, className, methods);
  ObjInstance* module = newInstance(vm, klass);
  ObjString* defaultKey = copyString(vm, "default");
  mapSet(module->fields, defaultKey, OBJ_VAL(module));
  return module;
}

void moduleAdd(VM* vm, ObjInstance* module, const char* name, NativeFn fn, int arity) {
  ObjString* fieldName = copyString(vm, name);
  ObjNative* native = newNative(vm, fn, arity, fieldName);
  mapSet(module->fields, fieldName, OBJ_VAL(native));
}

void moduleAddValue(VM* vm, ObjInstance* module, const char* name, Value value) {
  ObjString* fieldName = copyString(vm, name);
  mapSet(module->fields, fieldName, value);
}

const char* findLastSeparator(const char* path) {
  const char* lastSlash = strrchr(path, '/');
  const char* lastBackslash = strrchr(path, '\\');
  if (!lastSlash) return lastBackslash;
  if (!lastBackslash) return lastSlash;
  return lastSlash > lastBackslash ? lastSlash : lastBackslash;
}

bool isAbsolutePathString(const char* path) {
  return platform_path_is_absolute(path);
}

char pickSeparator(const char* left, const char* right) {
  return platform_path_pick_separator(left, right);
}

bool pathExists(const char* path) {
  return platform_path_exists(path);
}

bool pathIsDir(const char* path) {
  return platform_path_is_dir(path);
}

bool pathIsFile(const char* path) {
  return platform_path_is_file(path);
}

Value stdlibCwdValue(VM* vm) {
  char* buffer = platform_get_cwd();
  if (!buffer) {
    return runtimeErrorValue(vm, "fs.cwd failed to read current directory.");
  }
  ObjString* result = copyString(vm, buffer);
  free(buffer);
  return OBJ_VAL(result);
}

void bufferInit(ByteBuffer* buffer) {
  buffer->data = NULL;
  buffer->length = 0;
  buffer->capacity = 0;
}

void bufferEnsure(ByteBuffer* buffer, size_t needed) {
  if (buffer->capacity >= needed) return;
  size_t oldCapacity = buffer->capacity;
  size_t newCapacity = oldCapacity == 0 ? 128 : oldCapacity;
  while (newCapacity < needed) {
    newCapacity *= 2;
  }
  char* next = (char*)realloc(buffer->data, newCapacity);
  if (!next) {
    fprintf(stderr, "Out of memory.\n");
    exit(1);
  }
  buffer->data = next;
  buffer->capacity = newCapacity;
}

void bufferAppendN(ByteBuffer* buffer, const char* data, size_t length) {
  bufferEnsure(buffer, buffer->length + length + 1);
  memcpy(buffer->data + buffer->length, data, length);
  buffer->length += length;
  buffer->data[buffer->length] = '\0';
}

void bufferAppendChar(ByteBuffer* buffer, char c) {
  bufferEnsure(buffer, buffer->length + 2);
  buffer->data[buffer->length++] = c;
  buffer->data[buffer->length] = '\0';
}

void bufferFree(ByteBuffer* buffer) {
  free(buffer->data);
  buffer->data = NULL;
  buffer->length = 0;
  buffer->capacity = 0;
}

char* copyCString(const char* src, size_t length) {
  return platform_strndup(src, length);
}

void stringListInit(StringList* list) {
  list->items = NULL;
  list->count = 0;
  list->capacity = 0;
}

void stringListFree(StringList* list) {
  for (int i = 0; i < list->count; i++) {
    free(list->items[i]);
  }
  free(list->items);
  stringListInit(list);
}

void stringListAdd(StringList* list, const char* value) {
  if (list->capacity < list->count + 1) {
    int oldCapacity = list->capacity;
    list->capacity = oldCapacity == 0 ? 8 : oldCapacity * 2;
    list->items = (char**)realloc(list->items, sizeof(char*) * (size_t)list->capacity);
    if (!list->items) {
      fprintf(stderr, "Out of memory.\n");
      exit(1);
    }
  }
  list->items[list->count++] = copyCString(value, strlen(value));
}

void stringListAddWithLength(StringList* list, const char* value, size_t length) {
  if (list->capacity < list->count + 1) {
    int oldCapacity = list->capacity;
    list->capacity = oldCapacity == 0 ? 8 : oldCapacity * 2;
    list->items = (char**)realloc(list->items, sizeof(char*) * (size_t)list->capacity);
    if (!list->items) {
      fprintf(stderr, "Out of memory.\n");
      exit(1);
    }
  }
  list->items[list->count++] = copyCString(value, length);
}

static int stringListCompare(const void* a, const void* b) {
  const char* left = *(const char* const*)a;
  const char* right = *(const char* const*)b;
  return strcmp(left, right);
}

void stringListSort(StringList* list) {
  if (list->count > 1) {
    qsort(list->items, (size_t)list->count, sizeof(char*), stringListCompare);
  }
}

bool numberIsFinite(double value) {
#ifdef _MSC_VER
  return _finite(value) != 0;
#else
  return isfinite(value);
#endif
}

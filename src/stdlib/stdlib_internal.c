#include "stdlib_internal.h"
#include "platform.h"

#include <ctype.h>
#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

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
  buffer->failed = false;
}

void bufferEnsure(ByteBuffer* buffer, size_t needed) {
  if (buffer->failed || buffer->capacity >= needed) return;
  size_t oldCapacity = buffer->capacity;
  size_t newCapacity = oldCapacity == 0 ? 128 : oldCapacity;
  while (newCapacity < needed) {
    newCapacity *= 2;
  }
  char* next = (char*)realloc(buffer->data, newCapacity);
  if (!next) {
    buffer->failed = true;
    return;
  }
  buffer->data = next;
  buffer->capacity = newCapacity;
}

void bufferAppendN(ByteBuffer* buffer, const char* data, size_t length) {
  if (buffer->failed) return;
  bufferEnsure(buffer, buffer->length + length + 1);
  if (buffer->failed) return;
  memcpy(buffer->data + buffer->length, data, length);
  buffer->length += length;
  buffer->data[buffer->length] = '\0';
}

void bufferAppendChar(ByteBuffer* buffer, char c) {
  if (buffer->failed) return;
  bufferEnsure(buffer, buffer->length + 2);
  if (buffer->failed) return;
  buffer->data[buffer->length++] = c;
  buffer->data[buffer->length] = '\0';
}

void bufferFree(ByteBuffer* buffer) {
  free(buffer->data);
  buffer->data = NULL;
  buffer->length = 0;
  buffer->capacity = 0;
  buffer->failed = false;
}

char* copyCString(const char* src, size_t length) {
  return platform_strndup(src, length);
}

void stringListInit(StringList* list) {
  list->items = NULL;
  list->count = 0;
  list->capacity = 0;
  list->failed = false;
}

void stringListFree(StringList* list) {
  for (int i = 0; i < list->count; i++) {
    free(list->items[i]);
  }
  free(list->items);
  stringListInit(list);
}

void stringListAdd(StringList* list, const char* value) {
  if (list->failed) return;
  if (list->capacity < list->count + 1) {
    int oldCapacity = list->capacity;
    int newCapacity = oldCapacity == 0 ? 8 : oldCapacity * 2;
    char** resized = (char**)realloc(list->items, sizeof(char*) * (size_t)newCapacity);
    if (!resized) {
      list->failed = true;
      return;
    }
    list->items = resized;
    list->capacity = newCapacity;
  }
  char* item = copyCString(value, strlen(value));
  if (!item) {
    list->failed = true;
    return;
  }
  list->items[list->count++] = item;
}

void stringListAddWithLength(StringList* list, const char* value, size_t length) {
  if (list->failed) return;
  if (list->capacity < list->count + 1) {
    int oldCapacity = list->capacity;
    int newCapacity = oldCapacity == 0 ? 8 : oldCapacity * 2;
    char** resized = (char**)realloc(list->items, sizeof(char*) * (size_t)newCapacity);
    if (!resized) {
      list->failed = true;
      return;
    }
    list->items = resized;
    list->capacity = newCapacity;
  }
  char* item = copyCString(value, length);
  if (!item) {
    list->failed = true;
    return;
  }
  list->items[list->count++] = item;
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

static bool equalsIgnoreCase(const char* left, const char* right) {
  if (!left || !right) return false;
  while (*left != '\0' && *right != '\0') {
    unsigned char a = (unsigned char)*left;
    unsigned char b = (unsigned char)*right;
    if (tolower(a) != tolower(b)) return false;
    left++;
    right++;
  }
  return *left == '\0' && *right == '\0';
}

static bool envTruthy(const char* name) {
  const char* value = NULL;
#ifdef _WIN32
  char buffer[64];
  DWORD length = GetEnvironmentVariableA(name, buffer, (DWORD)sizeof(buffer));
  if (length == 0 || length >= sizeof(buffer)) return false;
  value = buffer;
#else
  value = getenv(name);
#endif
  if (!value || value[0] == '\0') return false;
  if (strcmp(value, "1") == 0) return true;
  if (equalsIgnoreCase(value, "true")) return true;
  if (equalsIgnoreCase(value, "yes")) return true;
  if (equalsIgnoreCase(value, "on")) return true;
  return false;
}

bool stdlibUnsafeEnabled(VM* vm, unsigned int featureFlag, const char* featureEnv) {
  if (vm && vm->unsafePolicyConfigured) {
    return (vm->unsafeFeatureMask & featureFlag) != 0;
  }
  if (envTruthy("ERKAO_ALLOW_UNSAFE")) return true;
  if (!featureEnv || featureEnv[0] == '\0') return false;
  return envTruthy(featureEnv);
}

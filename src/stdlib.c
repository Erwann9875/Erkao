#include "erkao_stdlib.h"
#include "gc.h"
#include "interpreter_internal.h"
#include "plugin.h"

#if ERKAO_HAS_GRAPHICS
#include "graphics.h"
#endif

#include <float.h>
#include <ctype.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <wchar.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winhttp.h>
#else
#include <arpa/inet.h>
#include <curl/curl.h>
#include <dirent.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

static Value runtimeErrorValue(VM* vm, const char* message) {
  Token token;
  memset(&token, 0, sizeof(Token));
  runtimeError(vm, token, message);
  return NULL_VAL;
}

static ObjInstance* makeModule(VM* vm, const char* name) {
  ObjString* className = copyString(vm, name);
  ObjMap* methods = newMap(vm);
  ObjClass* klass = newClass(vm, className, methods);
  return newInstance(vm, klass);
}

static void moduleAdd(VM* vm, ObjInstance* module, const char* name, NativeFn fn, int arity) {
  ObjString* fieldName = copyString(vm, name);
  ObjNative* native = newNative(vm, fn, arity, fieldName);
  mapSet(module->fields, fieldName, OBJ_VAL(native));
}

static void moduleAddValue(VM* vm, ObjInstance* module, const char* name, Value value) {
  ObjString* fieldName = copyString(vm, name);
  mapSet(module->fields, fieldName, value);
}

static const char* findLastSeparator(const char* path) {
  const char* lastSlash = strrchr(path, '/');
  const char* lastBackslash = strrchr(path, '\\');
  if (!lastSlash) return lastBackslash;
  if (!lastBackslash) return lastSlash;
  return lastSlash > lastBackslash ? lastSlash : lastBackslash;
}

static bool isAbsolutePathString(const char* path) {
  if (!path || path[0] == '\0') return false;
  if (path[0] == '/' || path[0] == '\\') return true;
  if (((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z')) &&
      path[1] == ':' && (path[2] == '\\' || path[2] == '/')) {
    return true;
  }
  return false;
}

static char pickSeparator(const char* left, const char* right) {
  if ((left && strchr(left, '\\')) || (right && strchr(right, '\\'))) return '\\';
  return '/';
}

static char* copyCString(const char* src, size_t length);

static bool pathExists(const char* path) {
#ifdef _WIN32
  DWORD attrs = GetFileAttributesA(path);
  return attrs != INVALID_FILE_ATTRIBUTES;
#else
  struct stat st;
  return stat(path, &st) == 0;
#endif
}

static bool pathIsDir(const char* path) {
#ifdef _WIN32
  DWORD attrs = GetFileAttributesA(path);
  if (attrs == INVALID_FILE_ATTRIBUTES) return false;
  return (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
#else
  struct stat st;
  if (stat(path, &st) != 0) return false;
  return S_ISDIR(st.st_mode);
#endif
}

static bool pathIsFile(const char* path) {
#ifdef _WIN32
  DWORD attrs = GetFileAttributesA(path);
  if (attrs == INVALID_FILE_ATTRIBUTES) return false;
  return (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
#else
  struct stat st;
  if (stat(path, &st) != 0) return false;
  return S_ISREG(st.st_mode);
#endif
}

static char* joinPathWithSep(const char* left, const char* right, char sep) {
  if (!left || left[0] == '\0' || strcmp(left, ".") == 0) {
    return copyCString(right, strlen(right));
  }
  if (isAbsolutePathString(right)) {
    return copyCString(right, strlen(right));
  }
  size_t leftLen = strlen(left);
  size_t rightLen = strlen(right);
  bool needsSep = leftLen > 0 &&
                  left[leftLen - 1] != '/' &&
                  left[leftLen - 1] != '\\';
  size_t total = leftLen + (needsSep ? 1 : 0) + rightLen;
  char* buffer = (char*)malloc(total + 1);
  if (!buffer) {
    fprintf(stderr, "Out of memory.\n");
    exit(1);
  }
  memcpy(buffer, left, leftLen);
  size_t offset = leftLen;
  if (needsSep) buffer[offset++] = sep;
  memcpy(buffer + offset, right, rightLen);
  buffer[total] = '\0';
  return buffer;
}

typedef struct {
  char* data;
  size_t length;
  size_t capacity;
} ByteBuffer;

static void bufferInit(ByteBuffer* buffer) {
  buffer->data = NULL;
  buffer->length = 0;
  buffer->capacity = 0;
}

static void bufferEnsure(ByteBuffer* buffer, size_t needed) {
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

static void bufferAppendN(ByteBuffer* buffer, const char* data, size_t length) {
  bufferEnsure(buffer, buffer->length + length + 1);
  memcpy(buffer->data + buffer->length, data, length);
  buffer->length += length;
  buffer->data[buffer->length] = '\0';
}

static void bufferAppendChar(ByteBuffer* buffer, char c) {
  bufferEnsure(buffer, buffer->length + 2);
  buffer->data[buffer->length++] = c;
  buffer->data[buffer->length] = '\0';
}

static void bufferFree(ByteBuffer* buffer) {
  free(buffer->data);
  buffer->data = NULL;
  buffer->length = 0;
  buffer->capacity = 0;
}

static char* copyCString(const char* src, size_t length) {
  char* out = (char*)malloc(length + 1);
  if (!out) {
    fprintf(stderr, "Out of memory.\n");
    exit(1);
  }
  memcpy(out, src, length);
  out[length] = '\0';
  return out;
}

typedef struct {
  char** items;
  int count;
  int capacity;
} StringList;

static void stringListInit(StringList* list) {
  list->items = NULL;
  list->count = 0;
  list->capacity = 0;
}

static void stringListFree(StringList* list) {
  for (int i = 0; i < list->count; i++) {
    free(list->items[i]);
  }
  free(list->items);
  stringListInit(list);
}

static void stringListAdd(StringList* list, const char* value) {
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

static void stringListAddWithLength(StringList* list, const char* value, size_t length) {
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

static int objStringCompare(const void* a, const void* b) {
  const ObjString* left = *(const ObjString* const*)a;
  const ObjString* right = *(const ObjString* const*)b;
  return strcmp(left->chars, right->chars);
}

static void stringListSort(StringList* list) {
  if (list->count > 1) {
    qsort(list->items, (size_t)list->count, sizeof(char*), stringListCompare);
  }
}

static bool numberIsFinite(double value) {
#ifdef _MSC_VER
  return _finite(value) != 0;
#else
  return isfinite(value);
#endif
}

static uint64_t gRandomState = 0;
static bool gRandomSeeded = false;
static bool gRandomHasSpare = false;
static double gRandomSpare = 0.0;

static void randomSeedIfNeeded(void) {
  if (gRandomSeeded) return;
  uint64_t seed = (uint64_t)time(NULL);
  seed ^= (uint64_t)clock() << 32;
  if (seed == 0) {
    seed = 0x9e3779b97f4a7c15ULL;
  }
  gRandomState = seed;
  gRandomSeeded = true;
}

static uint64_t randomNext(void) {
  randomSeedIfNeeded();
  uint64_t x = gRandomState;
  x ^= x >> 12;
  x ^= x << 25;
  x ^= x >> 27;
  gRandomState = x;
  return x * 2685821657736338717ULL;
}

static double randomNextDouble(void) {
  uint64_t value = randomNext();
  return (double)(value >> 11) * (1.0 / 9007199254740992.0);
}

static double randomNextNormal(void) {
  if (gRandomHasSpare) {
    gRandomHasSpare = false;
    return gRandomSpare;
  }

  double u = 0.0;
  double v = 0.0;
  double s = 0.0;
  do {
    u = randomNextDouble() * 2.0 - 1.0;
    v = randomNextDouble() * 2.0 - 1.0;
    s = u * u + v * v;
  } while (s <= 0.0 || s >= 1.0);

  double factor = sqrt(-2.0 * log(s) / s);
  gRandomSpare = v * factor;
  gRandomHasSpare = true;
  return u * factor;
}

static bool globSegmentHasWildcard(const char* segment) {
  if (!segment) return false;
  for (const char* c = segment; *c; c++) {
    if (*c == '*' || *c == '?') return true;
  }
  return false;
}

static bool globMatchSegment(const char* pattern, const char* text) {
  const char* p = pattern;
  const char* t = text;
  const char* star = NULL;
  const char* starText = NULL;

  while (*t) {
    if (*p == '*') {
      star = p++;
      starText = t;
      continue;
    }
    if (*p == '?' || *p == *t) {
      p++;
      t++;
      continue;
    }
    if (star) {
      p = star + 1;
      starText++;
      t = starText;
      continue;
    }
    return false;
  }

  while (*p == '*') p++;
  return *p == '\0';
}

static char* globRootFromPattern(const char* pattern, char sep, int* outIndex) {
  if (isAbsolutePathString(pattern)) {
    if (((pattern[0] >= 'A' && pattern[0] <= 'Z') ||
         (pattern[0] >= 'a' && pattern[0] <= 'z')) &&
        pattern[1] == ':' &&
        (pattern[2] == '\\' || pattern[2] == '/')) {
      char* root = (char*)malloc(4);
      if (!root) {
        fprintf(stderr, "Out of memory.\n");
        exit(1);
      }
      root[0] = pattern[0];
      root[1] = ':';
      root[2] = sep;
      root[3] = '\0';
      *outIndex = 3;
      return root;
    }
    if (pattern[0] == '\\' || pattern[0] == '/') {
      char* root = (char*)malloc(2);
      if (!root) {
        fprintf(stderr, "Out of memory.\n");
        exit(1);
      }
      root[0] = sep;
      root[1] = '\0';
      *outIndex = 1;
      return root;
    }
  }
  *outIndex = 0;
  return copyCString(".", 1);
}

static void globSplitSegments(const char* pattern, int start, StringList* segments) {
  stringListInit(segments);
  const char* cursor = pattern + start;
  while (*cursor) {
    while (*cursor == '/' || *cursor == '\\') cursor++;
    if (*cursor == '\0') break;
    const char* begin = cursor;
    while (*cursor && *cursor != '/' && *cursor != '\\') cursor++;
    size_t length = (size_t)(cursor - begin);
    if (length > 0) {
      if (segments->capacity < segments->count + 1) {
        int oldCap = segments->capacity;
        segments->capacity = oldCap == 0 ? 8 : oldCap * 2;
        segments->items = (char**)realloc(segments->items,
                                          sizeof(char*) * (size_t)segments->capacity);
        if (!segments->items) {
          fprintf(stderr, "Out of memory.\n");
          exit(1);
        }
      }
      segments->items[segments->count++] = copyCString(begin, length);
    }
  }
}

static bool globListDir(const char* path, StringList* out, const char** error) {
  stringListInit(out);
#ifdef _WIN32
  size_t pathLength = strlen(path);
  bool needsSep = pathLength > 0 &&
                  path[pathLength - 1] != '\\' &&
                  path[pathLength - 1] != '/';
  size_t patternLength = pathLength + (needsSep ? 2 : 1) + 1;
  char* pattern = (char*)malloc(patternLength);
  if (!pattern) {
    *error = "fs.glob out of memory.";
    return false;
  }
  snprintf(pattern, patternLength, "%s%s*", path, needsSep ? "\\" : "");

  WIN32_FIND_DATAA data;
  HANDLE handle = FindFirstFileA(pattern, &data);
  free(pattern);
  if (handle == INVALID_HANDLE_VALUE) {
    *error = "fs.glob failed to open directory.";
    return false;
  }

  do {
    if (strcmp(data.cFileName, ".") == 0 || strcmp(data.cFileName, "..") == 0) {
      continue;
    }
    stringListAdd(out, data.cFileName);
  } while (FindNextFileA(handle, &data));
  FindClose(handle);
#else
  DIR* dir = opendir(path);
  if (!dir) {
    *error = "fs.glob failed to open directory.";
    return false;
  }
  struct dirent* entry;
  while ((entry = readdir(dir)) != NULL) {
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
      continue;
    }
    stringListAdd(out, entry->d_name);
  }
  closedir(dir);
#endif
  stringListSort(out);
  return true;
}

static void globWalk(const char* base, char sep, StringList* segments,
                     int index, StringList* matches, const char** error) {
  if (*error) return;
  if (index >= segments->count) {
    if (pathExists(base)) {
      stringListAdd(matches, base);
    }
    return;
  }

  const char* segment = segments->items[index];
  if (strcmp(segment, "**") == 0) {
    globWalk(base, sep, segments, index + 1, matches, error);
    if (!pathIsDir(base)) return;

    StringList entries;
    if (!globListDir(base, &entries, error)) {
      return;
    }
    for (int i = 0; i < entries.count; i++) {
      char* next = joinPathWithSep(base, entries.items[i], sep);
      if (pathIsDir(next)) {
        globWalk(next, sep, segments, index, matches, error);
      }
      free(next);
      if (*error) break;
    }
    stringListFree(&entries);
    return;
  }

  if (globSegmentHasWildcard(segment)) {
    if (!pathIsDir(base)) return;
    StringList entries;
    if (!globListDir(base, &entries, error)) {
      return;
    }
    for (int i = 0; i < entries.count; i++) {
      if (!globMatchSegment(segment, entries.items[i])) continue;
      char* next = joinPathWithSep(base, entries.items[i], sep);
      if (index == segments->count - 1) {
        if (pathExists(next)) {
          stringListAdd(matches, next);
        }
      } else if (pathIsDir(next)) {
        globWalk(next, sep, segments, index + 1, matches, error);
      }
      free(next);
      if (*error) break;
    }
    stringListFree(&entries);
    return;
  }

  char* next = joinPathWithSep(base, segment, sep);
  if (index == segments->count - 1) {
    if (pathExists(next)) {
      stringListAdd(matches, next);
    }
  } else if (pathIsDir(next)) {
    globWalk(next, sep, segments, index + 1, matches, error);
  }
  free(next);
}

typedef struct {
  const char* start;
  const char* current;
  const char* error;
} JsonParser;

static void jsonSetError(JsonParser* parser, const char* message) {
  if (!parser->error) {
    parser->error = message;
  }
}

static Value jsonFail(JsonParser* parser, bool* ok, const char* message) {
  jsonSetError(parser, message);
  *ok = false;
  return NULL_VAL;
}

static void jsonSkipWhitespace(JsonParser* parser) {
  while (*parser->current &&
         isspace((unsigned char)*parser->current)) {
    parser->current++;
  }
}

static bool jsonMatch(JsonParser* parser, char expected) {
  if (*parser->current != expected) return false;
  parser->current++;
  return true;
}

static bool jsonConsume(JsonParser* parser, const char* text) {
  size_t length = strlen(text);
  if (strncmp(parser->current, text, length) != 0) return false;
  parser->current += length;
  return true;
}

static bool jsonParseHex(JsonParser* parser, uint32_t* out) {
  uint32_t value = 0;
  for (int i = 0; i < 4; i++) {
    char c = *parser->current++;
    if (c == '\0') return false;
    value <<= 4;
    if (c >= '0' && c <= '9') {
      value |= (uint32_t)(c - '0');
    } else if (c >= 'a' && c <= 'f') {
      value |= (uint32_t)(c - 'a' + 10);
    } else if (c >= 'A' && c <= 'F') {
      value |= (uint32_t)(c - 'A' + 10);
    } else {
      return false;
    }
  }
  *out = value;
  return true;
}

static bool jsonAppendUtf8(ByteBuffer* buffer, uint32_t codepoint, JsonParser* parser) {
  if (codepoint <= 0x7f) {
    bufferAppendChar(buffer, (char)codepoint);
    return true;
  }
  if (codepoint <= 0x7ff) {
    bufferAppendChar(buffer, (char)(0xc0 | ((codepoint >> 6) & 0x1f)));
    bufferAppendChar(buffer, (char)(0x80 | (codepoint & 0x3f)));
    return true;
  }
  if (codepoint <= 0xffff) {
    bufferAppendChar(buffer, (char)(0xe0 | ((codepoint >> 12) & 0x0f)));
    bufferAppendChar(buffer, (char)(0x80 | ((codepoint >> 6) & 0x3f)));
    bufferAppendChar(buffer, (char)(0x80 | (codepoint & 0x3f)));
    return true;
  }
  if (codepoint <= 0x10ffff) {
    bufferAppendChar(buffer, (char)(0xf0 | ((codepoint >> 18) & 0x07)));
    bufferAppendChar(buffer, (char)(0x80 | ((codepoint >> 12) & 0x3f)));
    bufferAppendChar(buffer, (char)(0x80 | ((codepoint >> 6) & 0x3f)));
    bufferAppendChar(buffer, (char)(0x80 | (codepoint & 0x3f)));
    return true;
  }
  jsonSetError(parser, "json.parse invalid unicode escape.");
  return false;
}

static Value jsonParseValue(VM* vm, JsonParser* parser, bool* ok);

static Value jsonParseString(VM* vm, JsonParser* parser, bool* ok) {
  ByteBuffer buffer;
  bufferInit(&buffer);

  parser->current++;
  while (*parser->current && *parser->current != '"') {
    char c = *parser->current++;
    if ((unsigned char)c < 0x20) {
      bufferFree(&buffer);
      return jsonFail(parser, ok, "json.parse invalid control character in string.");
    }
    if (c != '\\') {
      bufferAppendChar(&buffer, c);
      continue;
    }

    char escape = *parser->current++;
    if (escape == '\0') {
      bufferFree(&buffer);
      return jsonFail(parser, ok, "json.parse unterminated escape sequence.");
    }
    switch (escape) {
      case '"': bufferAppendChar(&buffer, '"'); break;
      case '\\': bufferAppendChar(&buffer, '\\'); break;
      case '/': bufferAppendChar(&buffer, '/'); break;
      case 'b': bufferAppendChar(&buffer, '\b'); break;
      case 'f': bufferAppendChar(&buffer, '\f'); break;
      case 'n': bufferAppendChar(&buffer, '\n'); break;
      case 'r': bufferAppendChar(&buffer, '\r'); break;
      case 't': bufferAppendChar(&buffer, '\t'); break;
      case 'u': {
        uint32_t codepoint = 0;
        if (!jsonParseHex(parser, &codepoint)) {
          bufferFree(&buffer);
          return jsonFail(parser, ok, "json.parse invalid unicode escape.");
        }

        if (codepoint >= 0xd800 && codepoint <= 0xdbff) {
          if (!jsonMatch(parser, '\\') || !jsonMatch(parser, 'u')) {
            bufferFree(&buffer);
            return jsonFail(parser, ok, "json.parse invalid unicode escape.");
          }
          uint32_t low = 0;
          if (!jsonParseHex(parser, &low) || low < 0xdc00 || low > 0xdfff) {
            bufferFree(&buffer);
            return jsonFail(parser, ok, "json.parse invalid unicode escape.");
          }
          codepoint = 0x10000 + (((codepoint - 0xd800) << 10) | (low - 0xdc00));
        } else if (codepoint >= 0xdc00 && codepoint <= 0xdfff) {
          bufferFree(&buffer);
          return jsonFail(parser, ok, "json.parse invalid unicode escape.");
        }

        if (!jsonAppendUtf8(&buffer, codepoint, parser)) {
          bufferFree(&buffer);
          *ok = false;
          return NULL_VAL;
        }
        break;
      }
      default:
        bufferFree(&buffer);
        return jsonFail(parser, ok, "json.parse invalid escape sequence.");
    }
  }

  if (!jsonMatch(parser, '"')) {
    bufferFree(&buffer);
    return jsonFail(parser, ok, "json.parse unterminated string.");
  }

  const char* text = buffer.data ? buffer.data : "";
  ObjString* result = copyStringWithLength(vm, text, (int)buffer.length);
  bufferFree(&buffer);
  return OBJ_VAL(result);
}

static Value jsonParseNumber(VM* vm, JsonParser* parser, bool* ok) {
  (void)vm;
  const char* start = parser->current;
  if (*parser->current == '-') parser->current++;

  if (*parser->current == '0') {
    parser->current++;
    if (isdigit((unsigned char)*parser->current)) {
      return jsonFail(parser, ok, "json.parse invalid number.");
    }
  } else if (isdigit((unsigned char)*parser->current)) {
    while (isdigit((unsigned char)*parser->current)) {
      parser->current++;
    }
  } else {
    return jsonFail(parser, ok, "json.parse invalid number.");
  }

  if (*parser->current == '.') {
    parser->current++;
    if (!isdigit((unsigned char)*parser->current)) {
      return jsonFail(parser, ok, "json.parse invalid number.");
    }
    while (isdigit((unsigned char)*parser->current)) {
      parser->current++;
    }
  }

  if (*parser->current == 'e' || *parser->current == 'E') {
    parser->current++;
    if (*parser->current == '+' || *parser->current == '-') {
      parser->current++;
    }
    if (!isdigit((unsigned char)*parser->current)) {
      return jsonFail(parser, ok, "json.parse invalid number.");
    }
    while (isdigit((unsigned char)*parser->current)) {
      parser->current++;
    }
  }

  char* end = NULL;
  double value = strtod(start, &end);
  if (end != parser->current) {
    return jsonFail(parser, ok, "json.parse invalid number.");
  }
  return NUMBER_VAL(value);
}

static Value jsonParseArray(VM* vm, JsonParser* parser, bool* ok) {
  ObjArray* array = newArray(vm);
  parser->current++;
  jsonSkipWhitespace(parser);
  if (jsonMatch(parser, ']')) {
    return OBJ_VAL(array);
  }

  for (;;) {
    Value value = jsonParseValue(vm, parser, ok);
    if (!*ok) return NULL_VAL;
    arrayWrite(array, value);
    jsonSkipWhitespace(parser);
    if (jsonMatch(parser, ']')) break;
    if (!jsonMatch(parser, ',')) {
      return jsonFail(parser, ok, "json.parse expected ',' or ']'.");
    }
    jsonSkipWhitespace(parser);
  }

  return OBJ_VAL(array);
}

static Value jsonParseObject(VM* vm, JsonParser* parser, bool* ok) {
  ObjMap* map = newMap(vm);
  parser->current++;
  jsonSkipWhitespace(parser);
  if (jsonMatch(parser, '}')) {
    return OBJ_VAL(map);
  }

  for (;;) {
    if (*parser->current != '"') {
      return jsonFail(parser, ok, "json.parse expected string key.");
    }
    Value keyValue = jsonParseString(vm, parser, ok);
    if (!*ok) return NULL_VAL;
    ObjString* key = (ObjString*)AS_OBJ(keyValue);

    jsonSkipWhitespace(parser);
    if (!jsonMatch(parser, ':')) {
      return jsonFail(parser, ok, "json.parse expected ':' after key.");
    }

    jsonSkipWhitespace(parser);
    Value value = jsonParseValue(vm, parser, ok);
    if (!*ok) return NULL_VAL;
    mapSet(map, key, value);

    jsonSkipWhitespace(parser);
    if (jsonMatch(parser, '}')) break;
    if (!jsonMatch(parser, ',')) {
      return jsonFail(parser, ok, "json.parse expected ',' or '}'.");
    }
    jsonSkipWhitespace(parser);
  }

  return OBJ_VAL(map);
}

static Value jsonParseValue(VM* vm, JsonParser* parser, bool* ok) {
  jsonSkipWhitespace(parser);

  char c = *parser->current;
  if (c == '"') return jsonParseString(vm, parser, ok);
  if (c == '{') return jsonParseObject(vm, parser, ok);
  if (c == '[') return jsonParseArray(vm, parser, ok);
  if (c == '-' || isdigit((unsigned char)c)) return jsonParseNumber(vm, parser, ok);

  if (jsonConsume(parser, "true")) return BOOL_VAL(true);
  if (jsonConsume(parser, "false")) return BOOL_VAL(false);
  if (jsonConsume(parser, "null")) return NULL_VAL;

  return jsonFail(parser, ok, "json.parse expected a value.");
}

static bool jsonStringifyValue(VM* vm, ByteBuffer* buffer, Value value, int depth,
                               const char** error);

static bool jsonAppendEscapedString(ByteBuffer* buffer, ObjString* string, const char** error) {
  bufferAppendChar(buffer, '"');
  for (int i = 0; i < string->length; i++) {
    unsigned char c = (unsigned char)string->chars[i];
    switch (c) {
      case '"':
        bufferAppendN(buffer, "\\\"", 2);
        break;
      case '\\':
        bufferAppendN(buffer, "\\\\", 2);
        break;
      case '\b':
        bufferAppendN(buffer, "\\b", 2);
        break;
      case '\f':
        bufferAppendN(buffer, "\\f", 2);
        break;
      case '\n':
        bufferAppendN(buffer, "\\n", 2);
        break;
      case '\r':
        bufferAppendN(buffer, "\\r", 2);
        break;
      case '\t':
        bufferAppendN(buffer, "\\t", 2);
        break;
      default:
        if (c < 0x20) {
          char escaped[7];
          int written = snprintf(escaped, sizeof(escaped), "\\u%04x", c);
          if (written != 6) {
            *error = "json.stringify failed.";
            return false;
          }
          bufferAppendN(buffer, escaped, 6);
        } else {
          bufferAppendChar(buffer, (char)c);
        }
        break;
    }
  }
  bufferAppendChar(buffer, '"');
  return true;
}

static int compareJsonEntries(const void* a, const void* b) {
  const MapEntryValue* left = *(const MapEntryValue* const*)a;
  const MapEntryValue* right = *(const MapEntryValue* const*)b;
  return strcmp(left->key->chars, right->key->chars);
}

static bool jsonStringifyArray(VM* vm, ByteBuffer* buffer, ObjArray* array, int depth,
                               const char** error) {
  (void)vm;
  bufferAppendChar(buffer, '[');
  for (int i = 0; i < array->count; i++) {
    if (i > 0) bufferAppendChar(buffer, ',');
    if (!jsonStringifyValue(vm, buffer, array->items[i], depth + 1, error)) {
      return false;
    }
  }
  bufferAppendChar(buffer, ']');
  return true;
}

static bool jsonStringifyMap(VM* vm, ByteBuffer* buffer, ObjMap* map, int depth,
                             const char** error) {
  (void)vm;
  bufferAppendChar(buffer, '{');
  if (map->count > 0) {
    MapEntryValue** entries =
        (MapEntryValue**)malloc(sizeof(MapEntryValue*) * (size_t)map->count);
    if (!entries) {
      fprintf(stderr, "Out of memory.\n");
      exit(1);
    }

    int count = 0;
    for (int i = 0; i < map->capacity; i++) {
      if (map->entries[i].key) {
        entries[count++] = &map->entries[i];
      }
    }

    qsort(entries, (size_t)count, sizeof(MapEntryValue*), compareJsonEntries);

    for (int i = 0; i < count; i++) {
      if (i > 0) bufferAppendChar(buffer, ',');
      if (!jsonAppendEscapedString(buffer, entries[i]->key, error)) {
        free(entries);
        return false;
      }
      bufferAppendChar(buffer, ':');
      if (!jsonStringifyValue(vm, buffer, entries[i]->value, depth + 1, error)) {
        free(entries);
        return false;
      }
    }

    free(entries);
  }
  bufferAppendChar(buffer, '}');
  return true;
}

static bool jsonStringifyValue(VM* vm, ByteBuffer* buffer, Value value, int depth,
                               const char** error) {
  if (depth > 128) {
    *error = "json.stringify exceeded max depth.";
    return false;
  }

  switch (value.type) {
    case VAL_NULL:
      bufferAppendN(buffer, "null", 4);
      return true;
    case VAL_BOOL:
      if (AS_BOOL(value)) {
        bufferAppendN(buffer, "true", 4);
      } else {
        bufferAppendN(buffer, "false", 5);
      }
      return true;
    case VAL_NUMBER: {
      double number = AS_NUMBER(value);
      if (!numberIsFinite(number)) {
        *error = "json.stringify expects finite numbers.";
        return false;
      }
      char temp[64];
      int length = snprintf(temp, sizeof(temp), "%.17g", number);
      if (length <= 0 || length >= (int)sizeof(temp)) {
        *error = "json.stringify failed to format number.";
        return false;
      }
      bufferAppendN(buffer, temp, (size_t)length);
      return true;
    }
    case VAL_OBJ: {
      Obj* obj = AS_OBJ(value);
      if (obj->type == OBJ_STRING) {
        return jsonAppendEscapedString(buffer, (ObjString*)obj, error);
      }
      if (obj->type == OBJ_ARRAY) {
        return jsonStringifyArray(vm, buffer, (ObjArray*)obj, depth, error);
      }
      if (obj->type == OBJ_MAP) {
        return jsonStringifyMap(vm, buffer, (ObjMap*)obj, depth, error);
      }
      *error = "json.stringify cannot serialize this value.";
      return false;
    }
  }
  *error = "json.stringify failed.";
  return false;
}

static Value nativeJsonParse(VM* vm, int argc, Value* args) {
  (void)argc;
  if (!isObjType(args[0], OBJ_STRING)) {
    return runtimeErrorValue(vm, "json.parse expects a string.");
  }

  ObjString* input = (ObjString*)AS_OBJ(args[0]);
  JsonParser parser;
  parser.start = input->chars;
  parser.current = input->chars;
  parser.error = NULL;

  bool ok = true;
  Value result = jsonParseValue(vm, &parser, &ok);
  if (ok) {
    jsonSkipWhitespace(&parser);
    if (*parser.current != '\0') {
      ok = false;
      jsonSetError(&parser, "json.parse found trailing characters.");
    }
  }

  if (!ok) {
    return runtimeErrorValue(vm, parser.error ? parser.error : "json.parse failed.");
  }
  return result;
}

static Value nativeJsonStringify(VM* vm, int argc, Value* args) {
  (void)argc;
  ByteBuffer buffer;
  bufferInit(&buffer);
  const char* error = NULL;

  if (!jsonStringifyValue(vm, &buffer, args[0], 0, &error)) {
    bufferFree(&buffer);
    return runtimeErrorValue(vm, error ? error : "json.stringify failed.");
  }

  const char* text = buffer.data ? buffer.data : "";
  ObjString* result = copyStringWithLength(vm, text, (int)buffer.length);
  bufferFree(&buffer);
  return OBJ_VAL(result);
}

typedef struct {
  char* text;
  int indent;
} YamlLine;

typedef struct {
  YamlLine* lines;
  int count;
  int index;
  const char* error;
  char* buffer;
} YamlParser;

static void yamlStripComment(char* line) {
  bool inSingle = false;
  bool inDouble = false;
  bool escaped = false;
  for (int i = 0; line[i] != '\0'; i++) {
    char c = line[i];
    if (inDouble) {
      if (escaped) {
        escaped = false;
        continue;
      }
      if (c == '\\') {
        escaped = true;
        continue;
      }
      if (c == '"') {
        inDouble = false;
      }
      continue;
    }
    if (inSingle) {
      if (c == '\'') {
        inSingle = false;
      }
      continue;
    }
    if (c == '"') {
      inDouble = true;
      continue;
    }
    if (c == '\'') {
      inSingle = true;
      continue;
    }
    if (c == '#') {
      line[i] = '\0';
      return;
    }
    if (c == '/' && line[i + 1] == '/') {
      line[i] = '\0';
      return;
    }
  }
}

static char* yamlTrimLeft(char* text) {
  while (*text && isspace((unsigned char)*text)) text++;
  return text;
}

static void yamlTrimRight(char* text) {
  size_t length = strlen(text);
  while (length > 0 && isspace((unsigned char)text[length - 1])) {
    text[length - 1] = '\0';
    length--;
  }
}

static bool yamlCollectLines(YamlParser* parser, const char* source) {
  size_t length = strlen(source);
  parser->buffer = copyCString(source, length);
  parser->lines = NULL;
  parser->count = 0;
  parser->index = 0;
  parser->error = NULL;
  int capacity = 0;

  char* cursor = parser->buffer;
  while (*cursor) {
    char* lineStart = cursor;
    char* newline = strchr(cursor, '\n');
    if (newline) {
      *newline = '\0';
      cursor = newline + 1;
    } else {
      cursor += strlen(cursor);
    }
    size_t lineLen = strlen(lineStart);
    if (lineLen > 0 && lineStart[lineLen - 1] == '\r') {
      lineStart[lineLen - 1] = '\0';
    }

    yamlStripComment(lineStart);
    yamlTrimRight(lineStart);

    int indent = 0;
    char* content = lineStart;
    while (*content == ' ') {
      indent++;
      content++;
    }
    if (*content == '\t') {
      parser->error = "yaml.parse does not allow tabs for indentation.";
      return false;
    }
    content = yamlTrimLeft(content);
    if (*content == '\0') continue;

    if (parser->count >= capacity) {
      int oldCapacity = capacity;
      capacity = oldCapacity == 0 ? 16 : oldCapacity * 2;
      parser->lines = (YamlLine*)realloc(parser->lines, sizeof(YamlLine) * (size_t)capacity);
      if (!parser->lines) {
        fprintf(stderr, "Out of memory.\n");
        exit(1);
      }
    }
    parser->lines[parser->count].text = content;
    parser->lines[parser->count].indent = indent;
    parser->count++;
  }
  return true;
}

static ObjString* yamlParseString(VM* vm, const char* text, bool* ok, const char** error) {
  size_t length = strlen(text);
  if (length == 0) {
    return copyString(vm, "");
  }
  if (text[0] == '"') {
    ByteBuffer buffer;
    bufferInit(&buffer);
    bool escaped = false;
    for (size_t i = 1; i < length; i++) {
      char c = text[i];
      if (escaped) {
        switch (c) {
          case 'n': bufferAppendChar(&buffer, '\n'); break;
          case 'r': bufferAppendChar(&buffer, '\r'); break;
          case 't': bufferAppendChar(&buffer, '\t'); break;
          case '"': bufferAppendChar(&buffer, '"'); break;
          case '\\': bufferAppendChar(&buffer, '\\'); break;
          default: bufferAppendChar(&buffer, c); break;
        }
        escaped = false;
        continue;
      }
      if (c == '\\') {
        escaped = true;
        continue;
      }
      if (c == '"') {
        ObjString* result = copyStringWithLength(vm, buffer.data ? buffer.data : "",
                                                 (int)buffer.length);
        bufferFree(&buffer);
        return result;
      }
      bufferAppendChar(&buffer, c);
    }
    bufferFree(&buffer);
    *ok = false;
    *error = "yaml.parse unterminated string.";
    return NULL;
  }
  if (text[0] == '\'') {
    ByteBuffer buffer;
    bufferInit(&buffer);
    for (size_t i = 1; i < length; i++) {
      char c = text[i];
      if (c == '\'') {
        if (text[i + 1] == '\'') {
          bufferAppendChar(&buffer, '\'');
          i++;
          continue;
        }
        ObjString* result = copyStringWithLength(vm, buffer.data ? buffer.data : "",
                                                 (int)buffer.length);
        bufferFree(&buffer);
        return result;
      }
      bufferAppendChar(&buffer, c);
    }
    bufferFree(&buffer);
    *ok = false;
    *error = "yaml.parse unterminated string.";
    return NULL;
  }
  return copyString(vm, text);
}

static Value yamlParseScalar(VM* vm, const char* text, bool* ok, const char** error) {
  char* trimmed = yamlTrimLeft((char*)text);
  yamlTrimRight(trimmed);
  if (*trimmed == '\0') {
    return OBJ_VAL(copyString(vm, ""));
  }
  if (strcmp(trimmed, "null") == 0 || strcmp(trimmed, "~") == 0) {
    return NULL_VAL;
  }
  if (strcmp(trimmed, "true") == 0) {
    return BOOL_VAL(true);
  }
  if (strcmp(trimmed, "false") == 0) {
    return BOOL_VAL(false);
  }
  if (trimmed[0] == '"' || trimmed[0] == '\'') {
    ObjString* value = yamlParseString(vm, trimmed, ok, error);
    if (!*ok) return NULL_VAL;
    return OBJ_VAL(value);
  }

  char* end = NULL;
  double number = strtod(trimmed, &end);
  if (end && *end == '\0' && end != trimmed) {
    return NUMBER_VAL(number);
  }
  ObjString* str = copyString(vm, trimmed);
  return OBJ_VAL(str);
}

static char* yamlFindColon(char* text) {
  bool inSingle = false;
  bool inDouble = false;
  bool escaped = false;
  for (int i = 0; text[i] != '\0'; i++) {
    char c = text[i];
    if (inDouble) {
      if (escaped) {
        escaped = false;
        continue;
      }
      if (c == '\\') {
        escaped = true;
        continue;
      }
      if (c == '"') {
        inDouble = false;
      }
      continue;
    }
    if (inSingle) {
      if (c == '\'') {
        inSingle = false;
      }
      continue;
    }
    if (c == '"') {
      inDouble = true;
      continue;
    }
    if (c == '\'') {
      inSingle = true;
      continue;
    }
    if (c == ':') return &text[i];
  }
  return NULL;
}

static Value yamlParseBlock(VM* vm, YamlParser* parser, int indent, bool* ok);

static Value yamlParseList(VM* vm, YamlParser* parser, int indent, bool* ok) {
  ObjArray* array = newArray(vm);
  while (parser->index < parser->count) {
    YamlLine* line = &parser->lines[parser->index];
    if (line->indent != indent) break;
    if (line->text[0] != '-' || (line->text[1] != '\0' && line->text[1] != ' ')) {
      parser->error = "yaml.parse expected '-' list item.";
      *ok = false;
      return NULL_VAL;
    }
    char* itemText = line->text + 1;
    if (*itemText == ' ') itemText++;
    if (*itemText == '\0') {
      parser->index++;
      if (parser->index >= parser->count) {
        parser->error = "yaml.parse expected nested block.";
        *ok = false;
        return NULL_VAL;
      }
      YamlLine* next = &parser->lines[parser->index];
      if (next->indent <= indent) {
        parser->error = "yaml.parse expected indented block.";
        *ok = false;
        return NULL_VAL;
      }
      Value value = yamlParseBlock(vm, parser, next->indent, ok);
      if (!*ok) return NULL_VAL;
      arrayWrite(array, value);
    } else {
      Value value = yamlParseScalar(vm, itemText, ok, &parser->error);
      if (!*ok) return NULL_VAL;
      arrayWrite(array, value);
      parser->index++;
    }
  }
  return OBJ_VAL(array);
}

static Value yamlParseMap(VM* vm, YamlParser* parser, int indent, bool* ok) {
  ObjMap* map = newMap(vm);
  while (parser->index < parser->count) {
    YamlLine* line = &parser->lines[parser->index];
    if (line->indent != indent) break;
    char* colon = yamlFindColon(line->text);
    if (!colon) {
      parser->error = "yaml.parse expected ':' in mapping.";
      *ok = false;
      return NULL_VAL;
    }
    *colon = '\0';
    char* keyText = yamlTrimLeft(line->text);
    yamlTrimRight(keyText);
    if (*keyText == '\0') {
      parser->error = "yaml.parse empty key.";
      *ok = false;
      return NULL_VAL;
    }
    ObjString* key = NULL;
    if (keyText[0] == '"' || keyText[0] == '\'') {
      key = yamlParseString(vm, keyText, ok, &parser->error);
      if (!*ok) return NULL_VAL;
    } else {
      key = copyString(vm, keyText);
    }

    char* valueText = colon + 1;
    valueText = yamlTrimLeft(valueText);
    yamlTrimRight(valueText);

    parser->index++;
    Value value;
    if (*valueText == '\0') {
      if (parser->index < parser->count &&
          parser->lines[parser->index].indent > indent) {
        int childIndent = parser->lines[parser->index].indent;
        value = yamlParseBlock(vm, parser, childIndent, ok);
        if (!*ok) return NULL_VAL;
      } else {
        value = NULL_VAL;
      }
    } else {
      value = yamlParseScalar(vm, valueText, ok, &parser->error);
      if (!*ok) return NULL_VAL;
    }
    mapSet(map, key, value);
  }
  return OBJ_VAL(map);
}

static Value yamlParseBlock(VM* vm, YamlParser* parser, int indent, bool* ok) {
  if (parser->index >= parser->count) {
    parser->error = "yaml.parse unexpected end.";
    *ok = false;
    return NULL_VAL;
  }
  YamlLine* line = &parser->lines[parser->index];
  if (line->indent < indent) {
    parser->error = "yaml.parse invalid indentation.";
    *ok = false;
    return NULL_VAL;
  }
  bool isList = line->text[0] == '-' &&
                (line->text[1] == '\0' || line->text[1] == ' ');
  if (isList) {
    return yamlParseList(vm, parser, indent, ok);
  }
  return yamlParseMap(vm, parser, indent, ok);
}

static Value nativeYamlParse(VM* vm, int argc, Value* args) {
  (void)argc;
  if (!isObjType(args[0], OBJ_STRING)) {
    return runtimeErrorValue(vm, "yaml.parse expects a string.");
  }
  ObjString* input = (ObjString*)AS_OBJ(args[0]);
  YamlParser parser;
  if (!yamlCollectLines(&parser, input->chars)) {
    free(parser.lines);
    free(parser.buffer);
    return runtimeErrorValue(vm, parser.error ? parser.error : "yaml.parse failed.");
  }
  if (parser.count == 0) {
    free(parser.lines);
    free(parser.buffer);
    return NULL_VAL;
  }
  bool ok = true;
  Value result = yamlParseBlock(vm, &parser, parser.lines[0].indent, &ok);
  const char* error = parser.error;
  free(parser.lines);
  free(parser.buffer);
  if (!ok) {
    return runtimeErrorValue(vm, error ? error : "yaml.parse failed.");
  }
  return result;
}

static void yamlAppendIndent(ByteBuffer* buffer, int indent) {
  for (int i = 0; i < indent; i++) {
    bufferAppendChar(buffer, ' ');
  }
}

static bool yamlStringNeedsQuotes(const char* text) {
  if (!text || *text == '\0') return true;
  if (strcmp(text, "null") == 0 || strcmp(text, "true") == 0 ||
      strcmp(text, "false") == 0 || strcmp(text, "~") == 0) {
    return true;
  }
  for (const char* c = text; *c; c++) {
    if (isspace((unsigned char)*c)) return true;
    if (*c == ':' || *c == '#' || *c == '-' || *c == '"' || *c == '\'' ||
        *c == '{' || *c == '}' || *c == '[' || *c == ']' || *c == ',') {
      return true;
    }
  }
  return false;
}

static bool yamlAppendEscaped(ByteBuffer* buffer, ObjString* string) {
  bufferAppendChar(buffer, '"');
  for (int i = 0; i < string->length; i++) {
    char c = string->chars[i];
    switch (c) {
      case '\\': bufferAppendN(buffer, "\\\\", 2); break;
      case '"': bufferAppendN(buffer, "\\\"", 2); break;
      case '\n': bufferAppendN(buffer, "\\n", 2); break;
      case '\r': bufferAppendN(buffer, "\\r", 2); break;
      case '\t': bufferAppendN(buffer, "\\t", 2); break;
      default: bufferAppendChar(buffer, c); break;
    }
  }
  bufferAppendChar(buffer, '"');
  return true;
}

static bool yamlStringifyValue(VM* vm, ByteBuffer* buffer, Value value,
                               int indent, int depth, const char** error);

static bool yamlStringifyArray(VM* vm, ByteBuffer* buffer, ObjArray* array,
                               int indent, int depth, const char** error) {
  if (depth > 64) {
    *error = "yaml.stringify exceeded max depth.";
    return false;
  }
  if (array->count == 0) {
    bufferAppendN(buffer, "[]", 2);
    return true;
  }
  for (int i = 0; i < array->count; i++) {
    yamlAppendIndent(buffer, indent);
    bufferAppendN(buffer, "- ", 2);
    Value item = array->items[i];
    if (isObjType(item, OBJ_ARRAY) || isObjType(item, OBJ_MAP)) {
      buffer->length -= 1;
      buffer->data[buffer->length] = '\0';
      bufferAppendChar(buffer, '\n');
      if (!yamlStringifyValue(vm, buffer, item, indent + 2, depth + 1, error)) {
        return false;
      }
    } else {
      if (!yamlStringifyValue(vm, buffer, item, 0, depth + 1, error)) {
        return false;
      }
    }
    if (i + 1 < array->count) {
      bufferAppendChar(buffer, '\n');
    }
  }
  return true;
}

static bool yamlStringifyMap(VM* vm, ByteBuffer* buffer, ObjMap* map,
                             int indent, int depth, const char** error) {
  if (depth > 64) {
    *error = "yaml.stringify exceeded max depth.";
    return false;
  }
  int count = mapCount(map);
  if (count == 0) {
    bufferAppendN(buffer, "{}", 2);
    return true;
  }

  ObjString** keys = (ObjString**)malloc(sizeof(ObjString*) * (size_t)count);
  if (!keys) {
    *error = "yaml.stringify out of memory.";
    return false;
  }
  int keyCount = 0;
  for (int i = 0; i < map->capacity; i++) {
    MapEntryValue* entry = &map->entries[i];
    if (!entry->key) continue;
    keys[keyCount++] = entry->key;
  }
  qsort(keys, (size_t)keyCount, sizeof(ObjString*), objStringCompare);

  for (int i = 0; i < keyCount; i++) {
    ObjString* key = keys[i];
    Value value;
    if (!mapGet(map, key, &value)) {
      continue;
    }
    yamlAppendIndent(buffer, indent);
    if (yamlStringNeedsQuotes(key->chars)) {
      yamlAppendEscaped(buffer, key);
    } else {
      bufferAppendN(buffer, key->chars, (size_t)key->length);
    }
    if (isObjType(value, OBJ_ARRAY) || isObjType(value, OBJ_MAP)) {
      bufferAppendChar(buffer, ':');
      bufferAppendChar(buffer, '\n');
      if (!yamlStringifyValue(vm, buffer, value, indent + 2, depth + 1, error)) {
        free(keys);
        return false;
      }
    } else {
      bufferAppendN(buffer, ": ", 2);
      if (!yamlStringifyValue(vm, buffer, value, 0, depth + 1, error)) {
        free(keys);
        return false;
      }
    }
    if (i + 1 < keyCount) {
      bufferAppendChar(buffer, '\n');
    }
  }
  free(keys);
  return true;
}

static bool yamlStringifyValue(VM* vm, ByteBuffer* buffer, Value value,
                               int indent, int depth, const char** error) {
  (void)vm;
  if (depth > 64) {
    *error = "yaml.stringify exceeded max depth.";
    return false;
  }
  if (IS_NULL(value)) {
    bufferAppendN(buffer, "null", 4);
    return true;
  }
  if (IS_BOOL(value)) {
    if (AS_BOOL(value)) {
      bufferAppendN(buffer, "true", 4);
    } else {
      bufferAppendN(buffer, "false", 5);
    }
    return true;
  }
  if (IS_NUMBER(value)) {
    if (!numberIsFinite(AS_NUMBER(value))) {
      *error = "yaml.stringify expects finite numbers.";
      return false;
    }
    char num[64];
    int length = snprintf(num, sizeof(num), "%g", AS_NUMBER(value));
    if (length < 0) length = 0;
    if (length >= (int)sizeof(num)) length = (int)sizeof(num) - 1;
    bufferAppendN(buffer, num, (size_t)length);
    return true;
  }
  if (isObjType(value, OBJ_STRING)) {
    ObjString* str = (ObjString*)AS_OBJ(value);
    if (yamlStringNeedsQuotes(str->chars)) {
      return yamlAppendEscaped(buffer, str);
    }
    bufferAppendN(buffer, str->chars, (size_t)str->length);
    return true;
  }
  if (isObjType(value, OBJ_ARRAY)) {
    return yamlStringifyArray(vm, buffer, (ObjArray*)AS_OBJ(value),
                              indent, depth + 1, error);
  }
  if (isObjType(value, OBJ_MAP)) {
    return yamlStringifyMap(vm, buffer, (ObjMap*)AS_OBJ(value),
                            indent, depth + 1, error);
  }
  *error = "yaml.stringify cannot serialize this value.";
  return false;
}

static Value nativeYamlStringify(VM* vm, int argc, Value* args) {
  (void)argc;
  ByteBuffer buffer;
  bufferInit(&buffer);
  const char* error = NULL;
  if (!yamlStringifyValue(vm, &buffer, args[0], 0, 0, &error)) {
    bufferFree(&buffer);
    return runtimeErrorValue(vm, error ? error : "yaml.stringify failed.");
  }
  ObjString* result = copyStringWithLength(vm, buffer.data ? buffer.data : "",
                                           (int)buffer.length);
  bufferFree(&buffer);
  return OBJ_VAL(result);
}

static bool expectNumberArg(VM* vm, Value value, const char* message) {
  if (!IS_NUMBER(value)) {
    runtimeErrorValue(vm, message);
    return false;
  }
  return true;
}

static ObjString* expectStringArg(VM* vm, Value value, const char* message) {
  if (!isObjType(value, OBJ_STRING)) {
    runtimeErrorValue(vm, message);
    return NULL;
  }
  return (ObjString*)AS_OBJ(value);
}

static ObjMap* expectMapArg(VM* vm, Value value, const char* message) {
  if (!isObjType(value, OBJ_MAP)) {
    runtimeErrorValue(vm, message);
    return NULL;
  }
  return (ObjMap*)AS_OBJ(value);
}

static double roundNumber(double value) {
  if (value >= 0.0) {
    return floor(value + 0.5);
  }
  return ceil(value - 0.5);
}

static Value nativeMathAbs(VM* vm, int argc, Value* args) {
  (void)argc;
  if (!expectNumberArg(vm, args[0], "math.abs expects a number.")) return NULL_VAL;
  return NUMBER_VAL(fabs(AS_NUMBER(args[0])));
}

static Value nativeMathFloor(VM* vm, int argc, Value* args) {
  (void)argc;
  if (!expectNumberArg(vm, args[0], "math.floor expects a number.")) return NULL_VAL;
  return NUMBER_VAL(floor(AS_NUMBER(args[0])));
}

static Value nativeMathCeil(VM* vm, int argc, Value* args) {
  (void)argc;
  if (!expectNumberArg(vm, args[0], "math.ceil expects a number.")) return NULL_VAL;
  return NUMBER_VAL(ceil(AS_NUMBER(args[0])));
}

static Value nativeMathRound(VM* vm, int argc, Value* args) {
  (void)argc;
  if (!expectNumberArg(vm, args[0], "math.round expects a number.")) return NULL_VAL;
  return NUMBER_VAL(roundNumber(AS_NUMBER(args[0])));
}

static Value nativeMathSqrt(VM* vm, int argc, Value* args) {
  (void)argc;
  if (!expectNumberArg(vm, args[0], "math.sqrt expects a number.")) return NULL_VAL;
  double value = AS_NUMBER(args[0]);
  if (value < 0.0) {
    return runtimeErrorValue(vm, "math.sqrt expects a non-negative number.");
  }
  return NUMBER_VAL(sqrt(value));
}

static Value nativeMathPow(VM* vm, int argc, Value* args) {
  (void)argc;
  if (!expectNumberArg(vm, args[0], "math.pow expects numbers.")) return NULL_VAL;
  if (!expectNumberArg(vm, args[1], "math.pow expects numbers.")) return NULL_VAL;
  return NUMBER_VAL(pow(AS_NUMBER(args[0]), AS_NUMBER(args[1])));
}

static Value nativeMathMin(VM* vm, int argc, Value* args) {
  if (argc < 1) {
    return runtimeErrorValue(vm, "math.min expects at least one number.");
  }
  if (!expectNumberArg(vm, args[0], "math.min expects numbers.")) return NULL_VAL;
  double result = AS_NUMBER(args[0]);
  for (int i = 1; i < argc; i++) {
    if (!expectNumberArg(vm, args[i], "math.min expects numbers.")) return NULL_VAL;
    double value = AS_NUMBER(args[i]);
    if (value < result) result = value;
  }
  return NUMBER_VAL(result);
}

static Value nativeMathMax(VM* vm, int argc, Value* args) {
  if (argc < 1) {
    return runtimeErrorValue(vm, "math.max expects at least one number.");
  }
  if (!expectNumberArg(vm, args[0], "math.max expects numbers.")) return NULL_VAL;
  double result = AS_NUMBER(args[0]);
  for (int i = 1; i < argc; i++) {
    if (!expectNumberArg(vm, args[i], "math.max expects numbers.")) return NULL_VAL;
    double value = AS_NUMBER(args[i]);
    if (value > result) result = value;
  }
  return NUMBER_VAL(result);
}

static Value nativeMathClamp(VM* vm, int argc, Value* args) {
  (void)argc;
  if (!expectNumberArg(vm, args[0], "math.clamp expects numbers.")) return NULL_VAL;
  if (!expectNumberArg(vm, args[1], "math.clamp expects numbers.")) return NULL_VAL;
  if (!expectNumberArg(vm, args[2], "math.clamp expects numbers.")) return NULL_VAL;
  double value = AS_NUMBER(args[0]);
  double minValue = AS_NUMBER(args[1]);
  double maxValue = AS_NUMBER(args[2]);
  if (minValue > maxValue) {
    return runtimeErrorValue(vm, "math.clamp expects min <= max.");
  }
  if (value < minValue) value = minValue;
  if (value > maxValue) value = maxValue;
  return NUMBER_VAL(value);
}

static bool vecRead(VM* vm, Value value, int dims, double* out, const char* message) {
  if (!isObjType(value, OBJ_ARRAY)) {
    runtimeErrorValue(vm, message);
    return false;
  }
  ObjArray* array = (ObjArray*)AS_OBJ(value);
  if (array->count < dims) {
    runtimeErrorValue(vm, message);
    return false;
  }
  for (int i = 0; i < dims; i++) {
    if (!IS_NUMBER(array->items[i])) {
      runtimeErrorValue(vm, message);
      return false;
    }
    out[i] = AS_NUMBER(array->items[i]);
  }
  return true;
}

static Value vecMake(VM* vm, int dims, const double* values) {
  ObjArray* array = newArrayWithCapacity(vm, dims);
  for (int i = 0; i < dims; i++) {
    arrayWrite(array, NUMBER_VAL(values[i]));
  }
  return OBJ_VAL(array);
}

static Value vecAddN(VM* vm, int dims, Value* args, const char* message) {
  double a[4];
  double b[4];
  if (!vecRead(vm, args[0], dims, a, message)) return NULL_VAL;
  if (!vecRead(vm, args[1], dims, b, message)) return NULL_VAL;
  double out[4];
  for (int i = 0; i < dims; i++) {
    out[i] = a[i] + b[i];
  }
  return vecMake(vm, dims, out);
}

static Value vecSubN(VM* vm, int dims, Value* args, const char* message) {
  double a[4];
  double b[4];
  if (!vecRead(vm, args[0], dims, a, message)) return NULL_VAL;
  if (!vecRead(vm, args[1], dims, b, message)) return NULL_VAL;
  double out[4];
  for (int i = 0; i < dims; i++) {
    out[i] = a[i] - b[i];
  }
  return vecMake(vm, dims, out);
}

static Value vecScaleN(VM* vm, int dims, Value* args, const char* message) {
  double a[4];
  if (!vecRead(vm, args[0], dims, a, message)) return NULL_VAL;
  if (!IS_NUMBER(args[1])) {
    return runtimeErrorValue(vm, message);
  }
  double scale = AS_NUMBER(args[1]);
  double out[4];
  for (int i = 0; i < dims; i++) {
    out[i] = a[i] * scale;
  }
  return vecMake(vm, dims, out);
}

static Value vecDotN(VM* vm, int dims, Value* args, const char* message) {
  double a[4];
  double b[4];
  if (!vecRead(vm, args[0], dims, a, message)) return NULL_VAL;
  if (!vecRead(vm, args[1], dims, b, message)) return NULL_VAL;
  double sum = 0.0;
  for (int i = 0; i < dims; i++) {
    sum += a[i] * b[i];
  }
  return NUMBER_VAL(sum);
}

static Value vecLenN(VM* vm, int dims, Value* args, const char* message) {
  double a[4];
  if (!vecRead(vm, args[0], dims, a, message)) return NULL_VAL;
  double sum = 0.0;
  for (int i = 0; i < dims; i++) {
    sum += a[i] * a[i];
  }
  return NUMBER_VAL(sqrt(sum));
}

static Value vecNormN(VM* vm, int dims, Value* args, const char* message) {
  double a[4];
  if (!vecRead(vm, args[0], dims, a, message)) return NULL_VAL;
  double sum = 0.0;
  for (int i = 0; i < dims; i++) {
    sum += a[i] * a[i];
  }
  double len = sqrt(sum);
  double out[4];
  if (len <= 0.0) {
    for (int i = 0; i < dims; i++) out[i] = 0.0;
  } else {
    for (int i = 0; i < dims; i++) out[i] = a[i] / len;
  }
  return vecMake(vm, dims, out);
}

static Value vecLerpN(VM* vm, int dims, Value* args, const char* message) {
  double a[4];
  double b[4];
  if (!vecRead(vm, args[0], dims, a, message)) return NULL_VAL;
  if (!vecRead(vm, args[1], dims, b, message)) return NULL_VAL;
  if (!IS_NUMBER(args[2])) {
    return runtimeErrorValue(vm, message);
  }
  double t = AS_NUMBER(args[2]);
  double out[4];
  for (int i = 0; i < dims; i++) {
    out[i] = a[i] + (b[i] - a[i]) * t;
  }
  return vecMake(vm, dims, out);
}

static Value vecDistN(VM* vm, int dims, Value* args, const char* message) {
  double a[4];
  double b[4];
  if (!vecRead(vm, args[0], dims, a, message)) return NULL_VAL;
  if (!vecRead(vm, args[1], dims, b, message)) return NULL_VAL;
  double sum = 0.0;
  for (int i = 0; i < dims; i++) {
    double d = b[i] - a[i];
    sum += d * d;
  }
  return NUMBER_VAL(sqrt(sum));
}

static Value nativeVec2Make(VM* vm, int argc, Value* args) {
  (void)argc;
  if (!IS_NUMBER(args[0]) || !IS_NUMBER(args[1])) {
    return runtimeErrorValue(vm, "vec2.make expects (x, y) numbers.");
  }
  double values[2] = { AS_NUMBER(args[0]), AS_NUMBER(args[1]) };
  return vecMake(vm, 2, values);
}

static Value nativeVec2Add(VM* vm, int argc, Value* args) {
  (void)argc;
  return vecAddN(vm, 2, args, "vec2.add expects two vec2 arrays.");
}

static Value nativeVec2Sub(VM* vm, int argc, Value* args) {
  (void)argc;
  return vecSubN(vm, 2, args, "vec2.sub expects two vec2 arrays.");
}

static Value nativeVec2Scale(VM* vm, int argc, Value* args) {
  (void)argc;
  return vecScaleN(vm, 2, args, "vec2.scale expects (vec2, scalar).");
}

static Value nativeVec2Dot(VM* vm, int argc, Value* args) {
  (void)argc;
  return vecDotN(vm, 2, args, "vec2.dot expects two vec2 arrays.");
}

static Value nativeVec2Len(VM* vm, int argc, Value* args) {
  (void)argc;
  return vecLenN(vm, 2, args, "vec2.len expects a vec2 array.");
}

static Value nativeVec2Norm(VM* vm, int argc, Value* args) {
  (void)argc;
  return vecNormN(vm, 2, args, "vec2.norm expects a vec2 array.");
}

static Value nativeVec2Lerp(VM* vm, int argc, Value* args) {
  (void)argc;
  return vecLerpN(vm, 2, args, "vec2.lerp expects (a, b, t).");
}

static Value nativeVec2Dist(VM* vm, int argc, Value* args) {
  (void)argc;
  return vecDistN(vm, 2, args, "vec2.dist expects two vec2 arrays.");
}

static Value nativeVec3Make(VM* vm, int argc, Value* args) {
  (void)argc;
  if (!IS_NUMBER(args[0]) || !IS_NUMBER(args[1]) || !IS_NUMBER(args[2])) {
    return runtimeErrorValue(vm, "vec3.make expects (x, y, z) numbers.");
  }
  double values[3] = { AS_NUMBER(args[0]), AS_NUMBER(args[1]), AS_NUMBER(args[2]) };
  return vecMake(vm, 3, values);
}

static Value nativeVec3Add(VM* vm, int argc, Value* args) {
  (void)argc;
  return vecAddN(vm, 3, args, "vec3.add expects two vec3 arrays.");
}

static Value nativeVec3Sub(VM* vm, int argc, Value* args) {
  (void)argc;
  return vecSubN(vm, 3, args, "vec3.sub expects two vec3 arrays.");
}

static Value nativeVec3Scale(VM* vm, int argc, Value* args) {
  (void)argc;
  return vecScaleN(vm, 3, args, "vec3.scale expects (vec3, scalar).");
}

static Value nativeVec3Dot(VM* vm, int argc, Value* args) {
  (void)argc;
  return vecDotN(vm, 3, args, "vec3.dot expects two vec3 arrays.");
}

static Value nativeVec3Len(VM* vm, int argc, Value* args) {
  (void)argc;
  return vecLenN(vm, 3, args, "vec3.len expects a vec3 array.");
}

static Value nativeVec3Norm(VM* vm, int argc, Value* args) {
  (void)argc;
  return vecNormN(vm, 3, args, "vec3.norm expects a vec3 array.");
}

static Value nativeVec3Lerp(VM* vm, int argc, Value* args) {
  (void)argc;
  return vecLerpN(vm, 3, args, "vec3.lerp expects (a, b, t).");
}

static Value nativeVec3Dist(VM* vm, int argc, Value* args) {
  (void)argc;
  return vecDistN(vm, 3, args, "vec3.dist expects two vec3 arrays.");
}

static Value nativeVec3Cross(VM* vm, int argc, Value* args) {
  (void)argc;
  double a[3];
  double b[3];
  if (!vecRead(vm, args[0], 3, a, "vec3.cross expects two vec3 arrays.")) return NULL_VAL;
  if (!vecRead(vm, args[1], 3, b, "vec3.cross expects two vec3 arrays.")) return NULL_VAL;
  double out[3] = {
    a[1] * b[2] - a[2] * b[1],
    a[2] * b[0] - a[0] * b[2],
    a[0] * b[1] - a[1] * b[0]
  };
  return vecMake(vm, 3, out);
}

static Value nativeVec4Make(VM* vm, int argc, Value* args) {
  (void)argc;
  if (!IS_NUMBER(args[0]) || !IS_NUMBER(args[1]) ||
      !IS_NUMBER(args[2]) || !IS_NUMBER(args[3])) {
    return runtimeErrorValue(vm, "vec4.make expects (x, y, z, w) numbers.");
  }
  double values[4] = {
    AS_NUMBER(args[0]), AS_NUMBER(args[1]),
    AS_NUMBER(args[2]), AS_NUMBER(args[3])
  };
  return vecMake(vm, 4, values);
}

static Value nativeVec4Add(VM* vm, int argc, Value* args) {
  (void)argc;
  return vecAddN(vm, 4, args, "vec4.add expects two vec4 arrays.");
}

static Value nativeVec4Sub(VM* vm, int argc, Value* args) {
  (void)argc;
  return vecSubN(vm, 4, args, "vec4.sub expects two vec4 arrays.");
}

static Value nativeVec4Scale(VM* vm, int argc, Value* args) {
  (void)argc;
  return vecScaleN(vm, 4, args, "vec4.scale expects (vec4, scalar).");
}

static Value nativeVec4Dot(VM* vm, int argc, Value* args) {
  (void)argc;
  return vecDotN(vm, 4, args, "vec4.dot expects two vec4 arrays.");
}

static Value nativeVec4Len(VM* vm, int argc, Value* args) {
  (void)argc;
  return vecLenN(vm, 4, args, "vec4.len expects a vec4 array.");
}

static Value nativeVec4Norm(VM* vm, int argc, Value* args) {
  (void)argc;
  return vecNormN(vm, 4, args, "vec4.norm expects a vec4 array.");
}

static Value nativeVec4Lerp(VM* vm, int argc, Value* args) {
  (void)argc;
  return vecLerpN(vm, 4, args, "vec4.lerp expects (a, b, t).");
}

static Value nativeVec4Dist(VM* vm, int argc, Value* args) {
  (void)argc;
  return vecDistN(vm, 4, args, "vec4.dist expects two vec4 arrays.");
}

#ifdef _WIN32
static wchar_t* utf8ToWide(const char* chars, int length) {
  int needed = MultiByteToWideChar(CP_UTF8, 0, chars, length, NULL, 0);
  if (needed <= 0) return NULL;
  int alloc = length == -1 ? needed : needed + 1;
  wchar_t* buffer = (wchar_t*)malloc(sizeof(wchar_t) * (size_t)alloc);
  if (!buffer) {
    fprintf(stderr, "Out of memory.\n");
    exit(1);
  }
  MultiByteToWideChar(CP_UTF8, 0, chars, length, buffer, needed);
  if (length != -1) {
    buffer[needed] = L'\0';
  }
  return buffer;
}

static wchar_t* wideSubstring(const wchar_t* start, DWORD length) {
  wchar_t* buffer = (wchar_t*)malloc(sizeof(wchar_t) * ((size_t)length + 1));
  if (!buffer) {
    fprintf(stderr, "Out of memory.\n");
    exit(1);
  }
  if (length > 0) {
    wmemcpy(buffer, start, length);
  }
  buffer[length] = L'\0';
  return buffer;
}

static char* wideToUtf8(const wchar_t* chars, int length, int* outLength) {
  int needed = WideCharToMultiByte(CP_UTF8, 0, chars, length, NULL, 0, NULL, NULL);
  if (needed <= 0) return NULL;
  int alloc = length == -1 ? needed : needed + 1;
  char* buffer = (char*)malloc((size_t)alloc);
  if (!buffer) {
    fprintf(stderr, "Out of memory.\n");
    exit(1);
  }
  WideCharToMultiByte(CP_UTF8, 0, chars, length, buffer, needed, NULL, NULL);
  if (length == -1) {
    buffer[needed - 1] = '\0';
    if (outLength) *outLength = needed - 1;
  } else {
    buffer[needed] = '\0';
    if (outLength) *outLength = needed;
  }
  return buffer;
}

static Value httpRequest(VM* vm, const char* method, ObjString* url,
                         const char* body, size_t bodyLength, const char* message) {
  Value result = NULL_VAL;
  bool ok = false;

  wchar_t* wideUrl = utf8ToWide(url->chars, -1);
  if (!wideUrl) return runtimeErrorValue(vm, message);

  URL_COMPONENTS parts;
  memset(&parts, 0, sizeof(parts));
  parts.dwStructSize = sizeof(parts);
  parts.dwSchemeLength = (DWORD)-1;
  parts.dwHostNameLength = (DWORD)-1;
  parts.dwUrlPathLength = (DWORD)-1;
  parts.dwExtraInfoLength = (DWORD)-1;

  if (!WinHttpCrackUrl(wideUrl, 0, 0, &parts)) {
    free(wideUrl);
    return runtimeErrorValue(vm, message);
  }

  wchar_t* host = wideSubstring(parts.lpszHostName, parts.dwHostNameLength);
  DWORD pathLength = parts.dwUrlPathLength + parts.dwExtraInfoLength;
  wchar_t* path = NULL;
  if (pathLength == 0) {
    path = wideSubstring(L"/", 1);
  } else {
    path = (wchar_t*)malloc(sizeof(wchar_t) * ((size_t)pathLength + 1));
    if (!path) {
      fprintf(stderr, "Out of memory.\n");
      exit(1);
    }
    if (parts.dwUrlPathLength > 0) {
      wmemcpy(path, parts.lpszUrlPath, parts.dwUrlPathLength);
    }
    if (parts.dwExtraInfoLength > 0) {
      wmemcpy(path + parts.dwUrlPathLength, parts.lpszExtraInfo, parts.dwExtraInfoLength);
    }
    path[pathLength] = L'\0';
  }

  wchar_t* wideMethod = utf8ToWide(method, -1);
  if (!wideMethod) {
    free(wideUrl);
    free(host);
    free(path);
    return runtimeErrorValue(vm, message);
  }

  HINTERNET session = WinHttpOpen(L"Erkao/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                  WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!session) goto cleanup;

  HINTERNET connect = WinHttpConnect(session, host, parts.nPort, 0);
  if (!connect) {
    WinHttpCloseHandle(session);
    goto cleanup;
  }

  DWORD flags = parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
  HINTERNET request = WinHttpOpenRequest(connect, wideMethod, path, NULL,
                                         WINHTTP_NO_REFERER,
                                         WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
  if (!request) {
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    goto cleanup;
  }

  BOOL sent = WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                 bodyLength > 0 ? (LPVOID)body : WINHTTP_NO_REQUEST_DATA,
                                 (DWORD)bodyLength, (DWORD)bodyLength, 0);
  if (!sent) goto request_cleanup;

  if (!WinHttpReceiveResponse(request, NULL)) goto request_cleanup;

  DWORD status = 0;
  DWORD statusSize = sizeof(status);
  WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                      WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize,
                      WINHTTP_NO_HEADER_INDEX);

  DWORD headerSize = 0;
  wchar_t* headerWide = NULL;
  if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_RAW_HEADERS_CRLF,
                           WINHTTP_HEADER_NAME_BY_INDEX, NULL, &headerSize,
                           WINHTTP_NO_HEADER_INDEX)) {
    if (GetLastError() == ERROR_INSUFFICIENT_BUFFER && headerSize > 0) {
      headerWide = (wchar_t*)malloc(headerSize);
      if (!headerWide) {
        fprintf(stderr, "Out of memory.\n");
        exit(1);
      }
      WinHttpQueryHeaders(request, WINHTTP_QUERY_RAW_HEADERS_CRLF,
                          WINHTTP_HEADER_NAME_BY_INDEX, headerWide, &headerSize,
                          WINHTTP_NO_HEADER_INDEX);
    }
  }

  ByteBuffer bodyBuffer;
  bufferInit(&bodyBuffer);

  for (;;) {
    DWORD available = 0;
    if (!WinHttpQueryDataAvailable(request, &available)) break;
    if (available == 0) break;

    char* chunk = (char*)malloc(available);
    if (!chunk) {
      fprintf(stderr, "Out of memory.\n");
      exit(1);
    }
    DWORD read = 0;
    if (!WinHttpReadData(request, chunk, available, &read)) {
      free(chunk);
      break;
    }
    if (read > 0) {
      bufferAppendN(&bodyBuffer, chunk, read);
    }
    free(chunk);
  }

  ObjMap* response = newMap(vm);
  mapSet(response, copyString(vm, "status"), NUMBER_VAL((double)status));
  mapSet(response, copyString(vm, "body"),
         OBJ_VAL(copyStringWithLength(vm,
                                      bodyBuffer.data ? bodyBuffer.data : "",
                                      (int)bodyBuffer.length)));

  if (headerWide) {
    int headerLength = 0;
    char* headerUtf8 = wideToUtf8(headerWide, -1, &headerLength);
    if (headerUtf8) {
      mapSet(response, copyString(vm, "headers"),
             OBJ_VAL(copyStringWithLength(vm, headerUtf8, headerLength)));
      free(headerUtf8);
    } else {
      mapSet(response, copyString(vm, "headers"), OBJ_VAL(copyString(vm, "")));
    }
    free(headerWide);
  } else {
    mapSet(response, copyString(vm, "headers"), OBJ_VAL(copyString(vm, "")));
  }

  bufferFree(&bodyBuffer);
  result = OBJ_VAL(response);
  ok = true;

request_cleanup:
  WinHttpCloseHandle(request);
  WinHttpCloseHandle(connect);
  WinHttpCloseHandle(session);

cleanup:
  free(wideUrl);
  free(host);
  free(path);
  free(wideMethod);
  if (!ok) {
    return runtimeErrorValue(vm, message);
  }
  return result;
}

static Value nativeHttpGet(VM* vm, int argc, Value* args) {
  (void)argc;
  if (!isObjType(args[0], OBJ_STRING)) {
    return runtimeErrorValue(vm, "http.get expects a url string.");
  }
  return httpRequest(vm, "GET", (ObjString*)AS_OBJ(args[0]), NULL, 0, "http.get failed.");
}

static Value nativeHttpPost(VM* vm, int argc, Value* args) {
  (void)argc;
  if (!isObjType(args[0], OBJ_STRING) || !isObjType(args[1], OBJ_STRING)) {
    return runtimeErrorValue(vm, "http.post expects (url, body) strings.");
  }
  ObjString* body = (ObjString*)AS_OBJ(args[1]);
  return httpRequest(vm, "POST", (ObjString*)AS_OBJ(args[0]),
                     body->chars, (size_t)body->length, "http.post failed.");
}

static Value nativeHttpRequest(VM* vm, int argc, Value* args) {
  (void)argc;
  if (!isObjType(args[0], OBJ_STRING) || !isObjType(args[1], OBJ_STRING)) {
    return runtimeErrorValue(vm, "http.request expects (method, url, body).");
  }
  const char* body = NULL;
  size_t bodyLength = 0;
  if (!IS_NULL(args[2])) {
    if (!isObjType(args[2], OBJ_STRING)) {
      return runtimeErrorValue(vm, "http.request expects body to be a string or null.");
    }
    ObjString* bodyString = (ObjString*)AS_OBJ(args[2]);
    body = bodyString->chars;
    bodyLength = (size_t)bodyString->length;
  }
  ObjString* method = (ObjString*)AS_OBJ(args[0]);
  return httpRequest(vm, method->chars, (ObjString*)AS_OBJ(args[1]),
                     body, bodyLength, "http.request failed.");
}
#else
static bool httpEnsureCurl(void) {
  static bool initialized = false;
  static bool ok = false;
  if (!initialized) {
    ok = (curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK);
    initialized = true;
  }
  return ok;
}

static size_t httpWriteCallback(char* contents, size_t size, size_t nmemb, void* userp) {
  size_t total = size * nmemb;
  ByteBuffer* buffer = (ByteBuffer*)userp;
  bufferAppendN(buffer, contents, total);
  return total;
}

static Value httpRequest(VM* vm, const char* method, ObjString* url,
                         const char* body, size_t bodyLength, const char* message) {
  if (!httpEnsureCurl()) {
    return runtimeErrorValue(vm, message);
  }

  CURL* curl = curl_easy_init();
  if (!curl) {
    return runtimeErrorValue(vm, message);
  }

  ByteBuffer bodyBuffer;
  ByteBuffer headerBuffer;
  bufferInit(&bodyBuffer);
  bufferInit(&headerBuffer);

  curl_easy_setopt(curl, CURLOPT_URL, url->chars);
  curl_easy_setopt(curl, CURLOPT_USERAGENT, "Erkao/1.0");
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, httpWriteCallback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &bodyBuffer);
  curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, httpWriteCallback);
  curl_easy_setopt(curl, CURLOPT_HEADERDATA, &headerBuffer);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

  bool isGet = strcmp(method, "GET") == 0;
  bool isPost = strcmp(method, "POST") == 0;

  if (isPost) {
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
  } else if (!isGet || bodyLength > 0) {
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method);
  }

  if (bodyLength > 0) {
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)bodyLength);
  } else if (isPost) {
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)0);
  }

  CURLcode code = curl_easy_perform(curl);
  if (code != CURLE_OK) {
    curl_easy_cleanup(curl);
    bufferFree(&bodyBuffer);
    bufferFree(&headerBuffer);
    return runtimeErrorValue(vm, message);
  }

  long status = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);

  ObjMap* response = newMap(vm);
  mapSet(response, copyString(vm, "status"), NUMBER_VAL((double)status));
  mapSet(response, copyString(vm, "body"),
         OBJ_VAL(copyStringWithLength(vm,
                                      bodyBuffer.data ? bodyBuffer.data : "",
                                      (int)bodyBuffer.length)));
  mapSet(response, copyString(vm, "headers"),
         OBJ_VAL(copyStringWithLength(vm,
                                      headerBuffer.data ? headerBuffer.data : "",
                                      (int)headerBuffer.length)));

  curl_easy_cleanup(curl);
  bufferFree(&bodyBuffer);
  bufferFree(&headerBuffer);
  return OBJ_VAL(response);
}

static Value nativeHttpGet(VM* vm, int argc, Value* args) {
  (void)argc;
  if (!isObjType(args[0], OBJ_STRING)) {
    return runtimeErrorValue(vm, "http.get expects a url string.");
  }
  return httpRequest(vm, "GET", (ObjString*)AS_OBJ(args[0]), NULL, 0, "http.get failed.");
}

static Value nativeHttpPost(VM* vm, int argc, Value* args) {
  (void)argc;
  if (!isObjType(args[0], OBJ_STRING) || !isObjType(args[1], OBJ_STRING)) {
    return runtimeErrorValue(vm, "http.post expects (url, body) strings.");
  }
  ObjString* body = (ObjString*)AS_OBJ(args[1]);
  return httpRequest(vm, "POST", (ObjString*)AS_OBJ(args[0]),
                     body->chars, (size_t)body->length, "http.post failed.");
}

static Value nativeHttpRequest(VM* vm, int argc, Value* args) {
  (void)argc;
  if (!isObjType(args[0], OBJ_STRING) || !isObjType(args[1], OBJ_STRING)) {
    return runtimeErrorValue(vm, "http.request expects (method, url, body).");
  }
  const char* body = NULL;
  size_t bodyLength = 0;
  if (!IS_NULL(args[2])) {
    if (!isObjType(args[2], OBJ_STRING)) {
      return runtimeErrorValue(vm, "http.request expects body to be a string or null.");
    }
    ObjString* bodyString = (ObjString*)AS_OBJ(args[2]);
    body = bodyString->chars;
    bodyLength = (size_t)bodyString->length;
  }
  ObjString* method = (ObjString*)AS_OBJ(args[0]);
  return httpRequest(vm, method->chars, (ObjString*)AS_OBJ(args[1]),
                     body, bodyLength, "http.request failed.");
}
#endif

#define HTTP_MAX_REQUEST_BYTES 65536

#ifdef _WIN32
typedef SOCKET ErkaoSocket;
#define ERKAO_INVALID_SOCKET INVALID_SOCKET
#define ERKAO_SOCKET_ERROR SOCKET_ERROR
#define erkaoCloseSocket closesocket
#else
typedef int ErkaoSocket;
#define ERKAO_INVALID_SOCKET (-1)
#define ERKAO_SOCKET_ERROR (-1)
#define erkaoCloseSocket close
#endif

static bool httpSocketStartup(void) {
#ifdef _WIN32
  static bool started = false;
  if (!started) {
    WSADATA data;
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
      return false;
    }
    started = true;
  }
#endif
  return true;
}

static bool httpSocketAddrInUse(void) {
#ifdef _WIN32
  return WSAGetLastError() == WSAEADDRINUSE;
#else
  return errno == EADDRINUSE;
#endif
}

static int httpSocketGetPort(ErkaoSocket server) {
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
#ifdef _WIN32
  int len = (int)sizeof(addr);
  if (getsockname(server, (struct sockaddr*)&addr, &len) == SOCKET_ERROR) {
    return -1;
  }
#else
  socklen_t len = sizeof(addr);
  if (getsockname(server, (struct sockaddr*)&addr, &len) < 0) {
    return -1;
  }
#endif
  return (int)ntohs(addr.sin_port);
}

static bool httpBindServerSocket(ErkaoSocket* out, int port, int* outPort, bool* outInUse) {
  if (outInUse) *outInUse = false;

  ErkaoSocket server = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (server == ERKAO_INVALID_SOCKET) {
    return false;
  }

  int opt = 1;
  setsockopt(server, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));
#ifdef SO_NOSIGPIPE
  setsockopt(server, SOL_SOCKET, SO_NOSIGPIPE, (const char*)&opt, sizeof(opt));
#endif

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons((uint16_t)port);

  if (bind(server, (struct sockaddr*)&addr, sizeof(addr)) == ERKAO_SOCKET_ERROR) {
    if (outInUse) *outInUse = httpSocketAddrInUse();
    erkaoCloseSocket(server);
    return false;
  }

  if (listen(server, SOMAXCONN) == ERKAO_SOCKET_ERROR) {
    erkaoCloseSocket(server);
    return false;
  }

  int boundPort = httpSocketGetPort(server);
  if (boundPort <= 0) {
    erkaoCloseSocket(server);
    return false;
  }

  printf("DEBUG: Socket bound to port %d\n", boundPort);
  *out = server;
  *outPort = boundPort;
  return true;
}

static bool httpPortFromValue(VM* vm, Value value, int* outPort) {
  if (IS_NULL(value)) {
    *outPort = 0;
    return true;
  }
  if (!IS_NUMBER(value)) {
    runtimeErrorValue(vm, "http.serve expects port to be a number or null.");
    return false;
  }
  double number = AS_NUMBER(value);
  double truncated = floor(number);
  if (number != truncated) {
    runtimeErrorValue(vm, "http.serve expects port to be an integer.");
    return false;
  }
  if (number < 0.0 || number > 65535.0) {
    runtimeErrorValue(vm, "http.serve expects port in range 0-65535.");
    return false;
  }
  *outPort = (int)number;
  return true;
}

static bool httpFindHeaderEnd(const char* data, size_t length, size_t* outIndex) {
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

static bool httpReadHeaders(ErkaoSocket client, ByteBuffer* buffer, size_t* headerEnd) {
  char chunk[1024];
  while (buffer->length < HTTP_MAX_REQUEST_BYTES) {
    int received = recv(client, chunk, (int)sizeof(chunk), 0);
    if (received <= 0) {
      return false;
    }
    bufferAppendN(buffer, chunk, (size_t)received);
    if (httpFindHeaderEnd(buffer->data, buffer->length, headerEnd)) {
      return true;
    }
  }
  return false;
}

static bool httpParseRequestLine(const char* data, size_t headerEnd,
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

static bool httpStringEqualsIgnoreCaseN(const char* left, int leftLen, const char* right) {
  int rightLen = (int)strlen(right);
  if (leftLen != rightLen) return false;
  for (int i = 0; i < leftLen; i++) {
    unsigned char a = (unsigned char)left[i];
    unsigned char b = (unsigned char)right[i];
    if (tolower(a) != tolower(b)) return false;
  }
  return true;
}

static void httpAppendHeader(ByteBuffer* buffer, const char* name, const char* value) {
  bufferAppendN(buffer, name, strlen(name));
  bufferAppendN(buffer, ": ", 2);
  bufferAppendN(buffer, value, strlen(value));
  bufferAppendN(buffer, "\r\n", 2);
}

static const char* httpStatusText(int status) {
  switch (status) {
    case 200:
      return "OK";
    case 201:
      return "Created";
    case 204:
      return "No Content";
    case 400:
      return "Bad Request";
    case 404:
      return "Not Found";
    case 500:
      return "Internal Server Error";
    default:
      return "OK";
  }
}

static bool httpAppendHeadersFromMap(ByteBuffer* buffer, ObjMap* headers, bool* hasContentType) {
  if (!headers) return true;
  for (int i = 0; i < headers->capacity; i++) {
    MapEntryValue* entry = &headers->entries[i];
    if (!entry->key) continue;
    if (!isObjType(entry->value, OBJ_STRING)) continue;
    ObjString* key = entry->key;
    ObjString* value = (ObjString*)AS_OBJ(entry->value);
    if (httpStringEqualsIgnoreCaseN(key->chars, key->length, "Content-Type")) {
      if (hasContentType) *hasContentType = true;
    }
    httpAppendHeader(buffer, key->chars, value->chars);
  }
  return true;
}

static bool httpSendAll(ErkaoSocket client, const char* data, size_t length) {
  size_t sent = 0;
  while (sent < length) {
    int chunk = (int)(length - sent);
    int wrote = send(client, data + sent, chunk, 0);
    if (wrote <= 0) return false;
    sent += (size_t)wrote;
  }
  return true;
}

static bool httpSendResponse(ErkaoSocket client, int status, const char* body,
                             size_t bodyLength, ObjMap* headers, ObjMap* corsConfig) {
  ByteBuffer response;
  bufferInit(&response);

  char statusLine[64];
  const char* statusText = httpStatusText(status);
  int statusLen = snprintf(statusLine, sizeof(statusLine),
                           "HTTP/1.1 %d %s\r\n", status, statusText);
  if (statusLen < 0) statusLen = 0;
  bufferAppendN(&response, statusLine, (size_t)statusLen);

  bool hasContentType = false;
  httpAppendHeadersFromMap(&response, headers, &hasContentType);
  if (!hasContentType) {
    httpAppendHeader(&response, "Content-Type", "text/plain; charset=utf-8");
  }

  if (corsConfig) {
    Value originVal;
    ObjString* originKey = copyString(corsConfig->vm, "origin");
    if (mapGet(corsConfig, originKey, &originVal) && isObjType(originVal, OBJ_STRING)) {
      ObjString* origin = (ObjString*)AS_OBJ(originVal);
      httpAppendHeader(&response, "Access-Control-Allow-Origin", origin->chars);
    }
    
    Value methodsVal;
    ObjString* methodsKey = copyString(corsConfig->vm, "methods");
    if (mapGet(corsConfig, methodsKey, &methodsVal) && isObjType(methodsVal, OBJ_STRING)) {
      ObjString* methods = (ObjString*)AS_OBJ(methodsVal);
      httpAppendHeader(&response, "Access-Control-Allow-Methods", methods->chars);
    }
    
    Value headersVal;
    ObjString* headersKey = copyString(corsConfig->vm, "headers");
    if (mapGet(corsConfig, headersKey, &headersVal) && isObjType(headersVal, OBJ_STRING)) {
      ObjString* hdrs = (ObjString*)AS_OBJ(headersVal);
      httpAppendHeader(&response, "Access-Control-Allow-Headers", hdrs->chars);
    }
  }

  char lengthValue[64];
  int lengthLen = snprintf(lengthValue, sizeof(lengthValue), "%zu", bodyLength);
  if (lengthLen < 0) lengthLen = 0;
  httpAppendHeader(&response, "Content-Length", lengthValue);
  httpAppendHeader(&response, "Connection", "close");
  bufferAppendN(&response, "\r\n", 2);

  if (bodyLength > 0 && body) {
    bufferAppendN(&response, body, bodyLength);
  }

  bool ok = httpSendAll(client,
                        response.data ? response.data : "",
                        response.length);
  bufferFree(&response);
  return ok;
}

static void httpLogRequest(const struct sockaddr_in* addr,
                           const char* path, size_t pathLen) {
  char ip[INET_ADDRSTRLEN] = "unknown";
#ifdef _WIN32
  if (addr) {
    InetNtopA(AF_INET, (void*)&addr->sin_addr, ip, (DWORD)sizeof(ip));
  }
#else
  if (addr) {
    if (!inet_ntop(AF_INET, &addr->sin_addr, ip, sizeof(ip))) {
      snprintf(ip, sizeof(ip), "unknown");
    }
  }
#endif

  char timeBuf[32] = {0};
  time_t now = time(NULL);
  struct tm localTime;
#ifdef _WIN32
  localtime_s(&localTime, &now);
#else
  localtime_r(&now, &localTime);
#endif
  strftime(timeBuf, sizeof(timeBuf), "%Y-%m-%d %H:%M:%S", &localTime);

  if (pathLen == 0 || !path) {
    printf("[%s] [%s] Called /\n", ip, timeBuf);
  } else {
    printf("[%s] [%s] Called %.*s\n", ip, timeBuf, (int)pathLen, path);
  }
  fflush(stdout);
}

static long httpGetContentLength(const char* headers, size_t headerEnd) {
  const char* clHeader = "Content-Length:";
  size_t clLen = strlen(clHeader);
  const char* cursor = headers;
  const char* end = headers + headerEnd;
  while (cursor < end) {
    const char* lineEnd = memchr(cursor, '\n', (size_t)(end - cursor));
    if (!lineEnd) break;
    size_t lineLen = (size_t)(lineEnd - cursor);
    if (lineLen > 0 && cursor[lineLen - 1] == '\r') lineLen--;
    if (lineLen > clLen && httpStringEqualsIgnoreCaseN(cursor, (int)clLen, clHeader)) {
      const char* value = cursor + clLen;
      while (*value == ' ' && value < lineEnd) value++;
      long length = strtol(value, NULL, 10);
      return length > 0 ? length : 0;
    }
    cursor = lineEnd + 1;
  }
  return 0;
}

static ObjMap* httpParseHeaders(VM* vm, const char* data, size_t headerEnd) {
  ObjMap* headers = newMap(vm);
  const char* cursor = data;
  const char* end = data + headerEnd;
  
  const char* firstLine = memchr(cursor, '\n', (size_t)(end - cursor));
  if (firstLine) cursor = firstLine + 1;
  
  while (cursor < end) {
    const char* lineEnd = memchr(cursor, '\n', (size_t)(end - cursor));
    if (!lineEnd) break;
    size_t lineLen = (size_t)(lineEnd - cursor);
    if (lineLen > 0 && cursor[lineLen - 1] == '\r') lineLen--;
    if (lineLen == 0) break;
    
    const char* colon = memchr(cursor, ':', lineLen);
    if (colon && colon > cursor) {
      size_t keyLen = (size_t)(colon - cursor);
      const char* value = colon + 1;
      while (*value == ' ' && value < cursor + lineLen) value++;
      size_t valueLen = lineLen - (size_t)(value - cursor);
      
      ObjString* key = copyStringWithLength(vm, cursor, (int)keyLen);
      ObjString* val = copyStringWithLength(vm, value, (int)valueLen);
      mapSet(headers, key, OBJ_VAL(val));
    }
    cursor = lineEnd + 1;
  }
  return headers;
}

static bool httpReadBody(ErkaoSocket client, ByteBuffer* buffer, size_t headerEnd, long contentLength) {
  if (contentLength <= 0) return true;

  size_t alreadyRead = buffer->length > headerEnd ? buffer->length - headerEnd : 0;
  size_t remaining = (size_t)contentLength > alreadyRead ? (size_t)contentLength - alreadyRead : 0;
  
  char chunk[1024];
  while (remaining > 0 && buffer->length < HTTP_MAX_REQUEST_BYTES) {
    size_t toRead = remaining < sizeof(chunk) ? remaining : sizeof(chunk);
    int received = recv(client, chunk, (int)toRead, 0);
    if (received <= 0) return false;
    bufferAppendN(buffer, chunk, (size_t)received);
    remaining -= (size_t)received;
  }
  return true;
}

static ObjMap* httpCreateRequestObject(VM* vm, const char* method, size_t methodLen,
                                        const char* path, size_t pathLen,
                                        ObjMap* headers, const char* body, size_t bodyLen) {
  ObjMap* request = newMap(vm);
  
  ObjString* methodKey = copyString(vm, "method");
  ObjString* methodVal = copyStringWithLength(vm, method, (int)methodLen);
  mapSet(request, methodKey, OBJ_VAL(methodVal));
  
  ObjString* pathKey = copyString(vm, "path");
  ObjString* pathVal = copyStringWithLength(vm, path, (int)pathLen);
  mapSet(request, pathKey, OBJ_VAL(pathVal));
  
  ObjString* headersKey = copyString(vm, "headers");
  mapSet(request, headersKey, OBJ_VAL(headers));
  
  ObjString* bodyKey = copyString(vm, "body");
  ObjString* bodyVal = copyStringWithLength(vm, body ? body : "", body ? (int)bodyLen : 0);
  mapSet(request, bodyKey, OBJ_VAL(bodyVal));
  
  return request;
}

static bool httpResponseFromValue(VM* vm, Value value, int* statusOut,
                                  const char** bodyOut, size_t* bodyLenOut,
                                  ObjMap** headersOut, ObjMap* requestObj) {
  *statusOut = 200;
  *bodyOut = "";
  *bodyLenOut = 0;
  *headersOut = NULL;

  if (isObjType(value, OBJ_FUNCTION) || isObjType(value, OBJ_BOUND_METHOD)) {
    if (!requestObj) {
      return false;
    }
    Value request = OBJ_VAL(requestObj);
    Value result;
    if (!vmCallValue(vm, value, 1, &request, &result)) {
      return false;
    }
    return httpResponseFromValue(vm, result, statusOut, bodyOut, bodyLenOut, headersOut, NULL);
  }

  if (isObjType(value, OBJ_STRING)) {
    ObjString* body = (ObjString*)AS_OBJ(value);
    *bodyOut = body->chars;
    *bodyLenOut = (size_t)body->length;
    return true;
  }

  if (isObjType(value, OBJ_MAP)) {
    ObjMap* response = (ObjMap*)AS_OBJ(value);
    Value statusValue;
    ObjString* statusKey = copyString(vm, "status");
    if (mapGet(response, statusKey, &statusValue)) {
      if (!IS_NUMBER(statusValue)) return false;
      double statusNumber = AS_NUMBER(statusValue);
      double truncated = floor(statusNumber);
      if (statusNumber != truncated || statusNumber < 100.0 || statusNumber > 599.0) {
        return false;
      }
      *statusOut = (int)statusNumber;
    }

    Value bodyValue;
    ObjString* bodyKey = copyString(vm, "body");
    if (mapGet(response, bodyKey, &bodyValue)) {
      if (!isObjType(bodyValue, OBJ_STRING)) return false;
      ObjString* body = (ObjString*)AS_OBJ(bodyValue);
      *bodyOut = body->chars;
      *bodyLenOut = (size_t)body->length;
    }

    Value headersValue;
    ObjString* headersKey = copyString(vm, "headers");
    if (mapGet(response, headersKey, &headersValue)) {
      if (!isObjType(headersValue, OBJ_MAP)) return false;
      *headersOut = (ObjMap*)AS_OBJ(headersValue);
    }

    return true;
  }

  return false;
}

static Value nativeHttpServe(VM* vm, int argc, Value* args) {
  int portValue = 0;
  if (!httpPortFromValue(vm, args[0], &portValue)) return NULL_VAL;
  if (!isObjType(args[1], OBJ_MAP)) {
    return runtimeErrorValue(vm, "http.serve expects (port, routes[, cors]).");
  }

  ObjMap* routes = (ObjMap*)AS_OBJ(args[1]);
  ObjMap* corsConfig = NULL;
  if (argc >= 3 && isObjType(args[2], OBJ_MAP)) {
    corsConfig = (ObjMap*)AS_OBJ(args[2]);
  }
  int requestedPort = portValue;

#ifndef _WIN32
  signal(SIGPIPE, SIG_IGN);
#endif

  if (!httpSocketStartup()) {
    return runtimeErrorValue(vm, "http.serve failed to initialize sockets.");
  }

  ErkaoSocket server = ERKAO_INVALID_SOCKET;
  int boundPort = 0;
  bool inUse = false;
  if (!httpBindServerSocket(&server, requestedPort, &boundPort, &inUse)) {
    if (requestedPort > 0 && inUse) {
      if (!httpBindServerSocket(&server, 0, &boundPort, NULL)) {
        return runtimeErrorValue(vm, "http.serve failed to bind.");
      }
      printf("http.serve port %d in use, selected %d\n", requestedPort, boundPort);
    } else {
      return runtimeErrorValue(vm, "http.serve failed to bind.");
    }
  }

  printf("http.serve listening on http://127.0.0.1:%d\n", boundPort);
  fflush(stdout);

  for (;;) {
    struct sockaddr_in clientAddr;
#ifdef _WIN32
    int addrLen = (int)sizeof(clientAddr);
#else
    socklen_t addrLen = sizeof(clientAddr);
#endif
    ErkaoSocket client = accept(server, (struct sockaddr*)&clientAddr, &addrLen);
    if (client == ERKAO_INVALID_SOCKET) {
      if (errno != EAGAIN && errno != EWOULDBLOCK) {
         printf("DEBUG: accept failed: %d\n", errno);
         fflush(stdout);
      }
      continue;
    }
    printf("DEBUG: Connection accepted from client\n");
    fflush(stdout);

    ByteBuffer request;
    bufferInit(&request);
    size_t headerEnd = 0;
    if (!httpReadHeaders(client, &request, &headerEnd)) {
      bufferFree(&request);
      erkaoCloseSocket(client);
      continue;
    }

    const char* method = NULL;
    size_t methodLen = 0;
    const char* path = NULL;
    size_t pathLen = 0;
    if (!httpParseRequestLine(request.data, headerEnd, &method, &methodLen, &path, &pathLen)) {
      httpSendResponse(client, 400, "bad request", strlen("bad request"), NULL, corsConfig);
      bufferFree(&request);
      erkaoCloseSocket(client);
      continue;
    }

    httpLogRequest(&clientAddr, path, pathLen);

    Value routeValue = NULL_VAL;
    bool found = false;
    char* methodKey = NULL;
    if (methodLen > 0 && pathLen > 0) {
      size_t methodKeyLen = methodLen + 1 + pathLen;
      methodKey = (char*)malloc(methodKeyLen + 1);
      if (!methodKey) {
        fprintf(stderr, "Out of memory.\n");
        exit(1);
      }
      memcpy(methodKey, method, methodLen);
      methodKey[methodLen] = ' ';
      memcpy(methodKey + methodLen + 1, path, pathLen);
      methodKey[methodKeyLen] = '\0';

      ObjString* routeKey = copyStringWithLength(vm, methodKey, (int)methodKeyLen);
      if (mapGet(routes, routeKey, &routeValue)) {
        found = true;
      }
    }

    if (!found) {
      ObjString* routeKey = copyStringWithLength(vm, path, (int)pathLen);
      if (mapGet(routes, routeKey, &routeValue)) {
        found = true;
      }
    }

    free(methodKey);

    if (methodLen == 7 && memcmp(method, "OPTIONS", 7) == 0) {
      httpSendResponse(client, 204, "", 0, NULL, corsConfig);
      bufferFree(&request);
      erkaoCloseSocket(client);
      continue;
    }

    if (!found) {
      httpSendResponse(client, 404, "not found", strlen("not found"), NULL, corsConfig);
      bufferFree(&request);
      erkaoCloseSocket(client);
      continue;
    }

    ObjMap* requestObj = NULL;
    bool isHandler = isObjType(routeValue, OBJ_FUNCTION) || isObjType(routeValue, OBJ_BOUND_METHOD);
    if (isHandler) {
      long contentLength = httpGetContentLength(request.data, headerEnd);
      if (contentLength > 0) {
        httpReadBody(client, &request, headerEnd, contentLength);
      }
      
      ObjMap* requestHeaders = httpParseHeaders(vm, request.data, headerEnd);
      
      const char* requestBody = NULL;
      size_t requestBodyLen = 0;
      if (request.length > headerEnd) {
        requestBody = request.data + headerEnd;
        requestBodyLen = request.length - headerEnd;
      }
      
      requestObj = httpCreateRequestObject(vm, method, methodLen, path, pathLen,
                                           requestHeaders, requestBody, requestBodyLen);
    }

    int status = 200;
    const char* body = "";
    size_t bodyLen = 0;
    ObjMap* headers = NULL;
    if (!httpResponseFromValue(vm, routeValue, &status, &body, &bodyLen, &headers, requestObj)) {
      httpSendResponse(client, 500, "invalid response", strlen("invalid response"), NULL, corsConfig);
      bufferFree(&request);
      erkaoCloseSocket(client);
      continue;
    }

    httpSendResponse(client, status, body, bodyLen, headers, corsConfig);
    bufferFree(&request);
    erkaoCloseSocket(client);
    gcMaybe(vm);
  }

  return NULL_VAL;
}

static Value nativePrint(VM* vm, int argc, Value* args) {
  (void)vm;
  for (int i = 0; i < argc; i++) {
    if (i > 0) printf(" ");
    printValue(args[i]);
  }
  printf("\n");
  return NULL_VAL;
}

static Value nativeClock(VM* vm, int argc, Value* args) {
  (void)vm;
  (void)argc;
  (void)args;
  double seconds = (double)clock() / (double)CLOCKS_PER_SEC;
  return NUMBER_VAL(seconds);
}

static Value nativeType(VM* vm, int argc, Value* args) {
  (void)argc;
  const char* name = valueTypeName(args[0]);
  return OBJ_VAL(copyString(vm, name));
}

static Value nativeLen(VM* vm, int argc, Value* args) {
  (void)argc;
  if (isObjType(args[0], OBJ_STRING)) {
    ObjString* string = (ObjString*)AS_OBJ(args[0]);
    return NUMBER_VAL(string->length);
  }
  if (isObjType(args[0], OBJ_ARRAY)) {
    ObjArray* array = (ObjArray*)AS_OBJ(args[0]);
    return NUMBER_VAL(array->count);
  }
  if (isObjType(args[0], OBJ_MAP)) {
    ObjMap* map = (ObjMap*)AS_OBJ(args[0]);
    return NUMBER_VAL(mapCount(map));
  }
  return runtimeErrorValue(vm, "len() expects a string, array, or map.");
}

static Value nativeArgs(VM* vm, int argc, Value* args) {
  (void)argc;
  (void)args;
  return OBJ_VAL(vm->args);
}

static Value nativePush(VM* vm, int argc, Value* args) {
  (void)argc;
  if (!isObjType(args[0], OBJ_ARRAY)) {
    return runtimeErrorValue(vm, "push() expects an array as the first argument.");
  }
  ObjArray* array = (ObjArray*)AS_OBJ(args[0]);
  arrayWrite(array, args[1]);
  return NUMBER_VAL(array->count);
}

static Value nativeKeys(VM* vm, int argc, Value* args) {
  (void)argc;
  if (!isObjType(args[0], OBJ_MAP)) {
    return runtimeErrorValue(vm, "keys() expects a map.");
  }
  ObjMap* map = (ObjMap*)AS_OBJ(args[0]);
  ObjArray* array = newArrayWithCapacity(vm, map->count);
  for (int i = 0; i < map->capacity; i++) {
    if (!map->entries[i].key) continue;
    arrayWrite(array, OBJ_VAL(map->entries[i].key));
  }
  return OBJ_VAL(array);
}

static Value nativeValues(VM* vm, int argc, Value* args) {
  (void)argc;
  if (!isObjType(args[0], OBJ_MAP)) {
    return runtimeErrorValue(vm, "values() expects a map.");
  }
  ObjMap* map = (ObjMap*)AS_OBJ(args[0]);
  ObjArray* array = newArrayWithCapacity(vm, map->count);
  for (int i = 0; i < map->capacity; i++) {
    if (!map->entries[i].key) continue;
    arrayWrite(array, map->entries[i].value);
  }
  return OBJ_VAL(array);
}

static Value nativeFsReadText(VM* vm, int argc, Value* args) {
  (void)argc;
  if (!isObjType(args[0], OBJ_STRING)) {
    return runtimeErrorValue(vm, "fs.readText expects a path string.");
  }
  ObjString* path = (ObjString*)AS_OBJ(args[0]);
  FILE* file = fopen(path->chars, "rb");
  if (!file) {
    return runtimeErrorValue(vm, "fs.readText failed to open file.");
  }

  fseek(file, 0L, SEEK_END);
  long size = ftell(file);
  rewind(file);
  if (size < 0) {
    fclose(file);
    return runtimeErrorValue(vm, "fs.readText failed to read file size.");
  }

  char* buffer = (char*)malloc((size_t)size + 1);
  if (!buffer) {
    fclose(file);
    return runtimeErrorValue(vm, "fs.readText out of memory.");
  }

  size_t read = fread(buffer, 1, (size_t)size, file);
  buffer[read] = '\0';
  fclose(file);

  ObjString* result = copyStringWithLength(vm, buffer, (int)read);
  free(buffer);
  return OBJ_VAL(result);
}

static Value nativeFsWriteText(VM* vm, int argc, Value* args) {
  (void)argc;
  if (!isObjType(args[0], OBJ_STRING) || !isObjType(args[1], OBJ_STRING)) {
    return runtimeErrorValue(vm, "fs.writeText expects (path, text) strings.");
  }
  ObjString* path = (ObjString*)AS_OBJ(args[0]);
  ObjString* text = (ObjString*)AS_OBJ(args[1]);

  FILE* file = fopen(path->chars, "wb");
  if (!file) {
    return runtimeErrorValue(vm, "fs.writeText failed to open file.");
  }

  size_t written = fwrite(text->chars, 1, (size_t)text->length, file);
  fclose(file);
  if (written != (size_t)text->length) {
    return runtimeErrorValue(vm, "fs.writeText failed to write file.");
  }
  return BOOL_VAL(true);
}

static Value nativeFsExists(VM* vm, int argc, Value* args) {
  (void)argc;
  if (!isObjType(args[0], OBJ_STRING)) {
    return runtimeErrorValue(vm, "fs.exists expects a path string.");
  }
  ObjString* path = (ObjString*)AS_OBJ(args[0]);
#ifdef _WIN32
  DWORD attrs = GetFileAttributesA(path->chars);
  return BOOL_VAL(attrs != INVALID_FILE_ATTRIBUTES);
#else
  struct stat st;
  return BOOL_VAL(stat(path->chars, &st) == 0);
#endif
}

static Value nativeFsCwd(VM* vm, int argc, Value* args) {
  (void)argc;
  (void)args;
#ifdef _WIN32
  DWORD length = GetCurrentDirectoryA(0, NULL);
  if (length == 0) {
    return runtimeErrorValue(vm, "fs.cwd failed to read current directory.");
  }
  char* buffer = (char*)malloc((size_t)length);
  if (!buffer) {
    return runtimeErrorValue(vm, "fs.cwd out of memory.");
  }
  if (GetCurrentDirectoryA(length, buffer) == 0) {
    free(buffer);
    return runtimeErrorValue(vm, "fs.cwd failed to read current directory.");
  }
  ObjString* result = copyString(vm, buffer);
  free(buffer);
  return OBJ_VAL(result);
#else
  char* buffer = getcwd(NULL, 0);
  if (!buffer) {
    return runtimeErrorValue(vm, "fs.cwd failed to read current directory.");
  }
  ObjString* result = copyString(vm, buffer);
  free(buffer);
  return OBJ_VAL(result);
#endif
}

static Value nativeFsListDir(VM* vm, int argc, Value* args) {
  (void)argc;
  if (!isObjType(args[0], OBJ_STRING)) {
    return runtimeErrorValue(vm, "fs.listDir expects a path string.");
  }
  ObjString* path = (ObjString*)AS_OBJ(args[0]);

#ifdef _WIN32
  size_t pathLength = strlen(path->chars);
  size_t patternLength = pathLength + 3;
  char* pattern = (char*)malloc(patternLength);
  if (!pattern) {
    return runtimeErrorValue(vm, "fs.listDir out of memory.");
  }
  snprintf(pattern, patternLength, "%s\\*", path->chars);

  WIN32_FIND_DATAA data;
  HANDLE handle = FindFirstFileA(pattern, &data);
  free(pattern);
  if (handle == INVALID_HANDLE_VALUE) {
    return runtimeErrorValue(vm, "fs.listDir failed to open directory.");
  }

  ObjArray* array = newArray(vm);
  do {
    if (strcmp(data.cFileName, ".") == 0 || strcmp(data.cFileName, "..") == 0) {
      continue;
    }
    arrayWrite(array, OBJ_VAL(copyString(vm, data.cFileName)));
  } while (FindNextFileA(handle, &data));

  FindClose(handle);
  return OBJ_VAL(array);
#else
  DIR* dir = opendir(path->chars);
  if (!dir) {
    return runtimeErrorValue(vm, "fs.listDir failed to open directory.");
  }

  ObjArray* array = newArray(vm);
  struct dirent* entry;
  while ((entry = readdir(dir)) != NULL) {
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
      continue;
    }
    arrayWrite(array, OBJ_VAL(copyString(vm, entry->d_name)));
  }
  closedir(dir);
  return OBJ_VAL(array);
#endif
}

static Value nativeFsIsFile(VM* vm, int argc, Value* args) {
  (void)argc;
  if (!isObjType(args[0], OBJ_STRING)) {
    return runtimeErrorValue(vm, "fs.isFile expects a path string.");
  }
  ObjString* path = (ObjString*)AS_OBJ(args[0]);
  return BOOL_VAL(pathIsFile(path->chars));
}

static Value nativeFsIsDir(VM* vm, int argc, Value* args) {
  (void)argc;
  if (!isObjType(args[0], OBJ_STRING)) {
    return runtimeErrorValue(vm, "fs.isDir expects a path string.");
  }
  ObjString* path = (ObjString*)AS_OBJ(args[0]);
  return BOOL_VAL(pathIsDir(path->chars));
}

static Value nativeFsSize(VM* vm, int argc, Value* args) {
  (void)argc;
  if (!isObjType(args[0], OBJ_STRING)) {
    return runtimeErrorValue(vm, "fs.size expects a path string.");
  }
  ObjString* path = (ObjString*)AS_OBJ(args[0]);
  FILE* file = fopen(path->chars, "rb");
  if (!file) {
    return runtimeErrorValue(vm, "fs.size failed to open file.");
  }
  fseek(file, 0L, SEEK_END);
  long size = ftell(file);
  fclose(file);
  if (size < 0) {
    return runtimeErrorValue(vm, "fs.size failed to read file size.");
  }
  return NUMBER_VAL((double)size);
}

static Value nativeFsGlob(VM* vm, int argc, Value* args) {
  (void)argc;
  if (!isObjType(args[0], OBJ_STRING)) {
    return runtimeErrorValue(vm, "fs.glob expects a pattern string.");
  }
  ObjString* pattern = (ObjString*)AS_OBJ(args[0]);
  const char* patternText = pattern->chars;
  char sep = pickSeparator(patternText, NULL);

  bool hasWildcard = false;
  for (const char* c = patternText; *c; c++) {
    if (*c == '*' || *c == '?') {
      hasWildcard = true;
      break;
    }
  }

  StringList matches;
  stringListInit(&matches);

  if (!hasWildcard) {
    if (pathExists(patternText)) {
      stringListAdd(&matches, patternText);
    }
  } else {
    int start = 0;
    char* root = globRootFromPattern(patternText, sep, &start);
    StringList segments;
    globSplitSegments(patternText, start, &segments);

    const char* error = NULL;
    globWalk(root, sep, &segments, 0, &matches, &error);
    stringListFree(&segments);
    free(root);
    if (error) {
      stringListFree(&matches);
      return runtimeErrorValue(vm, error);
    }
  }

  stringListSort(&matches);
  ObjArray* array = newArrayWithCapacity(vm, matches.count);
  for (int i = 0; i < matches.count; i++) {
    arrayWrite(array, OBJ_VAL(copyString(vm, matches.items[i])));
  }
  stringListFree(&matches);
  return OBJ_VAL(array);
}

static Value nativePathJoin(VM* vm, int argc, Value* args) {
  (void)argc;
  if (!isObjType(args[0], OBJ_STRING) || !isObjType(args[1], OBJ_STRING)) {
    return runtimeErrorValue(vm, "path.join expects (left, right) strings.");
  }
  ObjString* left = (ObjString*)AS_OBJ(args[0]);
  ObjString* right = (ObjString*)AS_OBJ(args[1]);
  if (isAbsolutePathString(right->chars)) {
    return OBJ_VAL(copyStringWithLength(vm, right->chars, right->length));
  }

  char sep = pickSeparator(left->chars, right->chars);
  bool needSep = left->length > 0 &&
                 left->chars[left->length - 1] != '/' &&
                 left->chars[left->length - 1] != '\\';
  size_t total = (size_t)left->length + (needSep ? 1 : 0) + (size_t)right->length;
  char* buffer = (char*)malloc(total + 1);
  if (!buffer) {
    return runtimeErrorValue(vm, "path.join out of memory.");
  }
  memcpy(buffer, left->chars, (size_t)left->length);
  size_t offset = (size_t)left->length;
  if (needSep) {
    buffer[offset++] = sep;
  }
  memcpy(buffer + offset, right->chars, (size_t)right->length);
  buffer[total] = '\0';

  ObjString* result = copyStringWithLength(vm, buffer, (int)total);
  free(buffer);
  return OBJ_VAL(result);
}

static Value nativePathDirname(VM* vm, int argc, Value* args) {
  (void)argc;
  if (!isObjType(args[0], OBJ_STRING)) {
    return runtimeErrorValue(vm, "path.dirname expects a path string.");
  }
  ObjString* path = (ObjString*)AS_OBJ(args[0]);
  const char* sep = findLastSeparator(path->chars);
  if (!sep) {
    return OBJ_VAL(copyString(vm, "."));
  }

  size_t length = (size_t)(sep - path->chars);
  if (length == 0) {
    length = 1;
  } else if (length == 2 && path->chars[1] == ':' &&
             (path->chars[2] == '\\' || path->chars[2] == '/')) {
    length = 3;
  }

  if (length > (size_t)path->length) {
    length = (size_t)path->length;
  }

  return OBJ_VAL(copyStringWithLength(vm, path->chars, (int)length));
}

static Value nativePathBasename(VM* vm, int argc, Value* args) {
  (void)argc;
  if (!isObjType(args[0], OBJ_STRING)) {
    return runtimeErrorValue(vm, "path.basename expects a path string.");
  }
  ObjString* path = (ObjString*)AS_OBJ(args[0]);
  const char* sep = findLastSeparator(path->chars);
  const char* base = sep ? sep + 1 : path->chars;
  return OBJ_VAL(copyString(vm, base));
}

static Value nativePathExtname(VM* vm, int argc, Value* args) {
  (void)argc;
  if (!isObjType(args[0], OBJ_STRING)) {
    return runtimeErrorValue(vm, "path.extname expects a path string.");
  }
  ObjString* path = (ObjString*)AS_OBJ(args[0]);
  const char* sep = findLastSeparator(path->chars);
  const char* base = sep ? sep + 1 : path->chars;
  const char* dot = strrchr(base, '.');
  if (!dot || dot == base) {
    return OBJ_VAL(copyString(vm, ""));
  }
  return OBJ_VAL(copyStringWithLength(vm, dot, (int)strlen(dot)));
}

static Value nativePathIsAbs(VM* vm, int argc, Value* args) {
  (void)argc;
  if (!isObjType(args[0], OBJ_STRING)) {
    return runtimeErrorValue(vm, "path.isAbs expects a path string.");
  }
  ObjString* path = (ObjString*)AS_OBJ(args[0]);
  return BOOL_VAL(isAbsolutePathString(path->chars));
}

static Value nativePathStem(VM* vm, int argc, Value* args) {
  (void)argc;
  if (!isObjType(args[0], OBJ_STRING)) {
    return runtimeErrorValue(vm, "path.stem expects a path string.");
  }
  ObjString* path = (ObjString*)AS_OBJ(args[0]);
  const char* sep = findLastSeparator(path->chars);
  const char* base = sep ? sep + 1 : path->chars;
  const char* dot = strrchr(base, '.');
  if (!dot || dot == base) {
    return OBJ_VAL(copyString(vm, base));
  }
  return OBJ_VAL(copyStringWithLength(vm, base, (int)(dot - base)));
}

static Value nativePathNormalize(VM* vm, int argc, Value* args) {
  (void)argc;
  if (!isObjType(args[0], OBJ_STRING)) {
    return runtimeErrorValue(vm, "path.normalize expects a path string.");
  }
  ObjString* path = (ObjString*)AS_OBJ(args[0]);
  const char* text = path->chars;
  bool hasBackslash = strchr(text, '\\') != NULL;
  char sep = hasBackslash ? '\\' : '/';
  bool isAbs = isAbsolutePathString(text);

  StringList parts;
  stringListInit(&parts);

  int start = 0;
  char drive = '\0';
  if (isAbs && ((text[0] >= 'A' && text[0] <= 'Z') ||
                (text[0] >= 'a' && text[0] <= 'z')) &&
      text[1] == ':' && (text[2] == '\\' || text[2] == '/')) {
    drive = text[0];
    start = 3;
  } else if (isAbs && (text[0] == '\\' || text[0] == '/')) {
    start = 1;
  }

  const char* cursor = text + start;
  while (*cursor) {
    while (*cursor == '/' || *cursor == '\\') cursor++;
    if (*cursor == '\0') break;
    const char* begin = cursor;
    while (*cursor && *cursor != '/' && *cursor != '\\') cursor++;
    size_t length = (size_t)(cursor - begin);
    if (length == 0) continue;
    if (length == 1 && begin[0] == '.') {
      continue;
    }
    if (length == 2 && begin[0] == '.' && begin[1] == '.') {
      if (parts.count > 0 && strcmp(parts.items[parts.count - 1], "..") != 0) {
        free(parts.items[--parts.count]);
        parts.items[parts.count] = NULL;
      } else if (!isAbs) {
        stringListAdd(&parts, "..");
      }
      continue;
    }
    stringListAddWithLength(&parts, begin, length);
  }

  size_t total = 0;
  if (isAbs) {
    total += drive ? 3 : 1;
  }
  for (int i = 0; i < parts.count; i++) {
    total += strlen(parts.items[i]);
    if (i + 1 < parts.count) total += 1;
  }
  if (total == 0) {
    if (isAbs) {
      stringListFree(&parts);
      if (drive) {
        char rootBuf[4] = { drive, ':', sep, '\0' };
        return OBJ_VAL(copyString(vm, rootBuf));
      }
      char rootBuf[2] = { sep, '\0' };
      return OBJ_VAL(copyString(vm, rootBuf));
    }
    stringListFree(&parts);
    return OBJ_VAL(copyString(vm, "."));
  }

  char* buffer = (char*)malloc(total + 1);
  if (!buffer) {
    stringListFree(&parts);
    return runtimeErrorValue(vm, "path.normalize out of memory.");
  }
  size_t offset = 0;
  if (isAbs) {
    if (drive) {
      buffer[offset++] = drive;
      buffer[offset++] = ':';
      buffer[offset++] = sep;
    } else {
      buffer[offset++] = sep;
    }
  }
  for (int i = 0; i < parts.count; i++) {
    size_t length = strlen(parts.items[i]);
    memcpy(buffer + offset, parts.items[i], length);
    offset += length;
    if (i + 1 < parts.count) {
      buffer[offset++] = sep;
    }
  }
  buffer[offset] = '\0';

  ObjString* result = copyStringWithLength(vm, buffer, (int)offset);
  free(buffer);
  stringListFree(&parts);
  return OBJ_VAL(result);
}

static Value nativePathSplit(VM* vm, int argc, Value* args) {
  (void)argc;
  if (!isObjType(args[0], OBJ_STRING)) {
    return runtimeErrorValue(vm, "path.split expects a path string.");
  }
  ObjString* path = (ObjString*)AS_OBJ(args[0]);
  const char* sep = findLastSeparator(path->chars);
  const char* base = sep ? sep + 1 : path->chars;
  const char* dot = strrchr(base, '.');
  ObjMap* map = newMap(vm);

  ObjString* keyDir = copyString(vm, "dir");
  ObjString* keyBase = copyString(vm, "base");
  ObjString* keyName = copyString(vm, "name");
  ObjString* keyExt = copyString(vm, "ext");

  Value dirValue;
  if (!sep) {
    dirValue = OBJ_VAL(copyString(vm, "."));
  } else {
    size_t length = (size_t)(sep - path->chars);
    if (length == 0) length = 1;
    dirValue = OBJ_VAL(copyStringWithLength(vm, path->chars, (int)length));
  }

  Value baseValue = OBJ_VAL(copyString(vm, base));
  Value nameValue;
  Value extValue;
  if (!dot || dot == base) {
    nameValue = OBJ_VAL(copyString(vm, base));
    extValue = OBJ_VAL(copyString(vm, ""));
  } else {
    nameValue = OBJ_VAL(copyStringWithLength(vm, base, (int)(dot - base)));
    extValue = OBJ_VAL(copyStringWithLength(vm, dot, (int)strlen(dot)));
  }

  mapSet(map, keyDir, dirValue);
  mapSet(map, keyBase, baseValue);
  mapSet(map, keyName, nameValue);
  mapSet(map, keyExt, extValue);
  return OBJ_VAL(map);
}

static Value nativeRandomSeed(VM* vm, int argc, Value* args) {
  (void)argc;
  if (!IS_NUMBER(args[0])) {
    return runtimeErrorValue(vm, "random.seed expects a number.");
  }
  int64_t seed = (int64_t)AS_NUMBER(args[0]);
  if (seed == 0) {
    seed = (int64_t)0x9e3779b97f4a7c15ULL;
  }
  gRandomState = (uint64_t)seed;
  gRandomSeeded = true;
  gRandomHasSpare = false;
  return NULL_VAL;
}

static Value nativeRandomInt(VM* vm, int argc, Value* args) {
  if (argc < 1) {
    return runtimeErrorValue(vm, "random.int expects (max) or (min, max).");
  }
  if (!IS_NUMBER(args[0])) {
    return runtimeErrorValue(vm, "random.int expects numeric bounds.");
  }

  if (argc == 1) {
    int max = (int)AS_NUMBER(args[0]);
    if (max <= 0) {
      return runtimeErrorValue(vm, "random.int expects max > 0.");
    }
    uint64_t value = randomNext();
    return NUMBER_VAL((double)(value % (uint64_t)max));
  }

  if (!IS_NUMBER(args[1])) {
    return runtimeErrorValue(vm, "random.int expects numeric bounds.");
  }
  int min = (int)AS_NUMBER(args[0]);
  int max = (int)AS_NUMBER(args[1]);
  if (max <= min) {
    return runtimeErrorValue(vm, "random.int expects max > min.");
  }
  uint64_t span = (uint64_t)(max - min);
  uint64_t value = randomNext() % span;
  return NUMBER_VAL((double)(min + (int)value));
}

static Value nativeRandomFloat(VM* vm, int argc, Value* args) {
  if (argc == 0) {
    return NUMBER_VAL(randomNextDouble());
  }
  if (!IS_NUMBER(args[0])) {
    return runtimeErrorValue(vm, "random.float expects numeric bounds.");
  }
  double min = 0.0;
  double max = AS_NUMBER(args[0]);
  if (argc >= 2) {
    if (!IS_NUMBER(args[1])) {
      return runtimeErrorValue(vm, "random.float expects numeric bounds.");
    }
    min = AS_NUMBER(args[0]);
    max = AS_NUMBER(args[1]);
  }
  if (max <= min) {
    return runtimeErrorValue(vm, "random.float expects max > min.");
  }
  double unit = randomNextDouble();
  return NUMBER_VAL(min + unit * (max - min));
}

static Value nativeRandomChoice(VM* vm, int argc, Value* args) {
  (void)argc;
  if (!isObjType(args[0], OBJ_ARRAY)) {
    return runtimeErrorValue(vm, "random.choice expects an array.");
  }
  ObjArray* array = (ObjArray*)AS_OBJ(args[0]);
  if (array->count <= 0) {
    return runtimeErrorValue(vm, "random.choice expects a non-empty array.");
  }
  uint64_t index = randomNext() % (uint64_t)array->count;
  return array->items[(int)index];
}

static Value nativeRandomNormal(VM* vm, int argc, Value* args) {
  if (argc < 2) {
    return runtimeErrorValue(vm, "random.normal expects (mean, stddev).");
  }
  if (!IS_NUMBER(args[0]) || !IS_NUMBER(args[1])) {
    return runtimeErrorValue(vm, "random.normal expects numeric bounds.");
  }
  double mean = AS_NUMBER(args[0]);
  double stddev = AS_NUMBER(args[1]);
  if (stddev < 0.0) {
    return runtimeErrorValue(vm, "random.normal expects stddev >= 0.");
  }
  return NUMBER_VAL(mean + randomNextNormal() * stddev);
}

static Value nativeRandomGaussian(VM* vm, int argc, Value* args) {
  return nativeRandomNormal(vm, argc, args);
}

static Value nativeRandomExponential(VM* vm, int argc, Value* args) {
  if (argc < 1) {
    return runtimeErrorValue(vm, "random.exponential expects (lambda).");
  }
  if (!IS_NUMBER(args[0])) {
    return runtimeErrorValue(vm, "random.exponential expects a number.");
  }
  double lambda = AS_NUMBER(args[0]);
  if (lambda <= 0.0) {
    return runtimeErrorValue(vm, "random.exponential expects lambda > 0.");
  }
  double u = randomNextDouble();
  if (u <= 0.0) {
    u = 1e-12;
  }
  return NUMBER_VAL(-log(1.0 - u) / lambda);
}

static Value nativeRandomUniform(VM* vm, int argc, Value* args) {
  return nativeRandomFloat(vm, argc, args);
}

static Value nativeStrUpper(VM* vm, int argc, Value* args) {
  (void)argc;
  if (!isObjType(args[0], OBJ_STRING)) {
    return runtimeErrorValue(vm, "str.upper expects a string.");
  }
  ObjString* input = (ObjString*)AS_OBJ(args[0]);
  char* buffer = (char*)malloc((size_t)input->length + 1);
  if (!buffer) {
    return runtimeErrorValue(vm, "str.upper out of memory.");
  }
  for (int i = 0; i < input->length; i++) {
    buffer[i] = (char)toupper((unsigned char)input->chars[i]);
  }
  buffer[input->length] = '\0';
  return OBJ_VAL(takeStringWithLength(vm, buffer, input->length));
}

static Value nativeStrLower(VM* vm, int argc, Value* args) {
  (void)argc;
  if (!isObjType(args[0], OBJ_STRING)) {
    return runtimeErrorValue(vm, "str.lower expects a string.");
  }
  ObjString* input = (ObjString*)AS_OBJ(args[0]);
  char* buffer = (char*)malloc((size_t)input->length + 1);
  if (!buffer) {
    return runtimeErrorValue(vm, "str.lower out of memory.");
  }
  for (int i = 0; i < input->length; i++) {
    buffer[i] = (char)tolower((unsigned char)input->chars[i]);
  }
  buffer[input->length] = '\0';
  return OBJ_VAL(takeStringWithLength(vm, buffer, input->length));
}

static Value nativeStrTrim(VM* vm, int argc, Value* args) {
  (void)argc;
  if (!isObjType(args[0], OBJ_STRING)) {
    return runtimeErrorValue(vm, "str.trim expects a string.");
  }
  ObjString* input = (ObjString*)AS_OBJ(args[0]);
  int start = 0;
  int end = input->length;
  while (start < end && isspace((unsigned char)input->chars[start])) {
    start++;
  }
  while (end > start && isspace((unsigned char)input->chars[end - 1])) {
    end--;
  }
  return OBJ_VAL(copyStringWithLength(vm, input->chars + start, end - start));
}

static Value nativeStrTrimStart(VM* vm, int argc, Value* args) {
  (void)argc;
  if (!isObjType(args[0], OBJ_STRING)) {
    return runtimeErrorValue(vm, "str.trimStart expects a string.");
  }
  ObjString* input = (ObjString*)AS_OBJ(args[0]);
  int start = 0;
  while (start < input->length && isspace((unsigned char)input->chars[start])) {
    start++;
  }
  return OBJ_VAL(copyStringWithLength(vm, input->chars + start, input->length - start));
}

static Value nativeStrTrimEnd(VM* vm, int argc, Value* args) {
  (void)argc;
  if (!isObjType(args[0], OBJ_STRING)) {
    return runtimeErrorValue(vm, "str.trimEnd expects a string.");
  }
  ObjString* input = (ObjString*)AS_OBJ(args[0]);
  int end = input->length;
  while (end > 0 && isspace((unsigned char)input->chars[end - 1])) {
    end--;
  }
  return OBJ_VAL(copyStringWithLength(vm, input->chars, end));
}

static Value nativeStrStartsWith(VM* vm, int argc, Value* args) {
  (void)argc;
  if (!isObjType(args[0], OBJ_STRING) || !isObjType(args[1], OBJ_STRING)) {
    return runtimeErrorValue(vm, "str.startsWith expects (text, prefix) strings.");
  }
  ObjString* text = (ObjString*)AS_OBJ(args[0]);
  ObjString* prefix = (ObjString*)AS_OBJ(args[1]);
  if (prefix->length > text->length) return BOOL_VAL(false);
  return BOOL_VAL(memcmp(text->chars, prefix->chars, (size_t)prefix->length) == 0);
}

static Value nativeStrEndsWith(VM* vm, int argc, Value* args) {
  (void)argc;
  if (!isObjType(args[0], OBJ_STRING) || !isObjType(args[1], OBJ_STRING)) {
    return runtimeErrorValue(vm, "str.endsWith expects (text, suffix) strings.");
  }
  ObjString* text = (ObjString*)AS_OBJ(args[0]);
  ObjString* suffix = (ObjString*)AS_OBJ(args[1]);
  if (suffix->length > text->length) return BOOL_VAL(false);
  const char* start = text->chars + (text->length - suffix->length);
  return BOOL_VAL(memcmp(start, suffix->chars, (size_t)suffix->length) == 0);
}

static Value nativeStrContains(VM* vm, int argc, Value* args) {
  (void)argc;
  if (!isObjType(args[0], OBJ_STRING) || !isObjType(args[1], OBJ_STRING)) {
    return runtimeErrorValue(vm, "str.contains expects (text, needle) strings.");
  }
  ObjString* text = (ObjString*)AS_OBJ(args[0]);
  ObjString* needle = (ObjString*)AS_OBJ(args[1]);
  if (needle->length == 0) return BOOL_VAL(true);
  return BOOL_VAL(strstr(text->chars, needle->chars) != NULL);
}

static Value nativeStrSplit(VM* vm, int argc, Value* args) {
  (void)argc;
  if (!isObjType(args[0], OBJ_STRING) || !isObjType(args[1], OBJ_STRING)) {
    return runtimeErrorValue(vm, "str.split expects (text, sep) strings.");
  }
  ObjString* text = (ObjString*)AS_OBJ(args[0]);
  ObjString* sep = (ObjString*)AS_OBJ(args[1]);

  ObjArray* array = newArray(vm);
  if (sep->length == 0) {
    for (int i = 0; i < text->length; i++) {
      char chunk[2];
      chunk[0] = text->chars[i];
      chunk[1] = '\0';
      arrayWrite(array, OBJ_VAL(copyStringWithLength(vm, chunk, 1)));
    }
    return OBJ_VAL(array);
  }

  const char* current = text->chars;
  const char* end = text->chars + text->length;
  while (current <= end) {
    const char* found = strstr(current, sep->chars);
    if (!found) {
      int length = (int)(end - current);
      arrayWrite(array, OBJ_VAL(copyStringWithLength(vm, current, length)));
      break;
    }
    int length = (int)(found - current);
    arrayWrite(array, OBJ_VAL(copyStringWithLength(vm, current, length)));
    current = found + sep->length;
  }

  return OBJ_VAL(array);
}

static Value nativeStrBuilder(VM* vm, int argc, Value* args) {
  (void)argc;
  (void)args;
  return OBJ_VAL(newArray(vm));
}

static Value nativeStrAppend(VM* vm, int argc, Value* args) {
  (void)argc;
  if (!isObjType(args[0], OBJ_ARRAY) || !isObjType(args[1], OBJ_STRING)) {
    return runtimeErrorValue(vm, "str.append expects (builder, text).");
  }
  ObjArray* array = (ObjArray*)AS_OBJ(args[0]);
  arrayWrite(array, args[1]);
  return args[0];
}

static Value nativeStrBuild(VM* vm, int argc, Value* args) {
  if (argc < 1) {
    return runtimeErrorValue(vm, "str.build expects (builder, sep?).");
  }
  if (!isObjType(args[0], OBJ_ARRAY)) {
    return runtimeErrorValue(vm, "str.build expects (builder, sep?).");
  }
  ObjArray* array = (ObjArray*)AS_OBJ(args[0]);

  const char* sepChars = "";
  int sepLength = 0;
  if (argc >= 2) {
    if (!isObjType(args[1], OBJ_STRING)) {
      return runtimeErrorValue(vm, "str.build expects (builder, sep?).");
    }
    ObjString* sep = (ObjString*)AS_OBJ(args[1]);
    sepChars = sep->chars;
    sepLength = sep->length;
  }

  ByteBuffer buffer;
  bufferInit(&buffer);

  for (int i = 0; i < array->count; i++) {
    if (!isObjType(array->items[i], OBJ_STRING)) {
      bufferFree(&buffer);
      return runtimeErrorValue(vm, "str.build expects an array of strings.");
    }
    ObjString* item = (ObjString*)AS_OBJ(array->items[i]);
    if (i > 0 && sepLength > 0) {
      bufferAppendN(&buffer, sepChars, (size_t)sepLength);
    }
    if (item->length > 0) {
      bufferAppendN(&buffer, item->chars, (size_t)item->length);
    }
  }

  ObjString* result = copyStringWithLength(vm, buffer.data ? buffer.data : "",
                                           (int)buffer.length);
  bufferFree(&buffer);
  return OBJ_VAL(result);
}

static Value nativeStrJoin(VM* vm, int argc, Value* args) {
  (void)argc;
  if (!isObjType(args[0], OBJ_ARRAY) || !isObjType(args[1], OBJ_STRING)) {
    return runtimeErrorValue(vm, "str.join expects (array, sep).");
  }
  ObjArray* array = (ObjArray*)AS_OBJ(args[0]);
  ObjString* sep = (ObjString*)AS_OBJ(args[1]);

  ByteBuffer buffer;
  bufferInit(&buffer);

  for (int i = 0; i < array->count; i++) {
    if (!isObjType(array->items[i], OBJ_STRING)) {
      bufferFree(&buffer);
      return runtimeErrorValue(vm, "str.join expects an array of strings.");
    }
    ObjString* item = (ObjString*)AS_OBJ(array->items[i]);
    if (i > 0 && sep->length > 0) {
      bufferAppendN(&buffer, sep->chars, (size_t)sep->length);
    }
    if (item->length > 0) {
      bufferAppendN(&buffer, item->chars, (size_t)item->length);
    }
  }

  ObjString* result = copyStringWithLength(vm, buffer.data ? buffer.data : "",
                                           (int)buffer.length);
  bufferFree(&buffer);
  return OBJ_VAL(result);
}

static Value nativeStrReplace(VM* vm, int argc, Value* args) {
  (void)argc;
  if (!isObjType(args[0], OBJ_STRING) || !isObjType(args[1], OBJ_STRING) ||
      !isObjType(args[2], OBJ_STRING)) {
    return runtimeErrorValue(vm, "str.replace expects (text, needle, replacement).");
  }
  ObjString* text = (ObjString*)AS_OBJ(args[0]);
  ObjString* needle = (ObjString*)AS_OBJ(args[1]);
  ObjString* repl = (ObjString*)AS_OBJ(args[2]);

  if (needle->length == 0) {
    return OBJ_VAL(text);
  }

  const char* found = strstr(text->chars, needle->chars);
  if (!found) {
    return OBJ_VAL(text);
  }

  ByteBuffer buffer;
  bufferInit(&buffer);
  bufferAppendN(&buffer, text->chars, (size_t)(found - text->chars));
  if (repl->length > 0) {
    bufferAppendN(&buffer, repl->chars, (size_t)repl->length);
  }
  const char* tail = found + needle->length;
  bufferAppendN(&buffer, tail, (size_t)(text->chars + text->length - tail));

  ObjString* result = copyStringWithLength(vm, buffer.data ? buffer.data : "",
                                           (int)buffer.length);
  bufferFree(&buffer);
  return OBJ_VAL(result);
}

static Value nativeStrReplaceAll(VM* vm, int argc, Value* args) {
  (void)argc;
  if (!isObjType(args[0], OBJ_STRING) || !isObjType(args[1], OBJ_STRING) ||
      !isObjType(args[2], OBJ_STRING)) {
    return runtimeErrorValue(vm, "str.replaceAll expects (text, needle, replacement).");
  }
  ObjString* text = (ObjString*)AS_OBJ(args[0]);
  ObjString* needle = (ObjString*)AS_OBJ(args[1]);
  ObjString* repl = (ObjString*)AS_OBJ(args[2]);

  if (needle->length == 0) {
    return OBJ_VAL(text);
  }

  const char* cursor = text->chars;
  const char* found = strstr(cursor, needle->chars);
  if (!found) {
    return OBJ_VAL(text);
  }

  ByteBuffer buffer;
  bufferInit(&buffer);
  while (found) {
    bufferAppendN(&buffer, cursor, (size_t)(found - cursor));
    if (repl->length > 0) {
      bufferAppendN(&buffer, repl->chars, (size_t)repl->length);
    }
    cursor = found + needle->length;
    found = strstr(cursor, needle->chars);
  }
  bufferAppendN(&buffer, cursor, (size_t)(text->chars + text->length - cursor));

  ObjString* result = copyStringWithLength(vm, buffer.data ? buffer.data : "",
                                           (int)buffer.length);
  bufferFree(&buffer);
  return OBJ_VAL(result);
}

static Value nativeStrRepeat(VM* vm, int argc, Value* args) {
  (void)argc;
  if (!isObjType(args[0], OBJ_STRING) || !IS_NUMBER(args[1])) {
    return runtimeErrorValue(vm, "str.repeat expects (text, count).");
  }
  ObjString* text = (ObjString*)AS_OBJ(args[0]);
  int count = (int)AS_NUMBER(args[1]);
  if (count < 0) {
    return runtimeErrorValue(vm, "str.repeat expects a non-negative count.");
  }
  if (count == 0 || text->length == 0) {
    return OBJ_VAL(copyString(vm, ""));
  }
  if (text->length > 0 && count > INT_MAX / text->length) {
    return runtimeErrorValue(vm, "str.repeat result too large.");
  }
  int total = text->length * count;
  char* buffer = (char*)malloc((size_t)total + 1);
  if (!buffer) {
    return runtimeErrorValue(vm, "str.repeat out of memory.");
  }
  char* cursor = buffer;
  for (int i = 0; i < count; i++) {
    memcpy(cursor, text->chars, (size_t)text->length);
    cursor += text->length;
  }
  buffer[total] = '\0';
  return OBJ_VAL(takeStringWithLength(vm, buffer, total));
}

static Value nativeArraySlice(VM* vm, int argc, Value* args) {
  if (!isObjType(args[0], OBJ_ARRAY)) {
    return runtimeErrorValue(vm, "array.slice expects an array.");
  }
  ObjArray* array = (ObjArray*)AS_OBJ(args[0]);
  int count = array->count;
  int start = 0;
  int end = count;
  if (argc >= 2) {
    if (!IS_NUMBER(args[1])) {
      return runtimeErrorValue(vm, "array.slice expects numeric indices.");
    }
    start = (int)AS_NUMBER(args[1]);
  }
  if (argc >= 3) {
    if (!IS_NUMBER(args[2])) {
      return runtimeErrorValue(vm, "array.slice expects numeric indices.");
    }
    end = (int)AS_NUMBER(args[2]);
  }
  if (start < 0) start = count + start;
  if (end < 0) end = count + end;
  if (start < 0) start = 0;
  if (end < 0) end = 0;
  if (start > count) start = count;
  if (end > count) end = count;
  if (end < start) end = start;

  ObjArray* result = newArrayWithCapacity(vm, end - start);
  for (int i = start; i < end; i++) {
    arrayWrite(result, array->items[i]);
  }
  return OBJ_VAL(result);
}

static Value nativeArrayMap(VM* vm, int argc, Value* args) {
  (void)argc;
  if (!isObjType(args[0], OBJ_ARRAY)) {
    return runtimeErrorValue(vm, "array.map expects (array, fn).");
  }
  ObjArray* array = (ObjArray*)AS_OBJ(args[0]);
  Value fn = args[1];
  ObjArray* result = newArrayWithCapacity(vm, array->count);
  for (int i = 0; i < array->count; i++) {
    Value arg = array->items[i];
    Value out;
    if (!vmCallValue(vm, fn, 1, &arg, &out)) {
      return NULL_VAL;
    }
    arrayWrite(result, out);
  }
  return OBJ_VAL(result);
}

static Value nativeArrayFilter(VM* vm, int argc, Value* args) {
  (void)argc;
  if (!isObjType(args[0], OBJ_ARRAY)) {
    return runtimeErrorValue(vm, "array.filter expects (array, fn).");
  }
  ObjArray* array = (ObjArray*)AS_OBJ(args[0]);
  Value fn = args[1];
  ObjArray* result = newArrayWithCapacity(vm, array->count);
  for (int i = 0; i < array->count; i++) {
    Value arg = array->items[i];
    Value out;
    if (!vmCallValue(vm, fn, 1, &arg, &out)) {
      return NULL_VAL;
    }
    if (isTruthy(out)) {
      arrayWrite(result, arg);
    }
  }
  return OBJ_VAL(result);
}

static Value nativeArrayReduce(VM* vm, int argc, Value* args) {
  if (argc < 2) {
    return runtimeErrorValue(vm, "array.reduce expects (array, fn, initial?).");
  }
  if (!isObjType(args[0], OBJ_ARRAY)) {
    return runtimeErrorValue(vm, "array.reduce expects (array, fn, initial?).");
  }
  ObjArray* array = (ObjArray*)AS_OBJ(args[0]);
  Value fn = args[1];
  int index = 0;
  Value acc = NULL_VAL;
  if (argc >= 3) {
    acc = args[2];
  } else {
    if (array->count == 0) {
      return runtimeErrorValue(vm, "array.reduce expects an initial value for empty arrays.");
    }
    acc = array->items[0];
    index = 1;
  }

  for (int i = index; i < array->count; i++) {
    Value callArgs[2] = {acc, array->items[i]};
    Value out;
    if (!vmCallValue(vm, fn, 2, callArgs, &out)) {
      return NULL_VAL;
    }
    acc = out;
  }

  return acc;
}

static Value nativeArrayContains(VM* vm, int argc, Value* args) {
  (void)argc;
  if (!isObjType(args[0], OBJ_ARRAY)) {
    return runtimeErrorValue(vm, "array.contains expects (array, value).");
  }
  ObjArray* array = (ObjArray*)AS_OBJ(args[0]);
  for (int i = 0; i < array->count; i++) {
    if (valuesEqual(array->items[i], args[1])) {
      return BOOL_VAL(true);
    }
  }
  return BOOL_VAL(false);
}

static Value nativeArrayIndexOf(VM* vm, int argc, Value* args) {
  (void)argc;
  if (!isObjType(args[0], OBJ_ARRAY)) {
    return runtimeErrorValue(vm, "array.indexOf expects (array, value).");
  }
  ObjArray* array = (ObjArray*)AS_OBJ(args[0]);
  for (int i = 0; i < array->count; i++) {
    if (valuesEqual(array->items[i], args[1])) {
      return NUMBER_VAL((double)i);
    }
  }
  return NUMBER_VAL(-1);
}

static Value nativeArrayConcat(VM* vm, int argc, Value* args) {
  (void)argc;
  if (!isObjType(args[0], OBJ_ARRAY) || !isObjType(args[1], OBJ_ARRAY)) {
    return runtimeErrorValue(vm, "array.concat expects (left, right) arrays.");
  }
  ObjArray* left = (ObjArray*)AS_OBJ(args[0]);
  ObjArray* right = (ObjArray*)AS_OBJ(args[1]);
  ObjArray* result = newArrayWithCapacity(vm, left->count + right->count);
  for (int i = 0; i < left->count; i++) {
    arrayWrite(result, left->items[i]);
  }
  for (int i = 0; i < right->count; i++) {
    arrayWrite(result, right->items[i]);
  }
  return OBJ_VAL(result);
}

static Value nativeArrayReverse(VM* vm, int argc, Value* args) {
  (void)argc;
  if (!isObjType(args[0], OBJ_ARRAY)) {
    return runtimeErrorValue(vm, "array.reverse expects an array.");
  }
  ObjArray* array = (ObjArray*)AS_OBJ(args[0]);
  ObjArray* result = newArrayWithCapacity(vm, array->count);
  for (int i = array->count - 1; i >= 0; i--) {
    arrayWrite(result, array->items[i]);
  }
  return OBJ_VAL(result);
}

static Value nativeOsPlatform(VM* vm, int argc, Value* args) {
  (void)argc; (void)args;
#ifdef _WIN32
  return OBJ_VAL(copyString(vm, "windows"));
#elif __APPLE__
  return OBJ_VAL(copyString(vm, "mac"));
#elif __linux__
  return OBJ_VAL(copyString(vm, "linux"));
#else
  return OBJ_VAL(copyString(vm, "unknown"));
#endif
}

static Value nativeOsArch(VM* vm, int argc, Value* args) {
  (void)argc; (void)args;
#if defined(_M_X64) || defined(__x86_64__) || defined(__amd64__)
  return OBJ_VAL(copyString(vm, "x64"));
#elif defined(_M_IX86) || defined(__i386__)
  return OBJ_VAL(copyString(vm, "x86"));
#elif defined(_M_ARM64) || defined(__aarch64__)
  return OBJ_VAL(copyString(vm, "arm64"));
#elif defined(_M_ARM) || defined(__arm__)
  return OBJ_VAL(copyString(vm, "arm"));
#else
  return OBJ_VAL(copyString(vm, "unknown"));
#endif
}

static Value nativeOsSep(VM* vm, int argc, Value* args) {
  (void)argc; (void)args;
#ifdef _WIN32
  return OBJ_VAL(copyString(vm, "\\"));
#else
  return OBJ_VAL(copyString(vm, "/"));
#endif
}

static Value nativeOsEol(VM* vm, int argc, Value* args) {
  (void)argc; (void)args;
#ifdef _WIN32
  return OBJ_VAL(copyString(vm, "\r\n"));
#else
  return OBJ_VAL(copyString(vm, "\n"));
#endif
}

static Value nativeOsCwd(VM* vm, int argc, Value* args) {
  return nativeFsCwd(vm, argc, args);
}

static Value nativeOsHome(VM* vm, int argc, Value* args) {
  (void)argc; (void)args;
#ifdef _WIN32
  const char* home = getenv("USERPROFILE");
  if (home && *home) {
    return OBJ_VAL(copyString(vm, home));
  }
  const char* drive = getenv("HOMEDRIVE");
  const char* path = getenv("HOMEPATH");
  if (drive && path) {
    size_t total = strlen(drive) + strlen(path);
    char* buffer = (char*)malloc(total + 1);
    if (!buffer) {
      return runtimeErrorValue(vm, "os.home out of memory.");
    }
    snprintf(buffer, total + 1, "%s%s", drive, path);
    ObjString* result = copyStringWithLength(vm, buffer, (int)total);
    free(buffer);
    return OBJ_VAL(result);
  }
  return NULL_VAL;
#else
  const char* home = getenv("HOME");
  if (!home || !*home) return NULL_VAL;
  return OBJ_VAL(copyString(vm, home));
#endif
}

static Value nativeOsTmp(VM* vm, int argc, Value* args) {
  (void)argc; (void)args;
#ifdef _WIN32
  DWORD length = GetTempPathA(0, NULL);
  if (length == 0) {
    return runtimeErrorValue(vm, "os.tmp failed to read temp path.");
  }
  char* buffer = (char*)malloc((size_t)length + 1);
  if (!buffer) {
    return runtimeErrorValue(vm, "os.tmp out of memory.");
  }
  DWORD written = GetTempPathA(length + 1, buffer);
  if (written == 0) {
    free(buffer);
    return runtimeErrorValue(vm, "os.tmp failed to read temp path.");
  }
  ObjString* result = copyString(vm, buffer);
  free(buffer);
  return OBJ_VAL(result);
#else
  const char* tmp = getenv("TMPDIR");
  if (!tmp || !*tmp) tmp = getenv("TMP");
  if (!tmp || !*tmp) tmp = getenv("TEMP");
  if (!tmp || !*tmp) tmp = "/tmp";
  return OBJ_VAL(copyString(vm, tmp));
#endif
}

static Value nativeTimeNow(VM* vm, int argc, Value* args) {
  (void)argc;
  (void)args;
  time_t now = time(NULL);
  if (now == (time_t)-1) {
    return runtimeErrorValue(vm, "time.now failed.");
  }
  return NUMBER_VAL((double)now);
}

static Value nativeTimeSleep(VM* vm, int argc, Value* args) {
  (void)argc;
  if (!IS_NUMBER(args[0])) {
    return runtimeErrorValue(vm, "time.sleep expects seconds as a number.");
  }
  double seconds = AS_NUMBER(args[0]);
  if (seconds < 0) {
    return runtimeErrorValue(vm, "time.sleep expects a non-negative number.");
  }
#ifdef _WIN32
  DWORD ms = (DWORD)(seconds * 1000.0);
  Sleep(ms);
#else
  struct timespec ts;
  ts.tv_sec = (time_t)seconds;
  ts.tv_nsec = (long)((seconds - (double)ts.tv_sec) * 1000000000.0);
  if (ts.tv_nsec < 0) ts.tv_nsec = 0;
  if (nanosleep(&ts, NULL) != 0) {
    return runtimeErrorValue(vm, "time.sleep failed.");
  }
#endif
}

static bool timeGetTm(double seconds, bool utc, struct tm* out) {
  time_t raw = (time_t)seconds;
#ifdef _WIN32
  int err = utc ? gmtime_s(out, &raw) : localtime_s(out, &raw);
  return err == 0;
#else
  struct tm* result = utc ? gmtime_r(&raw, out) : localtime_r(&raw, out);
  return result != NULL;
#endif
}

static bool valueIsTruthy(Value value) {
  if (IS_NULL(value)) return false;
  if (IS_BOOL(value)) return AS_BOOL(value);
  if (IS_NUMBER(value)) return AS_NUMBER(value) != 0;
  return true;
}

static Value nativeTimeFormat(VM* vm, int argc, Value* args) {
  if (argc < 2) {
    return runtimeErrorValue(vm, "time.format expects (timestamp, format, utc?).");
  }
  if (!IS_NUMBER(args[0]) || !isObjType(args[1], OBJ_STRING)) {
    return runtimeErrorValue(vm, "time.format expects (timestamp, format, utc?).");
  }
  bool utc = false;
  if (argc >= 3) {
    utc = valueIsTruthy(args[2]);
  }
  ObjString* format = (ObjString*)AS_OBJ(args[1]);
  struct tm tmValue;
  if (!timeGetTm(AS_NUMBER(args[0]), utc, &tmValue)) {
    return runtimeErrorValue(vm, "time.format failed.");
  }
  char buffer[256];
  size_t written = strftime(buffer, sizeof(buffer), format->chars, &tmValue);
  if (written == 0) {
    return runtimeErrorValue(vm, "time.format failed to format.");
  }
  return OBJ_VAL(copyStringWithLength(vm, buffer, (int)written));
}

static Value nativeTimeIso(VM* vm, int argc, Value* args) {
  if (argc < 1 || !IS_NUMBER(args[0])) {
    return runtimeErrorValue(vm, "time.iso expects (timestamp, utc?).");
  }
  bool utc = false;
  if (argc >= 2) {
    utc = valueIsTruthy(args[1]);
  }
  struct tm tmValue;
  if (!timeGetTm(AS_NUMBER(args[0]), utc, &tmValue)) {
    return runtimeErrorValue(vm, "time.iso failed.");
  }
  char buffer[32];
  size_t written = strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%S", &tmValue);
  if (written == 0) {
    return runtimeErrorValue(vm, "time.iso failed to format.");
  }
  if (utc && written + 1 < sizeof(buffer)) {
    buffer[written++] = 'Z';
    buffer[written] = '\0';
  }
  return OBJ_VAL(copyStringWithLength(vm, buffer, (int)written));
}

static Value nativeTimeParts(VM* vm, int argc, Value* args) {
  if (argc < 1 || !IS_NUMBER(args[0])) {
    return runtimeErrorValue(vm, "time.parts expects (timestamp, utc?).");
  }
  bool utc = false;
  if (argc >= 2) {
    utc = valueIsTruthy(args[1]);
  }
  struct tm tmValue;
  if (!timeGetTm(AS_NUMBER(args[0]), utc, &tmValue)) {
    return runtimeErrorValue(vm, "time.parts failed.");
  }
  ObjMap* map = newMap(vm);
  mapSet(map, copyString(vm, "year"), NUMBER_VAL((double)(tmValue.tm_year + 1900)));
  mapSet(map, copyString(vm, "month"), NUMBER_VAL((double)(tmValue.tm_mon + 1)));
  mapSet(map, copyString(vm, "day"), NUMBER_VAL((double)tmValue.tm_mday));
  mapSet(map, copyString(vm, "hour"), NUMBER_VAL((double)tmValue.tm_hour));
  mapSet(map, copyString(vm, "min"), NUMBER_VAL((double)tmValue.tm_min));
  mapSet(map, copyString(vm, "sec"), NUMBER_VAL((double)tmValue.tm_sec));
  mapSet(map, copyString(vm, "wday"), NUMBER_VAL((double)tmValue.tm_wday));
  mapSet(map, copyString(vm, "yday"), NUMBER_VAL((double)tmValue.tm_yday));
  mapSet(map, copyString(vm, "isdst"), BOOL_VAL(tmValue.tm_isdst > 0));
  return OBJ_VAL(map);
}

static Value nativeProcRun(VM* vm, int argc, Value* args) {
  (void)argc;
  if (!isObjType(args[0], OBJ_STRING)) {
    return runtimeErrorValue(vm, "proc.run expects a command string.");
  }
  ObjString* cmd = (ObjString*)AS_OBJ(args[0]);
  int result = system(cmd->chars);
  return NUMBER_VAL((double)result);
}

static Value nativeEnvGet(VM* vm, int argc, Value* args) {
  (void)argc;
  if (!isObjType(args[0], OBJ_STRING)) {
    return runtimeErrorValue(vm, "env.get expects a name string.");
  }
  ObjString* name = (ObjString*)AS_OBJ(args[0]);
#ifdef _WIN32
  DWORD length = GetEnvironmentVariableA(name->chars, NULL, 0);
  if (length == 0) {
    DWORD err = GetLastError();
    if (err == ERROR_ENVVAR_NOT_FOUND) {
      return NULL_VAL;
    }
    if (err == ERROR_SUCCESS) {
      return OBJ_VAL(copyString(vm, ""));
    }
    return runtimeErrorValue(vm, "env.get failed.");
  }
  char* buffer = (char*)malloc((size_t)length);
  if (!buffer) {
    return runtimeErrorValue(vm, "env.get out of memory.");
  }
  DWORD written = GetEnvironmentVariableA(name->chars, buffer, length);
  if (written == 0 && GetLastError() == ERROR_ENVVAR_NOT_FOUND) {
    free(buffer);
    return NULL_VAL;
  }
  ObjString* result = copyStringWithLength(vm, buffer, (int)written);
  free(buffer);
  return OBJ_VAL(result);
#else
  const char* value = getenv(name->chars);
  if (!value) return NULL_VAL;
  return OBJ_VAL(copyString(vm, value));
#endif
}

static Value nativeEnvSet(VM* vm, int argc, Value* args) {
  (void)argc;
  if (!isObjType(args[0], OBJ_STRING) || !isObjType(args[1], OBJ_STRING)) {
    return runtimeErrorValue(vm, "env.set expects (name, value) strings.");
  }
  ObjString* name = (ObjString*)AS_OBJ(args[0]);
  ObjString* value = (ObjString*)AS_OBJ(args[1]);
#ifdef _WIN32
  if (!SetEnvironmentVariableA(name->chars, value->chars)) {
    return runtimeErrorValue(vm, "env.set failed.");
  }
#else
  if (setenv(name->chars, value->chars, 1) != 0) {
    return runtimeErrorValue(vm, "env.set failed.");
  }
#endif
  return BOOL_VAL(true);
}

static Value nativeEnvHas(VM* vm, int argc, Value* args) {
  (void)argc;
  if (!isObjType(args[0], OBJ_STRING)) {
    return runtimeErrorValue(vm, "env.has expects a name string.");
  }
  ObjString* name = (ObjString*)AS_OBJ(args[0]);
#ifdef _WIN32
  DWORD length = GetEnvironmentVariableA(name->chars, NULL, 0);
  if (length == 0) {
    DWORD err = GetLastError();
    if (err == ERROR_ENVVAR_NOT_FOUND) return BOOL_VAL(false);
    if (err != ERROR_SUCCESS) return BOOL_VAL(false);
    return BOOL_VAL(true);
  }
  return BOOL_VAL(true);
#else
  return BOOL_VAL(getenv(name->chars) != NULL);
#endif
}

static Value nativeEnvUnset(VM* vm, int argc, Value* args) {
  (void)argc;
  if (!isObjType(args[0], OBJ_STRING)) {
    return runtimeErrorValue(vm, "env.unset expects a name string.");
  }
  ObjString* name = (ObjString*)AS_OBJ(args[0]);
#ifdef _WIN32
  if (!SetEnvironmentVariableA(name->chars, NULL)) {
    return runtimeErrorValue(vm, "env.unset failed.");
  }
#else
  if (unsetenv(name->chars) != 0) {
    return runtimeErrorValue(vm, "env.unset failed.");
  }
#endif
  return BOOL_VAL(true);
}

static Value nativeEnvAll(VM* vm, int argc, Value* args) {
  (void)argc;
  (void)args;
  ObjMap* result = newMap(vm);
#ifdef _WIN32
  LPCH block = GetEnvironmentStringsA();
  if (!block) {
    return runtimeErrorValue(vm, "env.all failed to read environment.");
  }
  for (char* entry = block; *entry; entry += strlen(entry) + 1) {
    char* eq = strchr(entry, '=');
    if (!eq || eq == entry || entry[0] == '=') {
      continue;
    }
    int keyLen = (int)(eq - entry);
    ObjString* key = copyStringWithLength(vm, entry, keyLen);
    ObjString* val = copyString(vm, eq + 1);
    mapSet(result, key, OBJ_VAL(val));
  }
  FreeEnvironmentStringsA(block);
#else
  extern char** environ;
  if (!environ) return OBJ_VAL(result);
  for (char** env = environ; *env; env++) {
    char* entry = *env;
    char* eq = strchr(entry, '=');
    if (!eq || eq == entry) continue;
    int keyLen = (int)(eq - entry);
    ObjString* key = copyStringWithLength(vm, entry, keyLen);
    ObjString* val = copyString(vm, eq + 1);
    mapSet(result, key, OBJ_VAL(val));
  }
#endif
  return OBJ_VAL(result);
}

static Value nativeEnvArgs(VM* vm, int argc, Value* args) {
  (void)argc;
  (void)args;
  return OBJ_VAL(vm->args);
}

static ObjMap* diGetMapField(VM* vm, ObjMap* container, const char* field, const char* message) {
  ObjString* key = copyString(vm, field);
  Value value;
  if (!mapGet(container, key, &value) || !isObjType(value, OBJ_MAP)) {
    runtimeErrorValue(vm, message);
    return NULL;
  }
  return (ObjMap*)AS_OBJ(value);
}

static bool diIsCallable(Value value) {
  return isObjType(value, OBJ_NATIVE) ||
         isObjType(value, OBJ_FUNCTION) ||
         isObjType(value, OBJ_CLASS);
}

static Value nativeDiContainer(VM* vm, int argc, Value* args) {
  (void)argc;
  (void)args;
  ObjMap* container = newMap(vm);
  mapSet(container, copyString(vm, "providers"), OBJ_VAL(newMap(vm)));
  mapSet(container, copyString(vm, "instances"), OBJ_VAL(newMap(vm)));
  mapSet(container, copyString(vm, "singletons"), OBJ_VAL(newMap(vm)));
  return OBJ_VAL(container);
}

static Value nativeDiBind(VM* vm, int argc, Value* args) {
  (void)argc;
  ObjMap* container = expectMapArg(vm, args[0], "di.bind expects a container map.");
  ObjString* name = expectStringArg(vm, args[1], "di.bind expects a name string.");
  if (!container || !name) return NULL_VAL;
  ObjMap* providers = diGetMapField(vm, container, "providers", "di.bind expects a container.");
  ObjMap* singletons = diGetMapField(vm, container, "singletons", "di.bind expects a container.");
  ObjMap* instances = diGetMapField(vm, container, "instances", "di.bind expects a container.");
  if (!providers || !singletons || !instances) return NULL_VAL;
  mapSet(providers, name, args[2]);
  mapSet(singletons, name, BOOL_VAL(false));
  mapSet(instances, name, NULL_VAL);
  return NULL_VAL;
}

static Value nativeDiSingleton(VM* vm, int argc, Value* args) {
  (void)argc;
  ObjMap* container = expectMapArg(vm, args[0], "di.singleton expects a container map.");
  ObjString* name = expectStringArg(vm, args[1], "di.singleton expects a name string.");
  if (!container || !name) return NULL_VAL;
  ObjMap* providers = diGetMapField(vm, container, "providers", "di.singleton expects a container.");
  ObjMap* singletons = diGetMapField(vm, container, "singletons", "di.singleton expects a container.");
  ObjMap* instances = diGetMapField(vm, container, "instances", "di.singleton expects a container.");
  if (!providers || !singletons || !instances) return NULL_VAL;
  mapSet(providers, name, args[2]);
  mapSet(singletons, name, BOOL_VAL(true));
  mapSet(instances, name, NULL_VAL);
  return NULL_VAL;
}

static Value nativeDiValue(VM* vm, int argc, Value* args) {
  (void)argc;
  ObjMap* container = expectMapArg(vm, args[0], "di.value expects a container map.");
  ObjString* name = expectStringArg(vm, args[1], "di.value expects a name string.");
  if (!container || !name) return NULL_VAL;
  ObjMap* providers = diGetMapField(vm, container, "providers", "di.value expects a container.");
  ObjMap* singletons = diGetMapField(vm, container, "singletons", "di.value expects a container.");
  ObjMap* instances = diGetMapField(vm, container, "instances", "di.value expects a container.");
  if (!providers || !singletons || !instances) return NULL_VAL;
  mapSet(providers, name, args[2]);
  mapSet(singletons, name, BOOL_VAL(true));
  mapSet(instances, name, args[2]);
  return NULL_VAL;
}

static Value nativeDiResolve(VM* vm, int argc, Value* args) {
  (void)argc;
  ObjMap* container = expectMapArg(vm, args[0], "di.resolve expects a container map.");
  ObjString* name = expectStringArg(vm, args[1], "di.resolve expects a name string.");
  if (!container || !name) return NULL_VAL;
  ObjMap* providers = diGetMapField(vm, container, "providers", "di.resolve expects a container.");
  ObjMap* singletons = diGetMapField(vm, container, "singletons", "di.resolve expects a container.");
  ObjMap* instances = diGetMapField(vm, container, "instances", "di.resolve expects a container.");
  if (!providers || !singletons || !instances) return NULL_VAL;

  Value singletonFlag;
  bool isSingleton = false;
  if (mapGet(singletons, name, &singletonFlag) && IS_BOOL(singletonFlag)) {
    isSingleton = AS_BOOL(singletonFlag);
  }

  if (isSingleton) {
    Value cached;
    if (mapGet(instances, name, &cached) && !IS_NULL(cached)) {
      return cached;
    }
  }

  Value provider;
  if (!mapGet(providers, name, &provider)) {
    return runtimeErrorValue(vm, "di.resolve missing provider.");
  }

  Value instance = provider;
  if (diIsCallable(provider)) {
    if (!vmCallValue(vm, provider, 0, NULL, &instance)) {
      return NULL_VAL;
    }
  }

  if (isSingleton) {
    mapSet(instances, name, instance);
  }
  return instance;
}

static Value nativePluginLoad(VM* vm, int argc, Value* args) {
  (void)argc;
  if (!isObjType(args[0], OBJ_STRING)) {
    return runtimeErrorValue(vm, "plugin.load expects a path string.");
  }
  ObjString* path = (ObjString*)AS_OBJ(args[0]);
  char error[256];
  if (!pluginLoad(vm, path->chars, error, sizeof(error))) {
    return runtimeErrorValue(vm, error);
  }
  return BOOL_VAL(true);
}

void defineStdlib(VM* vm) {
  defineNative(vm, "print", nativePrint, -1);
  defineNative(vm, "clock", nativeClock, 0);
  defineNative(vm, "type", nativeType, 1);
  defineNative(vm, "len", nativeLen, 1);
  defineNative(vm, "args", nativeArgs, 0);
  defineNative(vm, "push", nativePush, 2);
  defineNative(vm, "keys", nativeKeys, 1);
  defineNative(vm, "values", nativeValues, 1);

  ObjInstance* fs = makeModule(vm, "fs");
  moduleAdd(vm, fs, "readText", nativeFsReadText, 1);
  moduleAdd(vm, fs, "writeText", nativeFsWriteText, 2);
  moduleAdd(vm, fs, "exists", nativeFsExists, 1);
  moduleAdd(vm, fs, "cwd", nativeFsCwd, 0);
  moduleAdd(vm, fs, "listDir", nativeFsListDir, 1);
  moduleAdd(vm, fs, "isFile", nativeFsIsFile, 1);
  moduleAdd(vm, fs, "isDir", nativeFsIsDir, 1);
  moduleAdd(vm, fs, "size", nativeFsSize, 1);
  moduleAdd(vm, fs, "glob", nativeFsGlob, 1);
  defineGlobal(vm, "fs", OBJ_VAL(fs));

  ObjInstance* path = makeModule(vm, "path");
  moduleAdd(vm, path, "join", nativePathJoin, 2);
  moduleAdd(vm, path, "dirname", nativePathDirname, 1);
  moduleAdd(vm, path, "basename", nativePathBasename, 1);
  moduleAdd(vm, path, "extname", nativePathExtname, 1);
  moduleAdd(vm, path, "isAbs", nativePathIsAbs, 1);
  moduleAdd(vm, path, "normalize", nativePathNormalize, 1);
  moduleAdd(vm, path, "stem", nativePathStem, 1);
  moduleAdd(vm, path, "split", nativePathSplit, 1);
  defineGlobal(vm, "path", OBJ_VAL(path));

  ObjInstance* json = makeModule(vm, "json");
  moduleAdd(vm, json, "parse", nativeJsonParse, 1);
  moduleAdd(vm, json, "stringify", nativeJsonStringify, 1);
  defineGlobal(vm, "json", OBJ_VAL(json));

  ObjInstance* yaml = makeModule(vm, "yaml");
  moduleAdd(vm, yaml, "parse", nativeYamlParse, 1);
  moduleAdd(vm, yaml, "stringify", nativeYamlStringify, 1);
  defineGlobal(vm, "yaml", OBJ_VAL(yaml));

  ObjInstance* math = makeModule(vm, "math");
  moduleAdd(vm, math, "abs", nativeMathAbs, 1);
  moduleAdd(vm, math, "floor", nativeMathFloor, 1);
  moduleAdd(vm, math, "ceil", nativeMathCeil, 1);
  moduleAdd(vm, math, "round", nativeMathRound, 1);
  moduleAdd(vm, math, "sqrt", nativeMathSqrt, 1);
  moduleAdd(vm, math, "pow", nativeMathPow, 2);
  moduleAdd(vm, math, "min", nativeMathMin, -1);
  moduleAdd(vm, math, "max", nativeMathMax, -1);
  moduleAdd(vm, math, "clamp", nativeMathClamp, 3);
  moduleAddValue(vm, math, "PI", NUMBER_VAL(3.141592653589793));
  moduleAddValue(vm, math, "E", NUMBER_VAL(2.718281828459045));
  defineGlobal(vm, "math", OBJ_VAL(math));

  ObjInstance* random = makeModule(vm, "random");
  moduleAdd(vm, random, "seed", nativeRandomSeed, 1);
  moduleAdd(vm, random, "int", nativeRandomInt, -1);
  moduleAdd(vm, random, "float", nativeRandomFloat, -1);
  moduleAdd(vm, random, "choice", nativeRandomChoice, 1);
  moduleAdd(vm, random, "normal", nativeRandomNormal, 2);
  moduleAdd(vm, random, "gaussian", nativeRandomGaussian, 2);
  moduleAdd(vm, random, "exponential", nativeRandomExponential, 1);
  moduleAdd(vm, random, "uniform", nativeRandomUniform, -1);
  defineGlobal(vm, "random", OBJ_VAL(random));

  ObjInstance* str = makeModule(vm, "str");
  moduleAdd(vm, str, "upper", nativeStrUpper, 1);
  moduleAdd(vm, str, "lower", nativeStrLower, 1);
  moduleAdd(vm, str, "trim", nativeStrTrim, 1);
  moduleAdd(vm, str, "trimStart", nativeStrTrimStart, 1);
  moduleAdd(vm, str, "trimEnd", nativeStrTrimEnd, 1);
  moduleAdd(vm, str, "startsWith", nativeStrStartsWith, 2);
  moduleAdd(vm, str, "endsWith", nativeStrEndsWith, 2);
  moduleAdd(vm, str, "contains", nativeStrContains, 2);
  moduleAdd(vm, str, "split", nativeStrSplit, 2);
  moduleAdd(vm, str, "join", nativeStrJoin, 2);
  moduleAdd(vm, str, "builder", nativeStrBuilder, 0);
  moduleAdd(vm, str, "append", nativeStrAppend, 2);
  moduleAdd(vm, str, "build", nativeStrBuild, -1);
  moduleAdd(vm, str, "replace", nativeStrReplace, 3);
  moduleAdd(vm, str, "replaceAll", nativeStrReplaceAll, 3);
  moduleAdd(vm, str, "repeat", nativeStrRepeat, 2);
  defineGlobal(vm, "str", OBJ_VAL(str));

  ObjInstance* array = makeModule(vm, "array");
  moduleAdd(vm, array, "slice", nativeArraySlice, -1);
  moduleAdd(vm, array, "map", nativeArrayMap, 2);
  moduleAdd(vm, array, "filter", nativeArrayFilter, 2);
  moduleAdd(vm, array, "reduce", nativeArrayReduce, -1);
  moduleAdd(vm, array, "contains", nativeArrayContains, 2);
  moduleAdd(vm, array, "indexOf", nativeArrayIndexOf, 2);
  moduleAdd(vm, array, "concat", nativeArrayConcat, 2);
  moduleAdd(vm, array, "reverse", nativeArrayReverse, 1);
  defineGlobal(vm, "array", OBJ_VAL(array));

  ObjInstance* os = makeModule(vm, "os");
  moduleAdd(vm, os, "platform", nativeOsPlatform, 0);
  moduleAdd(vm, os, "arch", nativeOsArch, 0);
  moduleAdd(vm, os, "sep", nativeOsSep, 0);
  moduleAdd(vm, os, "eol", nativeOsEol, 0);
  moduleAdd(vm, os, "cwd", nativeOsCwd, 0);
  moduleAdd(vm, os, "home", nativeOsHome, 0);
  moduleAdd(vm, os, "tmp", nativeOsTmp, 0);
  defineGlobal(vm, "os", OBJ_VAL(os));

  ObjInstance* timeModule = makeModule(vm, "time");
  moduleAdd(vm, timeModule, "now", nativeTimeNow, 0);
  moduleAdd(vm, timeModule, "sleep", nativeTimeSleep, 1);
  moduleAdd(vm, timeModule, "format", nativeTimeFormat, -1);
  moduleAdd(vm, timeModule, "iso", nativeTimeIso, -1);
  moduleAdd(vm, timeModule, "parts", nativeTimeParts, -1);
  defineGlobal(vm, "time", OBJ_VAL(timeModule));

  ObjInstance* vec2 = makeModule(vm, "vec2");
  moduleAdd(vm, vec2, "make", nativeVec2Make, 2);
  moduleAdd(vm, vec2, "add", nativeVec2Add, 2);
  moduleAdd(vm, vec2, "sub", nativeVec2Sub, 2);
  moduleAdd(vm, vec2, "scale", nativeVec2Scale, 2);
  moduleAdd(vm, vec2, "dot", nativeVec2Dot, 2);
  moduleAdd(vm, vec2, "len", nativeVec2Len, 1);
  moduleAdd(vm, vec2, "norm", nativeVec2Norm, 1);
  moduleAdd(vm, vec2, "lerp", nativeVec2Lerp, 3);
  moduleAdd(vm, vec2, "dist", nativeVec2Dist, 2);
  defineGlobal(vm, "vec2", OBJ_VAL(vec2));

  ObjInstance* vec3 = makeModule(vm, "vec3");
  moduleAdd(vm, vec3, "make", nativeVec3Make, 3);
  moduleAdd(vm, vec3, "add", nativeVec3Add, 2);
  moduleAdd(vm, vec3, "sub", nativeVec3Sub, 2);
  moduleAdd(vm, vec3, "scale", nativeVec3Scale, 2);
  moduleAdd(vm, vec3, "dot", nativeVec3Dot, 2);
  moduleAdd(vm, vec3, "len", nativeVec3Len, 1);
  moduleAdd(vm, vec3, "norm", nativeVec3Norm, 1);
  moduleAdd(vm, vec3, "lerp", nativeVec3Lerp, 3);
  moduleAdd(vm, vec3, "dist", nativeVec3Dist, 2);
  moduleAdd(vm, vec3, "cross", nativeVec3Cross, 2);
  defineGlobal(vm, "vec3", OBJ_VAL(vec3));

  ObjInstance* vec4 = makeModule(vm, "vec4");
  moduleAdd(vm, vec4, "make", nativeVec4Make, 4);
  moduleAdd(vm, vec4, "add", nativeVec4Add, 2);
  moduleAdd(vm, vec4, "sub", nativeVec4Sub, 2);
  moduleAdd(vm, vec4, "scale", nativeVec4Scale, 2);
  moduleAdd(vm, vec4, "dot", nativeVec4Dot, 2);
  moduleAdd(vm, vec4, "len", nativeVec4Len, 1);
  moduleAdd(vm, vec4, "norm", nativeVec4Norm, 1);
  moduleAdd(vm, vec4, "lerp", nativeVec4Lerp, 3);
  moduleAdd(vm, vec4, "dist", nativeVec4Dist, 2);
  defineGlobal(vm, "vec4", OBJ_VAL(vec4));

  ObjInstance* http = makeModule(vm, "http");
  moduleAdd(vm, http, "get", nativeHttpGet, 1);
  moduleAdd(vm, http, "post", nativeHttpPost, 2);
  moduleAdd(vm, http, "request", nativeHttpRequest, 3);
  moduleAdd(vm, http, "serve", nativeHttpServe, -1);
  defineGlobal(vm, "http", OBJ_VAL(http));

  ObjInstance* proc = makeModule(vm, "proc");
  moduleAdd(vm, proc, "run", nativeProcRun, 1);
  defineGlobal(vm, "proc", OBJ_VAL(proc));

  ObjInstance* env = makeModule(vm, "env");
  moduleAdd(vm, env, "args", nativeEnvArgs, 0);
  moduleAdd(vm, env, "get", nativeEnvGet, 1);
  moduleAdd(vm, env, "set", nativeEnvSet, 2);
  moduleAdd(vm, env, "has", nativeEnvHas, 1);
  moduleAdd(vm, env, "unset", nativeEnvUnset, 1);
  moduleAdd(vm, env, "all", nativeEnvAll, 0);
  defineGlobal(vm, "env", OBJ_VAL(env));

  ObjInstance* di = makeModule(vm, "di");
  moduleAdd(vm, di, "container", nativeDiContainer, 0);
  moduleAdd(vm, di, "bind", nativeDiBind, 3);
  moduleAdd(vm, di, "singleton", nativeDiSingleton, 3);
  moduleAdd(vm, di, "value", nativeDiValue, 3);
  moduleAdd(vm, di, "resolve", nativeDiResolve, 2);
  defineGlobal(vm, "di", OBJ_VAL(di));

  ObjInstance* plugin = makeModule(vm, "plugin");
  moduleAdd(vm, plugin, "load", nativePluginLoad, 1);
  defineGlobal(vm, "plugin", OBJ_VAL(plugin));

#if ERKAO_HAS_GRAPHICS
  defineGraphicsModule(vm, makeModule, moduleAdd, defineGlobal);
#endif
}

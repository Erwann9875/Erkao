#include "erkao_stdlib.h"
#include "gc.h"
#include "interpreter_internal.h"
#include "plugin.h"

#include <float.h>
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

static bool numberIsFinite(double value) {
#ifdef _MSC_VER
  return _finite(value) != 0;
#else
  return isfinite(value);
#endif
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

static bool expectNumberArg(VM* vm, Value value, const char* message) {
  if (!IS_NUMBER(value)) {
    runtimeErrorValue(vm, message);
    return false;
  }
  return true;
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
                             size_t bodyLength, ObjMap* headers) {
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
  (void)argc;
  int portValue = 0;
  if (!httpPortFromValue(vm, args[0], &portValue)) return NULL_VAL;
  if (!isObjType(args[1], OBJ_MAP)) {
    return runtimeErrorValue(vm, "http.serve expects (port, routes).");
  }

  ObjMap* routes = (ObjMap*)AS_OBJ(args[1]);
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
      httpSendResponse(client, 400, "bad request", strlen("bad request"), NULL);
      bufferFree(&request);
      erkaoCloseSocket(client);
      continue;
    }

    httpLogRequest(&clientAddr, path, pathLen);

    Value routeValue;
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

    if (!found) {
      httpSendResponse(client, 404, "not found", strlen("not found"), NULL);
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
      httpSendResponse(client, 500, "invalid response", strlen("invalid response"), NULL);
      bufferFree(&request);
      erkaoCloseSocket(client);
      continue;
    }

    httpSendResponse(client, status, body, bodyLen, headers);
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
  return NULL_VAL;
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
  const char* value = getenv(name->chars);
  if (!value) return NULL_VAL;
  return OBJ_VAL(copyString(vm, value));
}

static Value nativeEnvArgs(VM* vm, int argc, Value* args) {
  (void)argc;
  (void)args;
  return OBJ_VAL(vm->args);
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
  defineGlobal(vm, "fs", OBJ_VAL(fs));

  ObjInstance* path = makeModule(vm, "path");
  moduleAdd(vm, path, "join", nativePathJoin, 2);
  moduleAdd(vm, path, "dirname", nativePathDirname, 1);
  moduleAdd(vm, path, "basename", nativePathBasename, 1);
  moduleAdd(vm, path, "extname", nativePathExtname, 1);
  defineGlobal(vm, "path", OBJ_VAL(path));

  ObjInstance* json = makeModule(vm, "json");
  moduleAdd(vm, json, "parse", nativeJsonParse, 1);
  moduleAdd(vm, json, "stringify", nativeJsonStringify, 1);
  defineGlobal(vm, "json", OBJ_VAL(json));

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

  ObjInstance* timeModule = makeModule(vm, "time");
  moduleAdd(vm, timeModule, "now", nativeTimeNow, 0);
  moduleAdd(vm, timeModule, "sleep", nativeTimeSleep, 1);
  defineGlobal(vm, "time", OBJ_VAL(timeModule));

  ObjInstance* http = makeModule(vm, "http");
  moduleAdd(vm, http, "get", nativeHttpGet, 1);
  moduleAdd(vm, http, "post", nativeHttpPost, 2);
  moduleAdd(vm, http, "request", nativeHttpRequest, 3);
  moduleAdd(vm, http, "serve", nativeHttpServe, 2);
  defineGlobal(vm, "http", OBJ_VAL(http));

  ObjInstance* proc = makeModule(vm, "proc");
  moduleAdd(vm, proc, "run", nativeProcRun, 1);
  defineGlobal(vm, "proc", OBJ_VAL(proc));

  ObjInstance* env = makeModule(vm, "env");
  moduleAdd(vm, env, "args", nativeEnvArgs, 0);
  moduleAdd(vm, env, "get", nativeEnvGet, 1);
  defineGlobal(vm, "env", OBJ_VAL(env));

  ObjInstance* plugin = makeModule(vm, "plugin");
  moduleAdd(vm, plugin, "load", nativePluginLoad, 1);
  defineGlobal(vm, "plugin", OBJ_VAL(plugin));
}

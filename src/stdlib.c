#include "erkao_stdlib.h"
#include "interpreter_internal.h"
#include "plugin.h"

#include <float.h>
#include <math.h>
#include <stdint.h>
#include <wchar.h>

#ifdef _WIN32
#include <windows.h>
#include <winhttp.h>
#else
#include <dirent.h>
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
static Value nativeHttpGet(VM* vm, int argc, Value* args) {
  (void)argc;
  (void)args;
  return runtimeErrorValue(vm, "http.get is only supported on Windows.");
}

static Value nativeHttpPost(VM* vm, int argc, Value* args) {
  (void)argc;
  (void)args;
  return runtimeErrorValue(vm, "http.post is only supported on Windows.");
}

static Value nativeHttpRequest(VM* vm, int argc, Value* args) {
  (void)argc;
  (void)args;
  return runtimeErrorValue(vm, "http.request is only supported on Windows.");
}
#endif

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

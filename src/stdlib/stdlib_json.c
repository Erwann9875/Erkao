#include "stdlib_internal.h"

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

  bool running = true;
  while (running) {
    if (vm->instructionBudget > 0 && vm->instructionCount > vm->instructionBudget) {
      running = false;
      continue;
    }
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


void stdlib_register_json(VM* vm, ObjInstance* module) {
  moduleAdd(vm, module, "parse", nativeJsonParse, 1);
  moduleAdd(vm, module, "stringify", nativeJsonStringify, 1);
}

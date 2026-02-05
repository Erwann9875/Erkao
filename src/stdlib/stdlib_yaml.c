#include "stdlib_internal.h"

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

static int objStringCompare(const void* a, const void* b) {
  const ObjString* left = *(const ObjString* const*)a;
  const ObjString* right = *(const ObjString* const*)b;
  return strcmp(left->chars, right->chars);
}

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
  if (!parser->buffer) {
    parser->error = "yaml.parse out of memory.";
    return false;
  }
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
      YamlLine* resized = (YamlLine*)realloc(parser->lines, sizeof(YamlLine) * (size_t)capacity);
      if (!resized) {
        parser->error = "yaml.parse out of memory.";
        return false;
      }
      parser->lines = resized;
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
        if (buffer.failed) {
          bufferFree(&buffer);
          *ok = false;
          *error = "yaml.parse out of memory.";
          return NULL;
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
        if (!result) {
          *ok = false;
          *error = "yaml.parse out of memory.";
        }
        return result;
      }
      bufferAppendChar(&buffer, c);
      if (buffer.failed) {
        bufferFree(&buffer);
        *ok = false;
        *error = "yaml.parse out of memory.";
        return NULL;
      }
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
          if (buffer.failed) {
            bufferFree(&buffer);
            *ok = false;
            *error = "yaml.parse out of memory.";
            return NULL;
          }
          i++;
          continue;
        }
        ObjString* result = copyStringWithLength(vm, buffer.data ? buffer.data : "",
                                                 (int)buffer.length);
        bufferFree(&buffer);
        if (!result) {
          *ok = false;
          *error = "yaml.parse out of memory.";
        }
        return result;
      }
      bufferAppendChar(&buffer, c);
      if (buffer.failed) {
        bufferFree(&buffer);
        *ok = false;
        *error = "yaml.parse out of memory.";
        return NULL;
      }
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
    ObjString* empty = copyString(vm, "");
    if (!empty) {
      *ok = false;
      *error = "yaml.parse out of memory.";
      return NULL_VAL;
    }
    return OBJ_VAL(empty);
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
    if (!value) {
      *ok = false;
      *error = "yaml.parse out of memory.";
      return NULL_VAL;
    }
    return OBJ_VAL(value);
  }

  char* end = NULL;
  double number = strtod(trimmed, &end);
  if (end && *end == '\0' && end != trimmed) {
    return NUMBER_VAL(number);
  }
  ObjString* str = copyString(vm, trimmed);
  if (!str) {
    *ok = false;
    *error = "yaml.parse out of memory.";
    return NULL_VAL;
  }
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
  if (!array) {
    parser->error = "yaml.parse out of memory.";
    *ok = false;
    return NULL_VAL;
  }
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
  if (!map) {
    parser->error = "yaml.parse out of memory.";
    *ok = false;
    return NULL_VAL;
  }
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
  if (buffer->failed) return false;
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
  return !buffer->failed;
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
    if (buffer->failed) {
      *error = "yaml.stringify out of memory.";
      return false;
    }
    return true;
  }
  for (int i = 0; i < array->count; i++) {
    yamlAppendIndent(buffer, indent);
    bufferAppendN(buffer, "- ", 2);
    if (buffer->failed) {
      *error = "yaml.stringify out of memory.";
      return false;
    }
    Value item = array->items[i];
    if (isObjType(item, OBJ_ARRAY) || isObjType(item, OBJ_MAP)) {
      if (buffer->length == 0) {
        *error = "yaml.stringify failed.";
        return false;
      }
      buffer->length -= 1;
      buffer->data[buffer->length] = '\0';
      bufferAppendChar(buffer, '\n');
      if (buffer->failed) {
        *error = "yaml.stringify out of memory.";
        return false;
      }
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
      if (buffer->failed) {
        *error = "yaml.stringify out of memory.";
        return false;
      }
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
    if (buffer->failed) {
      *error = "yaml.stringify out of memory.";
      return false;
    }
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
      if (!yamlAppendEscaped(buffer, key)) {
        free(keys);
        *error = "yaml.stringify out of memory.";
        return false;
      }
    } else {
      bufferAppendN(buffer, key->chars, (size_t)key->length);
      if (buffer->failed) {
        free(keys);
        *error = "yaml.stringify out of memory.";
        return false;
      }
    }
    if (isObjType(value, OBJ_ARRAY) || isObjType(value, OBJ_MAP)) {
      bufferAppendChar(buffer, ':');
      bufferAppendChar(buffer, '\n');
      if (buffer->failed) {
        free(keys);
        *error = "yaml.stringify out of memory.";
        return false;
      }
      if (!yamlStringifyValue(vm, buffer, value, indent + 2, depth + 1, error)) {
        free(keys);
        return false;
      }
    } else {
      bufferAppendN(buffer, ": ", 2);
      if (buffer->failed) {
        free(keys);
        *error = "yaml.stringify out of memory.";
        return false;
      }
      if (!yamlStringifyValue(vm, buffer, value, 0, depth + 1, error)) {
        free(keys);
        return false;
      }
    }
    if (i + 1 < keyCount) {
      bufferAppendChar(buffer, '\n');
      if (buffer->failed) {
        free(keys);
        *error = "yaml.stringify out of memory.";
        return false;
      }
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
    if (buffer->failed) {
      *error = "yaml.stringify out of memory.";
      return false;
    }
    return true;
  }
  if (IS_BOOL(value)) {
    if (AS_BOOL(value)) {
      bufferAppendN(buffer, "true", 4);
    } else {
      bufferAppendN(buffer, "false", 5);
    }
    if (buffer->failed) {
      *error = "yaml.stringify out of memory.";
      return false;
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
    if (buffer->failed) {
      *error = "yaml.stringify out of memory.";
      return false;
    }
    return true;
  }
  if (isObjType(value, OBJ_STRING)) {
    ObjString* str = (ObjString*)AS_OBJ(value);
    if (yamlStringNeedsQuotes(str->chars)) {
      if (!yamlAppendEscaped(buffer, str)) {
        *error = "yaml.stringify out of memory.";
        return false;
      }
      return true;
    }
    bufferAppendN(buffer, str->chars, (size_t)str->length);
    if (buffer->failed) {
      *error = "yaml.stringify out of memory.";
      return false;
    }
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
  if (!result) {
    return runtimeErrorValue(vm, "yaml.stringify out of memory.");
  }
  return OBJ_VAL(result);
}


void stdlib_register_yaml(VM* vm, ObjInstance* module) {
  moduleAdd(vm, module, "parse", nativeYamlParse, 1);
  moduleAdd(vm, module, "stringify", nativeYamlStringify, 1);
}

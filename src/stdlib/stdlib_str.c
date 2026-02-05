#include "stdlib_internal.h"

#include <limits.h>

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


void stdlib_register_str(VM* vm, ObjInstance* module) {
  moduleAdd(vm, module, "upper", nativeStrUpper, 1);
  moduleAdd(vm, module, "lower", nativeStrLower, 1);
  moduleAdd(vm, module, "trim", nativeStrTrim, 1);
  moduleAdd(vm, module, "trimStart", nativeStrTrimStart, 1);
  moduleAdd(vm, module, "trimEnd", nativeStrTrimEnd, 1);
  moduleAdd(vm, module, "startsWith", nativeStrStartsWith, 2);
  moduleAdd(vm, module, "endsWith", nativeStrEndsWith, 2);
  moduleAdd(vm, module, "contains", nativeStrContains, 2);
  moduleAdd(vm, module, "split", nativeStrSplit, 2);
  moduleAdd(vm, module, "join", nativeStrJoin, 2);
  moduleAdd(vm, module, "builder", nativeStrBuilder, 0);
  moduleAdd(vm, module, "append", nativeStrAppend, 2);
  moduleAdd(vm, module, "build", nativeStrBuild, -1);
  moduleAdd(vm, module, "replace", nativeStrReplace, 3);
  moduleAdd(vm, module, "replaceAll", nativeStrReplaceAll, 3);
  moduleAdd(vm, module, "repeat", nativeStrRepeat, 2);
}

#include "stdlib_internal.h"

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
        if (parts.failed) {
          stringListFree(&parts);
          return runtimeErrorValue(vm, "path.normalize out of memory.");
        }
      }
      continue;
    }
    stringListAddWithLength(&parts, begin, length);
    if (parts.failed) {
      stringListFree(&parts);
      return runtimeErrorValue(vm, "path.normalize out of memory.");
    }
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


void stdlib_register_path(VM* vm, ObjInstance* module) {
  moduleAdd(vm, module, "join", nativePathJoin, 2);
  moduleAdd(vm, module, "dirname", nativePathDirname, 1);
  moduleAdd(vm, module, "basename", nativePathBasename, 1);
  moduleAdd(vm, module, "extname", nativePathExtname, 1);
  moduleAdd(vm, module, "isAbs", nativePathIsAbs, 1);
  moduleAdd(vm, module, "normalize", nativePathNormalize, 1);
  moduleAdd(vm, module, "stem", nativePathStem, 1);
  moduleAdd(vm, module, "split", nativePathSplit, 1);
}

#include "stdlib_internal.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

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


void stdlib_register_fs(VM* vm, ObjInstance* module) {
  moduleAdd(vm, module, "readText", nativeFsReadText, 1);
  moduleAdd(vm, module, "writeText", nativeFsWriteText, 2);
  moduleAdd(vm, module, "exists", nativeFsExists, 1);
  moduleAdd(vm, module, "cwd", nativeFsCwd, 0);
  moduleAdd(vm, module, "listDir", nativeFsListDir, 1);
  moduleAdd(vm, module, "isFile", nativeFsIsFile, 1);
  moduleAdd(vm, module, "isDir", nativeFsIsDir, 1);
  moduleAdd(vm, module, "size", nativeFsSize, 1);
  moduleAdd(vm, module, "glob", nativeFsGlob, 1);
}

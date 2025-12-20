#include "interpreter_internal.h"
#include "compiler.h"
#include "parser.h"
#include "program.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void freeStatements(StmtArray* statements) {
  for (int i = 0; i < statements->count; i++) {
    freeStmt(statements->items[i]);
  }
  freeStmtArray(statements);
}

static char* readFilePath(const char* path) {
  FILE* file = fopen(path, "rb");
  if (!file) return NULL;

  fseek(file, 0L, SEEK_END);
  long size = ftell(file);
  rewind(file);

  if (size < 0) {
    fclose(file);
    return NULL;
  }

  char* buffer = (char*)malloc((size_t)size + 1);
  if (!buffer) {
    fclose(file);
    fprintf(stderr, "Out of memory.\n");
    exit(1);
  }

  size_t read = fread(buffer, 1, (size_t)size, file);
  buffer[read] = '\0';
  fclose(file);
  return buffer;
}

static bool isAbsolutePath(const char* path) {
  if (!path || path[0] == '\0') return false;
  if (path[0] == '/' || path[0] == '\\') return true;
  if (((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z')) &&
      path[1] == ':' && (path[2] == '\\' || path[2] == '/')) {
    return true;
  }
  return false;
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

static char* pathDirname(const char* path) {
  const char* lastSlash = strrchr(path, '/');
  const char* lastBackslash = strrchr(path, '\\');
  const char* sep = lastSlash;
  if (lastBackslash && (!sep || lastBackslash > sep)) {
    sep = lastBackslash;
  }

  if (!sep) {
    return copyCString(".", 1);
  }

  size_t length = (size_t)(sep - path);
  if (length == 0) {
    return copyCString(path, 1);
  }
  return copyCString(path, length);
}

static char* joinPaths(const char* dir, const char* rel) {
  if (!dir || dir[0] == '\0' || strcmp(dir, ".") == 0) {
    return copyCString(rel, strlen(rel));
  }
  char sep = '/';
  if (strchr(dir, '\\')) sep = '\\';
  size_t dirLen = strlen(dir);
  size_t relLen = strlen(rel);
  bool needsSep = dir[dirLen - 1] != '/' && dir[dirLen - 1] != '\\';
  size_t total = dirLen + (needsSep ? 1 : 0) + relLen;
  char* out = (char*)malloc(total + 1);
  if (!out) {
    fprintf(stderr, "Out of memory.\n");
    exit(1);
  }
  memcpy(out, dir, dirLen);
  size_t offset = dirLen;
  if (needsSep) out[offset++] = sep;
  memcpy(out + offset, rel, relLen);
  out[total] = '\0';
  return out;
}

bool hasExtension(const char* path) {
  const char* lastSlash = strrchr(path, '/');
  const char* lastBackslash = strrchr(path, '\\');
  const char* base = path;
  if (lastSlash && lastSlash + 1 > base) base = lastSlash + 1;
  if (lastBackslash && lastBackslash + 1 > base) base = lastBackslash + 1;
  return strchr(base, '.') != NULL;
}

char* resolveImportPath(const char* currentPath, const char* importPath) {
  if (isAbsolutePath(importPath) || !currentPath) {
    return copyCString(importPath, strlen(importPath));
  }

  char* base = pathDirname(currentPath);
  char* joined = joinPaths(base, importPath);
  free(base);
  return joined;
}

ObjFunction* loadModuleFunction(VM* vm, Token keyword, const char* path) {
  char* source = readFilePath(path);
  if (!source) {
    runtimeError(vm, keyword, "Failed to read import path.");
    return NULL;
  }

  bool lexError = false;
  TokenArray tokens = scanTokens(source, path, &lexError);
  if (lexError) {
    freeTokenArray(&tokens);
    free(source);
    return NULL;
  }

  StmtArray statements;
  bool parseOk = parseTokens(&tokens, source, path, &statements);
  freeTokenArray(&tokens);
  if (!parseOk) {
    freeStatements(&statements);
    free(source);
    return NULL;
  }

  Program* program = programCreate(vm, source, path, statements);
  ObjFunction* function = compileProgram(vm, program);
  if (!function) return NULL;
  return function;
}

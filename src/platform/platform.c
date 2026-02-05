#include "platform.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

static void platformOutOfMemory(void) {
  fprintf(stderr, "Out of memory.\n");
  exit(1);
}

char* platform_strdup(const char* src) {
  if (!src) src = "";
  size_t length = strlen(src);
  return platform_strndup(src, length);
}

char* platform_strndup(const char* src, size_t length) {
  if (!src) src = "";
  char* out = (char*)malloc(length + 1);
  if (!out) platformOutOfMemory();
  if (length > 0) {
    memcpy(out, src, length);
  }
  out[length] = '\0';
  return out;
}

char* platform_read_file(const char* path, size_t* out_size) {
  if (out_size) *out_size = 0;
  if (!path || path[0] == '\0') return NULL;

  FILE* file = fopen(path, "rb");
  if (!file) return NULL;

  if (fseek(file, 0L, SEEK_END) != 0) {
    fclose(file);
    return NULL;
  }
  long size = ftell(file);
  if (size < 0) {
    fclose(file);
    return NULL;
  }
  rewind(file);

  char* buffer = (char*)malloc((size_t)size + 1);
  if (!buffer) {
    fclose(file);
    platformOutOfMemory();
  }

  size_t read = fread(buffer, 1, (size_t)size, file);
  if (ferror(file)) {
    free(buffer);
    fclose(file);
    return NULL;
  }
  buffer[read] = '\0';
  fclose(file);

  if (out_size) *out_size = read;
  return buffer;
}

bool platform_path_exists(const char* path) {
  if (!path || path[0] == '\0') return false;
#ifdef _WIN32
  DWORD attrs = GetFileAttributesA(path);
  return attrs != INVALID_FILE_ATTRIBUTES;
#else
  struct stat st;
  return stat(path, &st) == 0;
#endif
}

bool platform_path_is_dir(const char* path) {
  if (!path || path[0] == '\0') return false;
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

bool platform_path_is_file(const char* path) {
  if (!path || path[0] == '\0') return false;
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

bool platform_path_is_absolute(const char* path) {
  if (!path || path[0] == '\0') return false;
  if (path[0] == '/' || path[0] == '\\') return true;
  if (((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z')) &&
      path[1] == ':' && (path[2] == '\\' || path[2] == '/')) {
    return true;
  }
  return false;
}

char platform_path_pick_separator(const char* left, const char* right) {
  if ((left && strchr(left, '\\')) || (right && strchr(right, '\\'))) return '\\';
  return '/';
}

char* platform_path_dirname(const char* path) {
  if (!path || path[0] == '\0') return platform_strdup(".");
  const char* lastSlash = strrchr(path, '/');
  const char* lastBackslash = strrchr(path, '\\');
  const char* sep = lastSlash;
  if (lastBackslash && (!sep || lastBackslash > sep)) {
    sep = lastBackslash;
  }
  if (!sep) return platform_strdup(".");

  size_t length = (size_t)(sep - path);
  if (length == 0) return platform_strndup(path, 1);
  return platform_strndup(path, length);
}

char* platform_path_basename(const char* path) {
  if (!path) return platform_strdup("");
  const char* lastSlash = strrchr(path, '/');
  const char* lastBackslash = strrchr(path, '\\');
  const char* base = path;
  if (lastSlash && lastSlash + 1 > base) base = lastSlash + 1;
  if (lastBackslash && lastBackslash + 1 > base) base = lastBackslash + 1;
  return platform_strdup(base);
}

char* platform_path_join(const char* left, const char* right) {
  if (!right) right = "";
  if (!left || left[0] == '\0' || strcmp(left, ".") == 0) {
    return platform_strdup(right);
  }

  char sep = platform_path_pick_separator(left, right);
  size_t leftLen = strlen(left);
  size_t rightLen = strlen(right);
  bool needSep = left[leftLen - 1] != '/' && left[leftLen - 1] != '\\';
  size_t total = leftLen + (needSep ? 1 : 0) + rightLen;

  char* out = (char*)malloc(total + 1);
  if (!out) platformOutOfMemory();
  memcpy(out, left, leftLen);
  size_t offset = leftLen;
  if (needSep) out[offset++] = sep;
  if (rightLen > 0) {
    memcpy(out + offset, right, rightLen);
  }
  out[total] = '\0';
  return out;
}

bool platform_make_dir(const char* path) {
  if (!path || path[0] == '\0') return false;
#ifdef _WIN32
  if (CreateDirectoryA(path, NULL)) return true;
  return GetLastError() == ERROR_ALREADY_EXISTS;
#else
  if (mkdir(path, 0755) == 0) return true;
  return errno == EEXIST;
#endif
}

bool platform_ensure_dir(const char* path) {
  if (!path || path[0] == '\0') return false;
  if (platform_path_is_dir(path)) return true;

  char* buffer = platform_strdup(path);
  char* cursor = buffer;
  if (isalpha((unsigned char)cursor[0]) && cursor[1] == ':') {
    cursor += 2;
  }
  while (*cursor) {
    if (*cursor == '/' || *cursor == '\\') {
      char saved = *cursor;
      *cursor = '\0';
      if (buffer[0] != '\0' && !platform_path_is_dir(buffer)) {
        if (!platform_make_dir(buffer)) {
          free(buffer);
          return false;
        }
      }
      *cursor = saved;
    }
    cursor++;
  }
  if (!platform_path_is_dir(buffer)) {
    if (!platform_make_dir(buffer)) {
      free(buffer);
      return false;
    }
  }
  free(buffer);
  return true;
}

char* platform_get_cwd(void) {
#ifdef _WIN32
  DWORD length = GetCurrentDirectoryA(0, NULL);
  if (length == 0) return NULL;

  char* buffer = (char*)malloc((size_t)length + 1);
  if (!buffer) platformOutOfMemory();
  DWORD written = GetCurrentDirectoryA(length + 1, buffer);
  if (written == 0 || written > length) {
    free(buffer);
    return NULL;
  }
  return buffer;
#else
  return getcwd(NULL, 0);
#endif
}

#include "interpreter_internal.h"
#include "singlepass.h"
#include "program.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

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

static bool isRelativePath(const char* path) {
  if (!path || path[0] == '\0') return false;
  if (path[0] != '.') return false;
  if (path[1] == '/' || path[1] == '\\') return true;
  if (path[1] == '.' && (path[2] == '/' || path[2] == '\\')) return true;
  return false;
}

static bool pathExists(const char* path) {
#ifdef _WIN32
  DWORD attrs = GetFileAttributesA(path);
  return attrs != INVALID_FILE_ATTRIBUTES;
#else
  struct stat st;
  return stat(path, &st) == 0;
#endif
}

static bool isDirectory(const char* path) {
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

static char* getCwd(void) {
#ifdef _WIN32
  DWORD length = GetCurrentDirectoryA(0, NULL);
  if (length == 0) return NULL;
  char* buffer = (char*)malloc((size_t)length + 1);
  if (!buffer) {
    fprintf(stderr, "Out of memory.\n");
    exit(1);
  }
  if (GetCurrentDirectoryA(length + 1, buffer) == 0) {
    free(buffer);
    return NULL;
  }
  return buffer;
#else
  return getcwd(NULL, 0);
#endif
}

static char* resolveModuleFile(const char* basePath) {
  if (!basePath) return NULL;

  if (pathExists(basePath) && !isDirectory(basePath)) {
    return copyCString(basePath, strlen(basePath));
  }

  if (!hasExtension(basePath)) {
    size_t length = strlen(basePath);
    char* withExt = (char*)malloc(length + 4);
    if (!withExt) {
      fprintf(stderr, "Out of memory.\n");
      exit(1);
    }
    memcpy(withExt, basePath, length);
    memcpy(withExt + length, ".ek", 4);
    if (pathExists(withExt) && !isDirectory(withExt)) {
      return withExt;
    }
    free(withExt);
  }

  if (isDirectory(basePath)) {
    char* mainPath = joinPaths(basePath, "main.ek");
    if (pathExists(mainPath) && !isDirectory(mainPath)) return mainPath;
    free(mainPath);
    char* indexPath = joinPaths(basePath, "index.ek");
    if (pathExists(indexPath) && !isDirectory(indexPath)) return indexPath;
    free(indexPath);
  }

  return NULL;
}

static bool hasMarkerFile(const char* dir, const char* name) {
  char* path = joinPaths(dir, name);
  bool ok = pathExists(path);
  free(path);
  return ok;
}

static char* findProjectRoot(const char* startPath) {
  char* startDir = NULL;
  if (startPath && startPath[0] != '\0') {
    startDir = pathDirname(startPath);
  } else {
    startDir = getCwd();
  }
  if (!startDir) return NULL;

  char* current = startDir;
  for (;;) {
    if (hasMarkerFile(current, "erkao.mod") || hasMarkerFile(current, "erkao.lock")) {
      return current;
    }
    char* parent = pathDirname(current);
    if (!parent || strcmp(parent, current) == 0) {
      free(parent);
      return current;
    }
    free(current);
    current = parent;
  }
}

static void stripComment(char* line) {
  char* hash = strchr(line, '#');
  char* slash = strstr(line, "//");
  char* cut = NULL;
  if (hash && slash) {
    cut = hash < slash ? hash : slash;
  } else if (hash) {
    cut = hash;
  } else if (slash) {
    cut = slash;
  }
  if (cut) *cut = '\0';
}

static char* readLockedVersion(const char* root, const char* name) {
  if (!root || !name || name[0] == '\0') return NULL;
  char* lockPath = joinPaths(root, "erkao.lock");
  FILE* file = fopen(lockPath, "rb");
  free(lockPath);
  if (!file) return NULL;

  char buffer[512];
  while (fgets(buffer, sizeof(buffer), file)) {
    stripComment(buffer);
    char* line = buffer;
    while (*line && isspace((unsigned char)*line)) line++;
    if (*line == '\0') continue;

    char* token = strtok(line, " \t\r\n");
    if (!token) continue;
    if (strcmp(token, "lock") == 0) continue;
    char* dep = token;
    char* version = strtok(NULL, " \t\r\n");
    if (!version) continue;
    if (strcmp(dep, name) == 0) {
      size_t length = strlen(version);
      char* out = copyCString(version, length);
      fclose(file);
      return out;
    }
  }
  fclose(file);
  return NULL;
}

static bool isSimpleVersion(const char* text) {
  if (!text || text[0] == '\0') return false;
  for (const char* c = text; *c; c++) {
    if (*c == '.') continue;
    if (!isdigit((unsigned char)*c)) return false;
  }
  return true;
}

static int compareVersion(const char* a, const char* b) {
  if (!isSimpleVersion(a) || !isSimpleVersion(b)) {
    return strcmp(a, b);
  }
  const char* pa = a;
  const char* pb = b;
  while (*pa != '\0' || *pb != '\0') {
    long va = 0;
    long vb = 0;
    while (*pa && *pa != '.') {
      va = va * 10 + (*pa - '0');
      pa++;
    }
    while (*pb && *pb != '.') {
      vb = vb * 10 + (*pb - '0');
      pb++;
    }
    if (va != vb) return va < vb ? -1 : 1;
    if (*pa == '.') pa++;
    if (*pb == '.') pb++;
  }
  return 0;
}

typedef struct {
  int major;
  int minor;
  int patch;
} Semver;

typedef struct {
  Semver min;
  Semver max;
  bool hasMin;
  bool hasMax;
  bool minInclusive;
  bool maxInclusive;
} SemverRange;

static void semverRangeInit(SemverRange* range) {
  range->hasMin = false;
  range->hasMax = false;
  range->minInclusive = false;
  range->maxInclusive = false;
}

static int compareSemver(const Semver* a, const Semver* b) {
  if (a->major != b->major) return a->major < b->major ? -1 : 1;
  if (a->minor != b->minor) return a->minor < b->minor ? -1 : 1;
  if (a->patch != b->patch) return a->patch < b->patch ? -1 : 1;
  return 0;
}

static bool parseSemverParts(const char* text, Semver* out, int* outParts) {
  if (!text || !isdigit((unsigned char)text[0])) return false;
  int parts = 0;
  long values[3] = {0, 0, 0};
  const char* p = text;
  while (*p && parts < 3) {
    if (!isdigit((unsigned char)*p)) return false;
    long value = 0;
    while (*p && isdigit((unsigned char)*p)) {
      value = value * 10 + (*p - '0');
      p++;
    }
    values[parts++] = value;
    if (*p == '.') {
      p++;
      if (*p == '\0') return false;
      continue;
    }
    break;
  }
  if (*p != '\0') return false;
  out->major = (int)values[0];
  out->minor = parts > 1 ? (int)values[1] : 0;
  out->patch = parts > 2 ? (int)values[2] : 0;
  if (outParts) *outParts = parts;
  return true;
}

static char* nextToken(char* text, const char* delimiters, char** context) {
  char* start = text ? text : (context ? *context : NULL);
  if (!start) return NULL;
  start += strspn(start, delimiters);
  if (*start == '\0') {
    if (context) *context = NULL;
    return NULL;
  }
  char* end = start + strcspn(start, delimiters);
  if (*end != '\0') {
    *end = '\0';
    if (context) *context = end + 1;
  } else if (context) {
    *context = NULL;
  }
  return start;
}

static void semverRangeApplyMin(SemverRange* range, const Semver* min, bool inclusive) {
  if (!range->hasMin) {
    range->min = *min;
    range->minInclusive = inclusive;
    range->hasMin = true;
    return;
  }
  int cmp = compareSemver(min, &range->min);
  if (cmp > 0 || (cmp == 0 && !inclusive && range->minInclusive)) {
    range->min = *min;
    range->minInclusive = inclusive;
  }
}

static void semverRangeApplyMax(SemverRange* range, const Semver* max, bool inclusive) {
  if (!range->hasMax) {
    range->max = *max;
    range->maxInclusive = inclusive;
    range->hasMax = true;
    return;
  }
  int cmp = compareSemver(max, &range->max);
  if (cmp < 0 || (cmp == 0 && !inclusive && range->maxInclusive)) {
    range->max = *max;
    range->maxInclusive = inclusive;
  }
}

static bool semverMatchesRange(const Semver* version, const SemverRange* range) {
  if (range->hasMin) {
    int cmp = compareSemver(version, &range->min);
    if (cmp < 0 || (cmp == 0 && !range->minInclusive)) return false;
  }
  if (range->hasMax) {
    int cmp = compareSemver(version, &range->max);
    if (cmp > 0 || (cmp == 0 && !range->maxInclusive)) return false;
  }
  return true;
}

static bool parseWildcardRange(const char* token, SemverRange* out) {
  if (!token || token[0] == '\0') return false;
  if (strcmp(token, "*") == 0 || strcmp(token, "x") == 0 || strcmp(token, "X") == 0) {
    semverRangeInit(out);
    return true;
  }
  if (!strchr(token, '*') && !strchr(token, 'x') && !strchr(token, 'X')) return false;

  char* copy = copyCString(token, strlen(token));
  char* parts[3] = {NULL, NULL, NULL};
  int count = 0;
  char* ctx = NULL;
  char* piece = nextToken(copy, ".", &ctx);
  while (piece && count < 3) {
    parts[count++] = piece;
    piece = nextToken(NULL, ".", &ctx);
  }
  if (piece != NULL) {
    free(copy);
    return false;
  }

  bool wildcard[3] = {false, false, false};
  int values[3] = {0, 0, 0};
  for (int i = 0; i < count; i++) {
    if (strcmp(parts[i], "*") == 0 || strcmp(parts[i], "x") == 0 || strcmp(parts[i], "X") == 0) {
      wildcard[i] = true;
      continue;
    }
    if (parts[i][0] == '\0') {
      free(copy);
      return false;
    }
    int value = 0;
    for (const char* c = parts[i]; *c; c++) {
      if (!isdigit((unsigned char)*c)) {
        free(copy);
        return false;
      }
      value = value * 10 + (*c - '0');
    }
    values[i] = value;
  }
  for (int i = 0; i < count; i++) {
    if (wildcard[i]) {
      for (int j = i + 1; j < count; j++) {
        if (!wildcard[j]) {
          free(copy);
          return false;
        }
      }
      break;
    }
  }

  semverRangeInit(out);
  if (wildcard[0]) {
    free(copy);
    return true;
  }
  if (count >= 2 && wildcard[1]) {
    Semver min = {values[0], 0, 0};
    Semver max = {values[0] + 1, 0, 0};
    semverRangeApplyMin(out, &min, true);
    semverRangeApplyMax(out, &max, false);
    free(copy);
    return true;
  }
  if (count >= 3 && wildcard[2]) {
    Semver min = {values[0], values[1], 0};
    Semver max = {values[0], values[1] + 1, 0};
    semverRangeApplyMin(out, &min, true);
    semverRangeApplyMax(out, &max, false);
    free(copy);
    return true;
  }

  free(copy);
  return false;
}

static bool applyRangeToken(const char* token, SemverRange* range) {
  if (!token || token[0] == '\0') return false;

  SemverRange wildcard;
  if (parseWildcardRange(token, &wildcard)) {
    if (wildcard.hasMin) semverRangeApplyMin(range, &wildcard.min, wildcard.minInclusive);
    if (wildcard.hasMax) semverRangeApplyMax(range, &wildcard.max, wildcard.maxInclusive);
    return true;
  }

  if (token[0] == '^' || token[0] == '~') {
    Semver base;
    int parts = 0;
    if (!parseSemverParts(token + 1, &base, &parts)) return false;
    Semver max = base;
    if (token[0] == '^') {
      if (base.major > 0) {
        max.major = base.major + 1;
        max.minor = 0;
        max.patch = 0;
      } else if (base.minor > 0) {
        max.minor = base.minor + 1;
        max.patch = 0;
      } else {
        max.patch = base.patch + 1;
      }
    } else {
      if (parts <= 1) {
        max.major = base.major + 1;
        max.minor = 0;
        max.patch = 0;
      } else {
        max.minor = base.minor + 1;
        max.patch = 0;
      }
    }
    semverRangeApplyMin(range, &base, true);
    semverRangeApplyMax(range, &max, false);
    return true;
  }

  const char* op = NULL;
  if (strncmp(token, ">=", 2) == 0 || strncmp(token, "<=", 2) == 0) {
    op = token;
  } else if (token[0] == '>' || token[0] == '<' || token[0] == '=') {
    op = token;
  }

  if (op) {
    int opLen = (op[1] == '=' ? 2 : 1);
    Semver base;
    int parts = 0;
    if (!parseSemverParts(token + opLen, &base, &parts)) return false;
    if (op[0] == '>') {
      semverRangeApplyMin(range, &base, opLen == 2);
      return true;
    }
    if (op[0] == '<') {
      semverRangeApplyMax(range, &base, opLen == 2);
      return true;
    }
    if (op[0] == '=') {
      semverRangeApplyMin(range, &base, true);
      semverRangeApplyMax(range, &base, true);
      return true;
    }
  }

  Semver exact;
  if (parseSemverParts(token, &exact, NULL)) {
    semverRangeApplyMin(range, &exact, true);
    semverRangeApplyMax(range, &exact, true);
    return true;
  }

  return false;
}

static bool parseVersionRange(const char* text, SemverRange* out) {
  if (!text) return false;
  char* copy = copyCString(text, strlen(text));
  char* start = copy;
  while (*start && isspace((unsigned char)*start)) start++;
  char* end = start + strlen(start);
  while (end > start && isspace((unsigned char)end[-1])) {
    end--;
  }
  *end = '\0';
  if (*start == '\0') {
    free(copy);
    return false;
  }

  semverRangeInit(out);
  bool any = false;
  if (strpbrk(start, " \t") != NULL) {
    char* ctx = NULL;
    char* token = nextToken(start, " \t\r\n", &ctx);
    while (token) {
      if (!applyRangeToken(token, out)) {
        free(copy);
        return false;
      }
      any = true;
      token = nextToken(NULL, " \t\r\n", &ctx);
    }
  } else {
    if (!applyRangeToken(start, out)) {
      free(copy);
      return false;
    }
    any = true;
  }

  free(copy);
  return any;
}

static char* findBestVersionInDir(const char* baseDir, const SemverRange* range) {
  if (!baseDir || !isDirectory(baseDir)) return NULL;
  char* best = NULL;
  Semver bestVersion = {0, 0, 0};
  bool hasBest = false;
#ifdef _WIN32
  size_t length = strlen(baseDir);
  char* pattern = (char*)malloc(length + 3);
  if (!pattern) {
    fprintf(stderr, "Out of memory.\n");
    exit(1);
  }
  memcpy(pattern, baseDir, length);
  pattern[length] = '\\';
  pattern[length + 1] = '*';
  pattern[length + 2] = '\0';

  WIN32_FIND_DATAA data;
  HANDLE handle = FindFirstFileA(pattern, &data);
  free(pattern);
  if (handle == INVALID_HANDLE_VALUE) return NULL;
  do {
    if (strcmp(data.cFileName, ".") == 0 || strcmp(data.cFileName, "..") == 0) {
      continue;
    }
    if (!(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
    Semver candidate;
    if (!parseSemverParts(data.cFileName, &candidate, NULL)) continue;
    if (!semverMatchesRange(&candidate, range)) continue;
    if (!hasBest || compareSemver(&candidate, &bestVersion) > 0) {
      free(best);
      best = copyCString(data.cFileName, strlen(data.cFileName));
      bestVersion = candidate;
      hasBest = true;
    }
  } while (FindNextFileA(handle, &data));
  FindClose(handle);
#else
  DIR* dir = opendir(baseDir);
  if (!dir) return NULL;
  struct dirent* entry;
  while ((entry = readdir(dir)) != NULL) {
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
      continue;
    }
    char* candidatePath = joinPaths(baseDir, entry->d_name);
    bool isDir = isDirectory(candidatePath);
    free(candidatePath);
    if (!isDir) continue;
    Semver candidate;
    if (!parseSemverParts(entry->d_name, &candidate, NULL)) continue;
    if (!semverMatchesRange(&candidate, range)) continue;
    if (!hasBest || compareSemver(&candidate, &bestVersion) > 0) {
      free(best);
      best = copyCString(entry->d_name, strlen(entry->d_name));
      bestVersion = candidate;
      hasBest = true;
    }
  }
  closedir(dir);
#endif
  return best;
}

static char* findLatestVersionInDir(const char* baseDir) {
  if (!baseDir || !isDirectory(baseDir)) return NULL;
  char* best = NULL;
#ifdef _WIN32
  size_t length = strlen(baseDir);
  char* pattern = (char*)malloc(length + 3);
  if (!pattern) {
    fprintf(stderr, "Out of memory.\n");
    exit(1);
  }
  memcpy(pattern, baseDir, length);
  pattern[length] = '\\';
  pattern[length + 1] = '*';
  pattern[length + 2] = '\0';

  WIN32_FIND_DATAA data;
  HANDLE handle = FindFirstFileA(pattern, &data);
  free(pattern);
  if (handle == INVALID_HANDLE_VALUE) return NULL;
  do {
    if (strcmp(data.cFileName, ".") == 0 || strcmp(data.cFileName, "..") == 0) {
      continue;
    }
    if (!(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
    if (!best || compareVersion(data.cFileName, best) > 0) {
      free(best);
      best = copyCString(data.cFileName, strlen(data.cFileName));
    }
  } while (FindNextFileA(handle, &data));
  FindClose(handle);
#else
  DIR* dir = opendir(baseDir);
  if (!dir) return NULL;
  struct dirent* entry;
  while ((entry = readdir(dir)) != NULL) {
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
      continue;
    }
    char* candidate = joinPaths(baseDir, entry->d_name);
    bool isDir = isDirectory(candidate);
    free(candidate);
    if (!isDir) continue;
    if (!best || compareVersion(entry->d_name, best) > 0) {
      free(best);
      best = copyCString(entry->d_name, strlen(entry->d_name));
    }
  }
  closedir(dir);
#endif
  return best;
}

static void parseModuleSpec(const char* importPath, char** outName,
                            char** outVersion, char** outSubpath) {
  *outName = NULL;
  *outVersion = NULL;
  *outSubpath = NULL;
  if (!importPath) return;

  const char* slash = strpbrk(importPath, "/\\");
  const char* at = strchr(importPath, '@');
  if (at && slash && at > slash) at = NULL;

  const char* nameEnd = at ? at : (slash ? slash : importPath + strlen(importPath));
  if (nameEnd == importPath) return;
  *outName = copyCString(importPath, (size_t)(nameEnd - importPath));

  if (at) {
    const char* versionEnd = slash ? slash : importPath + strlen(importPath);
    if (at + 1 < versionEnd) {
      *outVersion = copyCString(at + 1, (size_t)(versionEnd - (at + 1)));
    }
  }

  if (slash && slash[1] != '\0') {
    *outSubpath = copyCString(slash + 1, strlen(slash + 1));
  }
}

typedef struct {
  char* name;
  char* path;
} PackageExport;

typedef struct {
  char* mainPath;
  PackageExport* exports;
  int count;
  int capacity;
} PackageMeta;

static void packageMetaInit(PackageMeta* meta) {
  meta->mainPath = NULL;
  meta->exports = NULL;
  meta->count = 0;
  meta->capacity = 0;
}

static void packageMetaFree(PackageMeta* meta) {
  free(meta->mainPath);
  for (int i = 0; i < meta->count; i++) {
    free(meta->exports[i].name);
    free(meta->exports[i].path);
  }
  free(meta->exports);
  packageMetaInit(meta);
}

static void packageMetaAddExport(PackageMeta* meta, const char* name, const char* path) {
  if (!name || !path) return;
  for (int i = 0; i < meta->count; i++) {
    if (strcmp(meta->exports[i].name, name) == 0) {
      free(meta->exports[i].path);
      meta->exports[i].path = copyCString(path, strlen(path));
      return;
    }
  }
  if (meta->capacity < meta->count + 1) {
    int oldCap = meta->capacity;
    meta->capacity = oldCap == 0 ? 4 : oldCap * 2;
    meta->exports = (PackageExport*)realloc(meta->exports,
                                            sizeof(PackageExport) * (size_t)meta->capacity);
    if (!meta->exports) {
      fprintf(stderr, "Out of memory.\n");
      exit(1);
    }
  }
  meta->exports[meta->count].name = copyCString(name, strlen(name));
  meta->exports[meta->count].path = copyCString(path, strlen(path));
  meta->count++;
}

static bool readPackageMeta(const char* packageDir, PackageMeta* out) {
  packageMetaInit(out);
  if (!packageDir) return false;
  char* manifestPath = joinPaths(packageDir, "erkao.mod");
  FILE* file = fopen(manifestPath, "rb");
  free(manifestPath);
  if (!file) return false;

  char buffer[512];
  while (fgets(buffer, sizeof(buffer), file)) {
    stripComment(buffer);
    char* line = buffer;
    while (*line && isspace((unsigned char)*line)) line++;
    if (*line == '\0') continue;

    char* ctx = NULL;
    char* token = nextToken(line, " \t\r\n", &ctx);
    if (!token) continue;
    if (strcmp(token, "main") == 0) {
      char* path = nextToken(NULL, " \t\r\n", &ctx);
      if (path && path[0] != '\0') {
        free(out->mainPath);
        out->mainPath = copyCString(path, strlen(path));
      }
    } else if (strcmp(token, "export") == 0 || strcmp(token, "exports") == 0) {
      char* name = nextToken(NULL, " \t\r\n", &ctx);
      char* path = nextToken(NULL, " \t\r\n", &ctx);
      if (name && path) {
        packageMetaAddExport(out, name, path);
      }
    }
  }

  fclose(file);
  return true;
}

static char* resolvePackageEntry(const char* versionDir, const char* subpath) {
  if (!versionDir) return NULL;
  PackageMeta meta;
  bool hasMeta = readPackageMeta(versionDir, &meta);

  char* base = NULL;
  if (subpath && subpath[0] != '\0') {
    if (hasMeta) {
      for (int i = 0; i < meta.count; i++) {
        if (strcmp(meta.exports[i].name, subpath) == 0) {
          base = joinPaths(versionDir, meta.exports[i].path);
          break;
        }
      }
    }
    if (!base) {
      base = joinPaths(versionDir, subpath);
    }
  } else if (hasMeta && meta.mainPath) {
    base = joinPaths(versionDir, meta.mainPath);
  } else {
    base = copyCString(versionDir, strlen(versionDir));
  }

  char* resolved = resolveModuleFile(base);
  free(base);
  if (hasMeta) packageMetaFree(&meta);
  return resolved;
}

static char* resolvePackagePath(const char* packagesDir, const char* name,
                                const char* version, const char* subpath) {
  if (!packagesDir || !name || !version) return NULL;
  char* nameDir = joinPaths(packagesDir, name);
  char* versionDir = joinPaths(nameDir, version);
  free(nameDir);
  char* resolved = resolvePackageEntry(versionDir, subpath);
  free(versionDir);
  return resolved;
}

static char* resolveFromModulePaths(VM* vm, const char* importPath) {
  if (!vm || !importPath) return NULL;
  for (int i = 0; i < vm->modulePathCount; i++) {
    char* candidate = joinPaths(vm->modulePaths[i], importPath);
    char* resolved = resolveModuleFile(candidate);
    free(candidate);
    if (resolved) return resolved;
  }
  return NULL;
}

bool hasExtension(const char* path) {
  const char* lastSlash = strrchr(path, '/');
  const char* lastBackslash = strrchr(path, '\\');
  const char* base = path;
  if (lastSlash && lastSlash + 1 > base) base = lastSlash + 1;
  if (lastBackslash && lastBackslash + 1 > base) base = lastBackslash + 1;
  return strchr(base, '.') != NULL;
}

char* resolveImportPath(VM* vm, const char* currentPath, const char* importPath) {
  if (!importPath || importPath[0] == '\0') return NULL;

  bool hasAt = strchr(importPath, '@') != NULL;
  bool treatAsFile = isAbsolutePath(importPath) || isRelativePath(importPath) ||
                     (!hasAt && hasExtension(importPath));

  if (treatAsFile) {
    char* base = NULL;
    if (isAbsolutePath(importPath)) {
      base = copyCString(importPath, strlen(importPath));
    } else {
      char* baseDir = currentPath ? pathDirname(currentPath) : getCwd();
      if (!baseDir) return NULL;
      base = joinPaths(baseDir, importPath);
      free(baseDir);
    }
    char* resolved = resolveModuleFile(base);
    if (!resolved) {
      if (hasExtension(base)) {
        resolved = copyCString(base, strlen(base));
      } else {
        size_t length = strlen(base);
        resolved = (char*)malloc(length + 4);
        if (!resolved) {
          fprintf(stderr, "Out of memory.\n");
          exit(1);
        }
        memcpy(resolved, base, length);
        memcpy(resolved + length, ".ek", 4);
      }
    }
    free(base);
    return resolved;
  }

  char* name = NULL;
  char* version = NULL;
  char* subpath = NULL;
  parseModuleSpec(importPath, &name, &version, &subpath);
  if (!name) {
    free(version);
    free(subpath);
    return NULL;
  }

  const char* projectRoot = vm ? vm->projectRoot : NULL;
  char* computedRoot = NULL;
  if (!projectRoot) {
    computedRoot = findProjectRoot(currentPath);
    projectRoot = computedRoot;
    if (vm && projectRoot) {
      vmSetProjectRoot(vm, projectRoot);
    }
  }

  const char* globalPackages = vm ? vm->globalPackagesDir : NULL;
  char* packagesDir = projectRoot ? joinPaths(projectRoot, "packages") : NULL;

  bool explicitVersion = version != NULL;
  bool lockVersion = false;
  if (!explicitVersion && projectRoot) {
    version = readLockedVersion(projectRoot, name);
    if (version) lockVersion = true;
  }

  if (explicitVersion || lockVersion) {
    char* resolvedVersion = NULL;
    if (version) {
      SemverRange range;
      if (parseVersionRange(version, &range)) {
        char* localBest = NULL;
        char* globalBest = NULL;
        if (packagesDir) {
          char* nameDir = joinPaths(packagesDir, name);
          localBest = findBestVersionInDir(nameDir, &range);
          free(nameDir);
        }
        if (globalPackages) {
          char* nameDir = joinPaths(globalPackages, name);
          globalBest = findBestVersionInDir(nameDir, &range);
          free(nameDir);
        }
        if (localBest && globalBest) {
          Semver localSemver;
          Semver globalSemver;
          if (parseSemverParts(localBest, &localSemver, NULL) &&
              parseSemverParts(globalBest, &globalSemver, NULL) &&
              compareSemver(&globalSemver, &localSemver) > 0) {
            free(localBest);
            resolvedVersion = globalBest;
          } else {
            free(globalBest);
            resolvedVersion = localBest;
          }
        } else {
          resolvedVersion = localBest ? localBest : globalBest;
        }
        if (!resolvedVersion) {
          free(version);
          free(name);
          free(subpath);
          free(packagesDir);
          free(computedRoot);
          return NULL;
        }
      }
    }
    char* resolved = NULL;
    const char* useVersion = resolvedVersion ? resolvedVersion : version;
    if (packagesDir) {
      resolved = resolvePackagePath(packagesDir, name, useVersion, subpath);
    }
    if (!resolved && globalPackages) {
      resolved = resolvePackagePath(globalPackages, name, useVersion, subpath);
    }
    free(resolvedVersion);
    free(version);
    free(name);
    free(subpath);
    free(packagesDir);
    free(computedRoot);
    return resolved;
  }

  char* resolved = resolveFromModulePaths(vm, importPath);
  if (resolved) {
    free(name);
    free(subpath);
    free(packagesDir);
    free(computedRoot);
    return resolved;
  }

  if (packagesDir) {
    char* nameDir = joinPaths(packagesDir, name);
    char* latest = findLatestVersionInDir(nameDir);
    free(nameDir);
    if (latest) {
      resolved = resolvePackagePath(packagesDir, name, latest, subpath);
      free(latest);
    }
  }

  if (!resolved && globalPackages) {
    char* nameDir = joinPaths(globalPackages, name);
    char* latest = findLatestVersionInDir(nameDir);
    free(nameDir);
    if (latest) {
      resolved = resolvePackagePath(globalPackages, name, latest, subpath);
      free(latest);
    }
  }

  free(name);
  free(subpath);
  free(packagesDir);
  free(computedRoot);
  return resolved;
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

  bool compileError = false;
  ObjFunction* function = compile(vm, &tokens, source, path, &compileError);
  freeTokenArray(&tokens);
  
  if (compileError || !function) {
    free(source);
    return NULL;
  }

  Program* program = programCreate(vm, source, path, function);
  function->program = program;
  
  return function;
}

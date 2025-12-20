#include "package.h"

#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
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

#define ERKAO_MANIFEST_NAME "erkao.mod"
#define ERKAO_LOCK_NAME "erkao.lock"

typedef struct {
  char* name;
  char* version;
} PackageDep;

typedef struct {
  char* name;
  char* version;
  PackageDep* deps;
  int count;
  int capacity;
} PackageManifest;

static char* copyCString(const char* src) {
  size_t length = strlen(src);
  char* out = (char*)malloc(length + 1);
  if (!out) {
    fprintf(stderr, "Out of memory.\n");
    exit(1);
  }
  memcpy(out, src, length + 1);
  return out;
}

static char* copyCStringRange(const char* start, size_t length) {
  char* out = (char*)malloc(length + 1);
  if (!out) {
    fprintf(stderr, "Out of memory.\n");
    exit(1);
  }
  memcpy(out, start, length);
  out[length] = '\0';
  return out;
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

static bool makeDir(const char* path) {
#ifdef _WIN32
  if (CreateDirectoryA(path, NULL)) return true;
  return GetLastError() == ERROR_ALREADY_EXISTS;
#else
  if (mkdir(path, 0755) == 0) return true;
  return errno == EEXIST;
#endif
}

static bool ensureDir(const char* path) {
  if (!path || path[0] == '\0') return false;
  if (isDirectory(path)) return true;

  char* buffer = copyCString(path);
  char* cursor = buffer;
  if (isalpha((unsigned char)cursor[0]) && cursor[1] == ':') {
    cursor += 2;
  }
  while (*cursor) {
    if (*cursor == '/' || *cursor == '\\') {
      char saved = *cursor;
      *cursor = '\0';
      if (buffer[0] != '\0' && !isDirectory(buffer)) {
        if (!makeDir(buffer)) {
          free(buffer);
          return false;
        }
      }
      *cursor = saved;
    }
    cursor++;
  }
  if (!isDirectory(buffer)) {
    if (!makeDir(buffer)) {
      free(buffer);
      return false;
    }
  }
  free(buffer);
  return true;
}

static char* pathDirname(const char* path) {
  const char* lastSlash = strrchr(path, '/');
  const char* lastBackslash = strrchr(path, '\\');
  const char* sep = lastSlash;
  if (lastBackslash && (!sep || lastBackslash > sep)) {
    sep = lastBackslash;
  }
  if (!sep) return copyCString(".");
  size_t length = (size_t)(sep - path);
  if (length == 0) return copyCString(path);
  return copyCStringRange(path, length);
}

static char* pathBasename(const char* path) {
  const char* lastSlash = strrchr(path, '/');
  const char* lastBackslash = strrchr(path, '\\');
  const char* base = path;
  if (lastSlash && lastSlash + 1 > base) base = lastSlash + 1;
  if (lastBackslash && lastBackslash + 1 > base) base = lastBackslash + 1;
  return copyCString(base);
}

static char* joinPaths(const char* left, const char* right) {
  if (!left || left[0] == '\0' || strcmp(left, ".") == 0) {
    return copyCString(right);
  }
  char sep = '/';
  if (strchr(left, '\\') || strchr(right, '\\')) sep = '\\';
  size_t leftLen = strlen(left);
  size_t rightLen = strlen(right);
  bool needSep = left[leftLen - 1] != '/' && left[leftLen - 1] != '\\';
  size_t total = leftLen + (needSep ? 1 : 0) + rightLen;
  char* out = (char*)malloc(total + 1);
  if (!out) {
    fprintf(stderr, "Out of memory.\n");
    exit(1);
  }
  memcpy(out, left, leftLen);
  size_t offset = leftLen;
  if (needSep) out[offset++] = sep;
  memcpy(out + offset, right, rightLen);
  out[total] = '\0';
  return out;
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

static bool copyFile(const char* src, const char* dst) {
  FILE* in = fopen(src, "rb");
  if (!in) return false;
  char* dir = pathDirname(dst);
  if (!ensureDir(dir)) {
    free(dir);
    fclose(in);
    return false;
  }
  free(dir);
  FILE* out = fopen(dst, "wb");
  if (!out) {
    fclose(in);
    return false;
  }
  char buffer[4096];
  size_t read = 0;
  while ((read = fread(buffer, 1, sizeof(buffer), in)) > 0) {
    if (fwrite(buffer, 1, read, out) != read) {
      fclose(in);
      fclose(out);
      return false;
    }
  }
  fclose(in);
  fclose(out);
  return true;
}

static bool shouldSkipEntry(const char* name) {
  return strcmp(name, ".") == 0 || strcmp(name, "..") == 0 ||
         strcmp(name, "packages") == 0 || strcmp(name, ".git") == 0;
}

static bool copyDirRecursive(const char* src, const char* dst) {
  if (!ensureDir(dst)) return false;
#ifdef _WIN32
  size_t length = strlen(src);
  char* pattern = (char*)malloc(length + 3);
  if (!pattern) {
    fprintf(stderr, "Out of memory.\n");
    exit(1);
  }
  memcpy(pattern, src, length);
  pattern[length] = '\\';
  pattern[length + 1] = '*';
  pattern[length + 2] = '\0';

  WIN32_FIND_DATAA data;
  HANDLE handle = FindFirstFileA(pattern, &data);
  free(pattern);
  if (handle == INVALID_HANDLE_VALUE) return false;
  do {
    if (shouldSkipEntry(data.cFileName)) continue;
    char* srcPath = joinPaths(src, data.cFileName);
    char* dstPath = joinPaths(dst, data.cFileName);
    if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
      if (!copyDirRecursive(srcPath, dstPath)) {
        free(srcPath);
        free(dstPath);
        FindClose(handle);
        return false;
      }
    } else {
      if (!copyFile(srcPath, dstPath)) {
        free(srcPath);
        free(dstPath);
        FindClose(handle);
        return false;
      }
    }
    free(srcPath);
    free(dstPath);
  } while (FindNextFileA(handle, &data));
  FindClose(handle);
#else
  DIR* dir = opendir(src);
  if (!dir) return false;
  struct dirent* entry;
  while ((entry = readdir(dir)) != NULL) {
    if (shouldSkipEntry(entry->d_name)) continue;
    char* srcPath = joinPaths(src, entry->d_name);
    char* dstPath = joinPaths(dst, entry->d_name);
    if (isDirectory(srcPath)) {
      if (!copyDirRecursive(srcPath, dstPath)) {
        free(srcPath);
        free(dstPath);
        closedir(dir);
        return false;
      }
    } else {
      if (!copyFile(srcPath, dstPath)) {
        free(srcPath);
        free(dstPath);
        closedir(dir);
        return false;
      }
    }
    free(srcPath);
    free(dstPath);
  }
  closedir(dir);
#endif
  return true;
}

static void manifestInit(PackageManifest* manifest) {
  manifest->name = NULL;
  manifest->version = NULL;
  manifest->deps = NULL;
  manifest->count = 0;
  manifest->capacity = 0;
}

static void manifestFree(PackageManifest* manifest) {
  free(manifest->name);
  free(manifest->version);
  for (int i = 0; i < manifest->count; i++) {
    free(manifest->deps[i].name);
    free(manifest->deps[i].version);
  }
  free(manifest->deps);
  manifestInit(manifest);
}

static void manifestAddDep(PackageManifest* manifest, const char* name,
                           const char* version) {
  for (int i = 0; i < manifest->count; i++) {
    if (strcmp(manifest->deps[i].name, name) == 0) {
      free(manifest->deps[i].version);
      manifest->deps[i].version = copyCString(version);
      return;
    }
  }
  if (manifest->capacity < manifest->count + 1) {
    int oldCapacity = manifest->capacity;
    manifest->capacity = oldCapacity == 0 ? 4 : oldCapacity * 2;
    manifest->deps = (PackageDep*)realloc(manifest->deps,
                                          sizeof(PackageDep) * (size_t)manifest->capacity);
    if (!manifest->deps) {
      fprintf(stderr, "Out of memory.\n");
      exit(1);
    }
  }
  manifest->deps[manifest->count].name = copyCString(name);
  manifest->deps[manifest->count].version = copyCString(version);
  manifest->count++;
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

static bool parseManifest(const char* path, PackageManifest* out, const char** error) {
  FILE* file = fopen(path, "rb");
  if (!file) {
    if (error) *error = "Failed to open manifest.";
    return false;
  }

  manifestInit(out);
  char buffer[512];
  while (fgets(buffer, sizeof(buffer), file)) {
    stripComment(buffer);
    char* line = buffer;
    while (*line && isspace((unsigned char)*line)) line++;
    if (*line == '\0') continue;

    char* token = strtok(line, " \t\r\n");
    if (!token) continue;
    if (strcmp(token, "module") == 0) {
      char* name = strtok(NULL, " \t\r\n");
      char* version = strtok(NULL, " \t\r\n");
      if (!name || !version) {
        fclose(file);
        if (error) *error = "Invalid module line.";
        manifestFree(out);
        return false;
      }
      free(out->name);
      free(out->version);
      out->name = copyCString(name);
      out->version = copyCString(version);
    } else if (strcmp(token, "require") == 0) {
      char* name = strtok(NULL, " \t\r\n");
      char* version = strtok(NULL, " \t\r\n");
      if (!name || !version) {
        fclose(file);
        if (error) *error = "Invalid require line.";
        manifestFree(out);
        return false;
      }
      manifestAddDep(out, name, version);
    }
  }

  fclose(file);
  if (!out->name || !out->version) {
    if (error) *error = "Manifest missing module line.";
    manifestFree(out);
    return false;
  }
  return true;
}

static bool writeManifest(const char* path, const PackageManifest* manifest) {
  FILE* file = fopen(path, "wb");
  if (!file) return false;
  fprintf(file, "module %s %s\n", manifest->name, manifest->version);
  for (int i = 0; i < manifest->count; i++) {
    fprintf(file, "require %s %s\n", manifest->deps[i].name, manifest->deps[i].version);
  }
  fclose(file);
  return true;
}

static bool writeLock(const char* path, const PackageManifest* manifest) {
  FILE* file = fopen(path, "wb");
  if (!file) return false;
  fprintf(file, "lock 1\n");
  for (int i = 0; i < manifest->count; i++) {
    fprintf(file, "%s %s\n", manifest->deps[i].name, manifest->deps[i].version);
  }
  fclose(file);
  return true;
}

static int readLock(const char* path, PackageDep** outDeps, int* outCount) {
  FILE* file = fopen(path, "rb");
  if (!file) return -1;
  PackageDep* deps = NULL;
  int count = 0;
  int capacity = 0;
  char buffer[512];
  while (fgets(buffer, sizeof(buffer), file)) {
    stripComment(buffer);
    char* line = buffer;
    while (*line && isspace((unsigned char)*line)) line++;
    if (*line == '\0') continue;

    char* token = strtok(line, " \t\r\n");
    if (!token) continue;
    if (strcmp(token, "lock") == 0) continue;
    char* name = token;
    char* version = strtok(NULL, " \t\r\n");
    if (!version) continue;
    if (capacity < count + 1) {
      int oldCapacity = capacity;
      capacity = oldCapacity == 0 ? 4 : oldCapacity * 2;
      deps = (PackageDep*)realloc(deps, sizeof(PackageDep) * (size_t)capacity);
      if (!deps) {
        fprintf(stderr, "Out of memory.\n");
        exit(1);
      }
    }
    deps[count].name = copyCString(name);
    deps[count].version = copyCString(version);
    count++;
  }
  fclose(file);
  *outDeps = deps;
  *outCount = count;
  return 0;
}

static void freeDeps(PackageDep* deps, int count) {
  for (int i = 0; i < count; i++) {
    free(deps[i].name);
    free(deps[i].version);
  }
  free(deps);
}

static char* findProjectRoot(const char* startPath) {
  char* current = startPath ? pathDirname(startPath) : getCwd();
  if (!current) return NULL;

  for (;;) {
    char* manifestPath = joinPaths(current, ERKAO_MANIFEST_NAME);
    bool hasManifest = pathExists(manifestPath);
    free(manifestPath);
    char* lockPath = joinPaths(current, ERKAO_LOCK_NAME);
    bool hasLock = pathExists(lockPath);
    free(lockPath);
    if (hasManifest || hasLock) {
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

static void printPkgHelp(const char* exe) {
  fprintf(stdout,
          "Usage:\n"
          "  %s pkg init [name] [version]\n"
          "  %s pkg add <path> [--global]\n"
          "  %s pkg install\n"
          "  %s pkg list\n",
          exe, exe, exe, exe);
}

static char* resolveGlobalPackagesDir(void) {
  const char* overridePath = getenv("ERKAO_PACKAGES");
  if (overridePath && overridePath[0] != '\0') {
    return copyCString(overridePath);
  }

#ifdef _WIN32
  const char* home = getenv("USERPROFILE");
  char* homeBuffer = NULL;
  if (!home || home[0] == '\0') {
    const char* drive = getenv("HOMEDRIVE");
    const char* path = getenv("HOMEPATH");
    if (drive && path) {
      size_t length = strlen(drive) + strlen(path);
      homeBuffer = (char*)malloc(length + 1);
      if (!homeBuffer) {
        fprintf(stderr, "Out of memory.\n");
        exit(1);
      }
      strcpy(homeBuffer, drive);
      strcat(homeBuffer, path);
      home = homeBuffer;
    }
  }
  if (!home || home[0] == '\0') {
    home = ".";
  }
  const char* suffix = "\\.erkao\\packages";
  size_t length = strlen(home) + strlen(suffix);
  char* path = (char*)malloc(length + 1);
  if (!path) {
    fprintf(stderr, "Out of memory.\n");
    exit(1);
  }
  strcpy(path, home);
  strcat(path, suffix);
  free(homeBuffer);
  return path;
#else
  const char* home = getenv("HOME");
  if (!home || home[0] == '\0') home = ".";
  const char* suffix = "/.erkao/packages";
  size_t length = strlen(home) + strlen(suffix);
  char* path = (char*)malloc(length + 1);
  if (!path) {
    fprintf(stderr, "Out of memory.\n");
    exit(1);
  }
  strcpy(path, home);
  strcat(path, suffix);
  return path;
#endif
}

static int cmdPkgInit(const char* name, const char* version) {
  char* cwd = getCwd();
  if (!cwd) return 1;
  char* manifestPath = joinPaths(cwd, ERKAO_MANIFEST_NAME);
  if (pathExists(manifestPath)) {
    fprintf(stderr, "Manifest already exists: %s\n", manifestPath);
    free(cwd);
    free(manifestPath);
    return 1;
  }
  PackageManifest manifest;
  manifestInit(&manifest);
  char* defaultName = NULL;
  if (!name) {
    defaultName = pathBasename(cwd);
    name = defaultName;
  }
  manifest.name = copyCString(name);
  manifest.version = copyCString(version ? version : "0.1.0");
  bool ok = writeManifest(manifestPath, &manifest);
  manifestFree(&manifest);
  free(defaultName);
  char* packagesDir = joinPaths(cwd, "packages");
  ensureDir(packagesDir);
  free(packagesDir);
  free(cwd);
  free(manifestPath);
  return ok ? 0 : 1;
}

static int cmdPkgAdd(const char* path, bool copyGlobal) {
  if (!path) {
    fprintf(stderr, "Missing package path.\n");
    return 1;
  }

  char* cwd = getCwd();
  if (!cwd) return 1;

  char* projectRoot = findProjectRoot(cwd);
  if (!projectRoot) {
    fprintf(stderr, "Failed to locate project root.\n");
    free(cwd);
    return 1;
  }

  char* manifestPath = joinPaths(projectRoot, ERKAO_MANIFEST_NAME);
  const char* error = NULL;
  PackageManifest manifest;
  if (!parseManifest(manifestPath, &manifest, &error)) {
    fprintf(stderr, "%s\n", error ? error : "Failed to read manifest.");
    free(projectRoot);
    free(cwd);
    free(manifestPath);
    return 1;
  }

  char* packageDir = pathExists(path) ? copyCString(path) : joinPaths(cwd, path);
  char* packageManifestPath = joinPaths(packageDir, ERKAO_MANIFEST_NAME);
  PackageManifest packageManifest;
  if (!parseManifest(packageManifestPath, &packageManifest, &error)) {
    fprintf(stderr, "%s\n", error ? error : "Failed to read package manifest.");
    manifestFree(&manifest);
    free(projectRoot);
    free(cwd);
    free(manifestPath);
    free(packageDir);
    free(packageManifestPath);
    return 1;
  }

  manifestAddDep(&manifest, packageManifest.name, packageManifest.version);
  if (!writeManifest(manifestPath, &manifest)) {
    fprintf(stderr, "Failed to write manifest.\n");
    manifestFree(&manifest);
    manifestFree(&packageManifest);
    free(projectRoot);
    free(cwd);
    free(manifestPath);
    free(packageDir);
    free(packageManifestPath);
    return 1;
  }

  char* lockPath = joinPaths(projectRoot, ERKAO_LOCK_NAME);
  if (!writeLock(lockPath, &manifest)) {
    fprintf(stderr, "Failed to write lock file.\n");
    free(lockPath);
    manifestFree(&manifest);
    manifestFree(&packageManifest);
    free(projectRoot);
    free(cwd);
    free(manifestPath);
    free(packageDir);
    free(packageManifestPath);
    return 1;
  }
  free(lockPath);

  char* packagesDir = joinPaths(projectRoot, "packages");
  char* destRoot = joinPaths(packagesDir, packageManifest.name);
  char* destDir = joinPaths(destRoot, packageManifest.version);
  free(destRoot);
  if (!copyDirRecursive(packageDir, destDir)) {
    fprintf(stderr, "Failed to copy package.\n");
    free(destDir);
    free(packagesDir);
    manifestFree(&manifest);
    manifestFree(&packageManifest);
    free(projectRoot);
    free(cwd);
    free(manifestPath);
    free(packageDir);
    free(packageManifestPath);
    return 1;
  }
  free(destDir);
  free(packagesDir);

  char* globalDir = resolveGlobalPackagesDir();
  if (copyGlobal && globalDir) {
    char* globalRoot = joinPaths(globalDir, packageManifest.name);
    char* globalDest = joinPaths(globalRoot, packageManifest.version);
    free(globalRoot);
    if (!copyDirRecursive(packageDir, globalDest)) {
      fprintf(stderr, "Failed to copy package to global cache.\n");
      free(globalDest);
      free(globalDir);
      manifestFree(&manifest);
      manifestFree(&packageManifest);
      free(projectRoot);
      free(cwd);
      free(manifestPath);
      free(packageDir);
      free(packageManifestPath);
      return 1;
    }
    free(globalDest);
  }
  free(globalDir);

  manifestFree(&manifest);
  manifestFree(&packageManifest);
  free(projectRoot);
  free(cwd);
  free(manifestPath);
  free(packageDir);
  free(packageManifestPath);
  return 0;
}

static int cmdPkgInstall(void) {
  char* cwd = getCwd();
  if (!cwd) return 1;
  char* projectRoot = findProjectRoot(cwd);
  if (!projectRoot) {
    free(cwd);
    return 1;
  }

  char* manifestPath = joinPaths(projectRoot, ERKAO_MANIFEST_NAME);
  const char* error = NULL;
  PackageManifest manifest;
  if (!parseManifest(manifestPath, &manifest, &error)) {
    fprintf(stderr, "%s\n", error ? error : "Failed to read manifest.");
    free(manifestPath);
    free(projectRoot);
    free(cwd);
    return 1;
  }

  char* packagesDir = joinPaths(projectRoot, "packages");
  char* globalDir = resolveGlobalPackagesDir();

  for (int i = 0; i < manifest.count; i++) {
    PackageDep* dep = &manifest.deps[i];
    char* localRoot = joinPaths(packagesDir, dep->name);
    char* localDir = joinPaths(localRoot, dep->version);
    free(localRoot);
    if (isDirectory(localDir)) {
      free(localDir);
      continue;
    }
    if (globalDir) {
      char* globalRoot = joinPaths(globalDir, dep->name);
      char* globalPkg = joinPaths(globalRoot, dep->version);
      free(globalRoot);
      if (isDirectory(globalPkg)) {
        if (!copyDirRecursive(globalPkg, localDir)) {
          fprintf(stderr, "Failed to copy %s@%s from cache.\n",
                  dep->name, dep->version);
          free(globalPkg);
          free(localDir);
          manifestFree(&manifest);
          free(packagesDir);
          free(globalDir);
          free(manifestPath);
          free(projectRoot);
          free(cwd);
          return 1;
        }
        free(globalPkg);
        free(localDir);
        continue;
      }
      free(globalPkg);
    }
    fprintf(stderr, "Missing package %s@%s.\n", dep->name, dep->version);
    free(localDir);
    manifestFree(&manifest);
    free(packagesDir);
    free(globalDir);
    free(manifestPath);
    free(projectRoot);
    free(cwd);
    return 1;
  }

  char* lockPath = joinPaths(projectRoot, ERKAO_LOCK_NAME);
  writeLock(lockPath, &manifest);
  free(lockPath);

  manifestFree(&manifest);
  free(packagesDir);
  free(globalDir);
  free(manifestPath);
  free(projectRoot);
  free(cwd);
  return 0;
}

static int cmdPkgList(void) {
  char* cwd = getCwd();
  if (!cwd) return 1;
  char* projectRoot = findProjectRoot(cwd);
  if (!projectRoot) {
    free(cwd);
    return 1;
  }

  char* lockPath = joinPaths(projectRoot, ERKAO_LOCK_NAME);
  if (pathExists(lockPath)) {
    PackageDep* deps = NULL;
    int count = 0;
    if (readLock(lockPath, &deps, &count) == 0) {
      for (int i = 0; i < count; i++) {
        printf("%s %s\n", deps[i].name, deps[i].version);
      }
    }
    freeDeps(deps, count);
    free(lockPath);
    free(projectRoot);
    free(cwd);
    return 0;
  }
  free(lockPath);

  char* manifestPath = joinPaths(projectRoot, ERKAO_MANIFEST_NAME);
  const char* error = NULL;
  PackageManifest manifest;
  if (!parseManifest(manifestPath, &manifest, &error)) {
    fprintf(stderr, "%s\n", error ? error : "Failed to read manifest.");
    free(manifestPath);
    free(projectRoot);
    free(cwd);
    return 1;
  }

  for (int i = 0; i < manifest.count; i++) {
    printf("%s %s\n", manifest.deps[i].name, manifest.deps[i].version);
  }
  manifestFree(&manifest);
  free(manifestPath);
  free(projectRoot);
  free(cwd);
  return 0;
}

int runPackageCommand(const char* exe, int argc, const char** argv) {
  if (argc < 2) {
    printPkgHelp(exe);
    return 64;
  }
  if (argc == 2) {
    printPkgHelp(exe);
    return 64;
  }

  const char* sub = argv[2];
  if (strcmp(sub, "init") == 0) {
    const char* name = argc > 3 ? argv[3] : NULL;
    const char* version = argc > 4 ? argv[4] : NULL;
    return cmdPkgInit(name, version);
  }
  if (strcmp(sub, "add") == 0) {
    bool copyGlobal = false;
    const char* path = NULL;
    for (int i = 3; i < argc; i++) {
      if (strcmp(argv[i], "--global") == 0 || strcmp(argv[i], "-g") == 0) {
        copyGlobal = true;
        continue;
      }
      if (!path) {
        path = argv[i];
        continue;
      }
    }
    if (!path) {
      fprintf(stderr, "Missing package path.\n");
      return 64;
    }
    return cmdPkgAdd(path, copyGlobal);
  }
  if (strcmp(sub, "install") == 0) {
    return cmdPkgInstall();
  }
  if (strcmp(sub, "list") == 0) {
    return cmdPkgList();
  }

  printPkgHelp(exe);
  return 64;
}

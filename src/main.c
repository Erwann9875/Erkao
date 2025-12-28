#include "lexer.h"
#include "singlepass.h"
#include "interpreter.h"
#include "program.h"
#include "tooling.h"
#include "package.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef ERKAO_VERSION
#define ERKAO_VERSION "dev"
#endif

#ifdef _WIN32
#include <windows.h>
#endif

#define HISTORY_LIMIT 1000

typedef struct {
  char** entries;
  int count;
  int capacity;
  char* path;
} History;

static char* readFile(const char* path) {
  FILE* file = fopen(path, "rb");
  if (!file) {
    fprintf(stderr, "Could not open file '%s'.\n", path);
    return NULL;
  }

  fseek(file, 0L, SEEK_END);
  long size = ftell(file);
  rewind(file);

  char* buffer = (char*)malloc((size_t)size + 1);
  if (!buffer) {
    fprintf(stderr, "Not enough memory to read '%s'.\n", path);
    fclose(file);
    return NULL;
  }

  size_t read = fread(buffer, 1, (size_t)size, file);
  buffer[read] = '\0';
  fclose(file);
  return buffer;
}

static bool fileExists(const char* path) {
  FILE* file = fopen(path, "rb");
  if (!file) return false;
  fclose(file);
  return true;
}

static void historyInit(History* history, char* path) {
  history->entries = NULL;
  history->count = 0;
  history->capacity = 0;
  history->path = path;
}

static void historyFree(History* history) {
  for (int i = 0; i < history->count; i++) {
    free(history->entries[i]);
  }
  free(history->entries);
  free(history->path);
  history->entries = NULL;
  history->count = 0;
  history->capacity = 0;
  history->path = NULL;
}

static void historyAdd(History* history, const char* line) {
  if (!line || line[0] == '\0') return;
  if (history->count > 0 && strcmp(history->entries[history->count - 1], line) == 0) {
    return;
  }
  if (history->count >= HISTORY_LIMIT) {
    free(history->entries[0]);
    memmove(history->entries, history->entries + 1,
            sizeof(char*) * (size_t)(history->count - 1));
    history->count--;
  }
  if (history->capacity < history->count + 1) {
    int oldCapacity = history->capacity;
    history->capacity = oldCapacity == 0 ? 32 : oldCapacity * 2;
    history->entries = (char**)realloc(history->entries,
                                       sizeof(char*) * (size_t)history->capacity);
    if (!history->entries) {
      fprintf(stderr, "Out of memory.\n");
      exit(1);
    }
  }
  size_t length = strlen(line);
  char* copy = (char*)malloc(length + 1);
  if (!copy) {
    fprintf(stderr, "Out of memory.\n");
    exit(1);
  }
  memcpy(copy, line, length + 1);
  history->entries[history->count++] = copy;
}

static void historyLoad(History* history) {
  if (!history->path) return;
  FILE* file = fopen(history->path, "rb");
  if (!file) return;

  char buffer[2048];
  while (fgets(buffer, sizeof(buffer), file)) {
    size_t length = strlen(buffer);
    while (length > 0 && (buffer[length - 1] == '\n' || buffer[length - 1] == '\r')) {
      buffer[--length] = '\0';
    }
    historyAdd(history, buffer);
  }

  fclose(file);
}

static void historyAppend(History* history, const char* line) {
  if (!history->path || !line || line[0] == '\0') return;
  FILE* file = fopen(history->path, "ab");
  if (!file) return;
  fwrite(line, 1, strlen(line), file);
  fwrite("\n", 1, 1, file);
  fclose(file);
}

static char* resolveHistoryPath(void) {
  const char* overridePath = getenv("ERKAO_HISTORY");
  if (overridePath && overridePath[0] != '\0') {
    char* copy = (char*)malloc(strlen(overridePath) + 1);
    if (!copy) {
      fprintf(stderr, "Out of memory.\n");
      exit(1);
    }
    strcpy(copy, overridePath);
    return copy;
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
  const char* name = "\\.erkao_history";
  size_t length = strlen(home) + strlen(name);
  char* path = (char*)malloc(length + 1);
  if (!path) {
    fprintf(stderr, "Out of memory.\n");
    exit(1);
  }
  strcpy(path, home);
  strcat(path, name);
  free(homeBuffer);
  return path;
#else
  const char* home = getenv("HOME");
  if (!home || home[0] == '\0') home = ".";
  const char* name = "/.erkao_history";
  size_t length = strlen(home) + strlen(name);
  char* path = (char*)malloc(length + 1);
  if (!path) {
    fprintf(stderr, "Out of memory.\n");
    exit(1);
  }
  strcpy(path, home);
  strcat(path, name);
  return path;
#endif
}

#ifdef _WIN32
static void redrawLine(const char* prompt, const char* buffer, int* previousLength) {
  int length = (int)strlen(buffer);
  printf("\r%s%s", prompt, buffer);
  if (*previousLength > length) {
    int diff = *previousLength - length;
    for (int i = 0; i < diff; i++) {
      putchar(' ');
    }
    printf("\r%s%s", prompt, buffer);
  }
  fflush(stdout);
  *previousLength = length;
}

static char* readLineWithHistory(const char* prompt, History* history) {
  HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
  DWORD mode = 0;
  if (!GetConsoleMode(input, &mode)) return NULL;

  DWORD rawMode = mode & ~(ENABLE_ECHO_INPUT | ENABLE_LINE_INPUT);
  SetConsoleMode(input, rawMode);

  int capacity = 1024;
  int length = 0;
  char* buffer = (char*)malloc((size_t)capacity);
  if (!buffer) {
    fprintf(stderr, "Out of memory.\n");
    SetConsoleMode(input, mode);
    return NULL;
  }
  buffer[0] = '\0';

  int previousLength = 0;
  int historyIndex = history->count;

  printf("%s", prompt);
  fflush(stdout);

  for (;;) {
    INPUT_RECORD record;
    DWORD read = 0;
    if (!ReadConsoleInput(input, &record, 1, &read)) {
      free(buffer);
      SetConsoleMode(input, mode);
      return NULL;
    }

    if (record.EventType != KEY_EVENT) continue;
    KEY_EVENT_RECORD key = record.Event.KeyEvent;
    if (!key.bKeyDown) continue;

    if ((key.dwControlKeyState & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) &&
        key.wVirtualKeyCode == 'C') {
      printf("^C\n");
      free(buffer);
      SetConsoleMode(input, mode);
      return NULL;
    }

    switch (key.wVirtualKeyCode) {
      case VK_RETURN:
        printf("\n");
        buffer[length] = '\0';
        SetConsoleMode(input, mode);
        return buffer;
      case VK_BACK:
        if (length > 0) {
          length--;
          buffer[length] = '\0';
          redrawLine(prompt, buffer, &previousLength);
        }
        break;
      case VK_UP:
        if (history->count > 0) {
          if (historyIndex > 0) historyIndex--;
          if (historyIndex >= 0 && historyIndex < history->count) {
            const char* entry = history->entries[historyIndex];
            size_t entryLen = strlen(entry);
            if ((int)entryLen + 1 > capacity) {
              capacity = (int)entryLen + 32;
              char* next = (char*)realloc(buffer, (size_t)capacity);
              if (!next) {
                fprintf(stderr, "Out of memory.\n");
                free(buffer);
                SetConsoleMode(input, mode);
                return NULL;
              }
              buffer = next;
            }
            memcpy(buffer, entry, entryLen + 1);
            length = (int)entryLen;
            redrawLine(prompt, buffer, &previousLength);
          }
        }
        break;
      case VK_DOWN:
        if (history->count > 0) {
          if (historyIndex < history->count - 1) {
            historyIndex++;
            const char* entry = history->entries[historyIndex];
            size_t entryLen = strlen(entry);
            if ((int)entryLen + 1 > capacity) {
              capacity = (int)entryLen + 32;
              char* next = (char*)realloc(buffer, (size_t)capacity);
              if (!next) {
                fprintf(stderr, "Out of memory.\n");
                free(buffer);
                SetConsoleMode(input, mode);
                return NULL;
              }
              buffer = next;
            }
            memcpy(buffer, entry, entryLen + 1);
            length = (int)entryLen;
          } else {
            historyIndex = history->count;
            length = 0;
            buffer[0] = '\0';
          }
          redrawLine(prompt, buffer, &previousLength);
        }
        break;
      default: {
        char c = key.uChar.AsciiChar;
        if (c >= 32) {
          if (length + 2 > capacity) {
            capacity *= 2;
            char* next = (char*)realloc(buffer, (size_t)capacity);
            if (!next) {
              fprintf(stderr, "Out of memory.\n");
              free(buffer);
              SetConsoleMode(input, mode);
              return NULL;
            }
            buffer = next;
          }
          buffer[length++] = c;
          buffer[length] = '\0';
          putchar(c);
          fflush(stdout);
          previousLength = length;
        }
        break;
      }
    }
  }
}
#else
static char* readLineWithHistory(const char* prompt, History* history) {
  (void)history;
  char buffer[1024];
  printf("%s", prompt);
  fflush(stdout);
  if (!fgets(buffer, sizeof(buffer), stdin)) {
    printf("\n");
    return NULL;
  }
  size_t length = strlen(buffer);
  if (length > 0 && buffer[length - 1] == '\n') {
    buffer[length - 1] = '\0';
  }
  char* copy = (char*)malloc(length + 1);
  if (!copy) {
    fprintf(stderr, "Out of memory.\n");
    return NULL;
  }
  memcpy(copy, buffer, length + 1);
  return copy;
}
#endif

static bool runSource(VM* vm, const char* path, char* source) {
  const char* displayPath = path ? path : "<repl>";
  bool lexError = false;
  TokenArray tokens = scanTokens(source, displayPath, &lexError);
  if (lexError) {
    freeTokenArray(&tokens);
    free(source);
    return false;
  }

  bool compileError = false;
  ObjFunction* function = compile(vm, &tokens, source, displayPath, &compileError);
  freeTokenArray(&tokens);
  if (compileError || !function) {
    free(source);
    return false;
  }

  Program* program = programCreate(vm, source, path, function);
  function->program = program;
  programRetain(program);
  return interpret(vm, program);
}

static int runFile(VM* vm, const char* path, int argc, const char** argv) {
  char* source = readFile(path);
  if (!source) return 74;

  vmSetArgs(vm, argc, argv);
  bool ok = runSource(vm, path, source);

  return ok ? 0 : 65;
}

static int typecheckFile(VM* vm, const char* path) {
  char* source = readFile(path);
  if (!source) return 74;

  const char* displayPath = path ? path : "<repl>";
  bool lexError = false;
  TokenArray tokens = scanTokens(source, displayPath, &lexError);
  if (lexError) {
    freeTokenArray(&tokens);
    free(source);
    return 65;
  }

  bool compileError = false;
  bool prevTypecheck = vm->typecheck;
  vm->typecheck = true;
  ObjFunction* function = compile(vm, &tokens, source, displayPath, &compileError);
  vm->typecheck = prevTypecheck;
  freeTokenArray(&tokens);
  free(source);
  return (compileError || !function) ? 65 : 0;
}

static void repl(VM* vm) {
  History history;
  historyInit(&history, resolveHistoryPath());
  historyLoad(&history);

  vmSetArgs(vm, 0, NULL);
  for (;;) {
    char* line = readLineWithHistory("> ", &history);
    if (!line) break;

    if (line[0] != '\0') {
      historyAdd(&history, line);
      historyAppend(&history, line);
    }

    size_t length = strlen(line);
    char* copy = (char*)malloc(length + 1);
    if (!copy) {
      fprintf(stderr, "Out of memory.\n");
      free(line);
      break;
    }
    memcpy(copy, line, length + 1);
    runSource(vm, NULL, copy);
    free(line);
    vm->hadError = false;
  }

  historyFree(&history);
}

static const char* exeName(const char* path) {
  const char* lastSlash = strrchr(path, '/');
  const char* lastBackslash = strrchr(path, '\\');
  const char* base = path;
  if (lastSlash && lastSlash + 1 > base) base = lastSlash + 1;
  if (lastBackslash && lastBackslash + 1 > base) base = lastBackslash + 1;
  return base;
}

static bool isFlag(const char* arg, const char* longName, const char* shortName) {
  if (longName && strcmp(arg, longName) == 0) return true;
  if (shortName && strcmp(arg, shortName) == 0) return true;
  return false;
}

static bool isDebugFlag(const char* arg) {
  return isFlag(arg, "--bytecode", "-d") || isFlag(arg, "--disasm", NULL);
}

static void printHelp(const char* exe) {
  fprintf(stdout,
          "Usage:\n"
          "  %s [--help|-h] [--version|-v]\n"
          "  %s repl\n"
          "  %s run [--bytecode|--disasm] <file> [-- args...]\n"
          "  %s typecheck <file>\n"
          "  %s pkg <command>\n"
          "  %s fmt <file> [--check]\n"
          "  %s lint <file>\n"
          "  %s [--bytecode|--disasm] <file> [args...]\n"
          "\n"
          "Commands:\n"
          "  run   Run a script file.\n"
          "  typecheck  Typecheck a script file.\n"
          "  repl  Start the interactive REPL.\n"
          "  pkg   Manage packages.\n"
          "  fmt   Format a source file in-place.\n"
          "  lint  Run simple formatting checks.\n"
          "\n"
          "Options:\n"
          "  -h, --help     Show this help.\n"
          "  -v, --version  Show the version.\n"
          "  --bytecode     Print bytecode before running.\n"
          "  --disasm       Alias for --bytecode.\n"
          "  --module-path  Add a module search path.\n"
          "  --check        Check formatting without writing changes.\n"
          "  --config       Tooling config file for fmt/lint.\n"
          "  --ruleset      Tooling ruleset name.\n"
          "  --indent       Formatter indentation width.\n"
          "  --max-line     Linter max line length.\n"
          "  --rules        Linter rules list (comma-separated).\n",
          exe, exe, exe, exe, exe, exe, exe, exe);
}

static void printVersion(void) {
  fprintf(stdout, "Erkao %s\n", ERKAO_VERSION);
}

static int runFormatCommand(const char* exe, int argc, const char** argv) {
  bool checkOnly = false;
  int files = 0;
  int exitCode = 0;
  ToolingConfig config;
  toolingConfigInit(&config);

  const char* configPath = NULL;
  for (int i = 2; i < argc; i++) {
    if (isFlag(argv[i], "--config", NULL)) {
      if (i + 1 >= argc) {
        fprintf(stderr, "Missing value for --config.\n");
        printHelp(exe);
        return 64;
      }
      configPath = argv[++i];
    }
  }

  if (configPath) {
    if (!toolingLoadConfig(configPath, &config)) {
      return 1;
    }
  } else if (fileExists("erkao.tooling")) {
    if (!toolingLoadConfig("erkao.tooling", &config)) {
      return 1;
    }
  }

  for (int i = 2; i < argc; i++) {
    if (isFlag(argv[i], "--check", "-c")) {
      checkOnly = true;
      continue;
    }
    if (isFlag(argv[i], "--config", NULL)) {
      i++;
      continue;
    }
    if (isFlag(argv[i], "--ruleset", "-r")) {
      if (i + 1 >= argc) {
        fprintf(stderr, "Missing value for --ruleset.\n");
        printHelp(exe);
        return 64;
      }
      if (!toolingApplyFormatRuleset(&config, argv[i + 1])) {
        fprintf(stderr, "Unknown format ruleset: %s\n", argv[i + 1]);
        return 64;
      }
      i++;
      continue;
    }
    if (isFlag(argv[i], "--indent", "-i")) {
      if (i + 1 >= argc) {
        fprintf(stderr, "Missing value for --indent.\n");
        printHelp(exe);
        return 64;
      }
      int indent = atoi(argv[i + 1]);
      if (indent <= 0) {
        fprintf(stderr, "Invalid indent: %s\n", argv[i + 1]);
        return 64;
      }
      config.formatIndent = indent;
      i++;
      continue;
    }
    if (argv[i][0] == '-') {
      fprintf(stderr, "Unknown option for 'fmt': %s\n", argv[i]);
      printHelp(exe);
      return 64;
    }
    bool changed = false;
    if (!formatFileWithConfig(argv[i], checkOnly, &changed, &config)) {
      return 1;
    }
    if (checkOnly && changed) {
      exitCode = 1;
    }
    files++;
  }

  if (files == 0) {
    fprintf(stderr, "Missing file for 'fmt'.\n");
    printHelp(exe);
    return 64;
  }

  return exitCode;
}

static int runLintCommand(const char* exe, int argc, const char** argv) {
  int files = 0;
  int issues = 0;
  ToolingConfig config;
  toolingConfigInit(&config);

  const char* configPath = NULL;
  for (int i = 2; i < argc; i++) {
    if (isFlag(argv[i], "--config", NULL)) {
      if (i + 1 >= argc) {
        fprintf(stderr, "Missing value for --config.\n");
        printHelp(exe);
        return 64;
      }
      configPath = argv[++i];
    }
  }

  if (configPath) {
    if (!toolingLoadConfig(configPath, &config)) {
      return 1;
    }
  } else if (fileExists("erkao.tooling")) {
    if (!toolingLoadConfig("erkao.tooling", &config)) {
      return 1;
    }
  }

  for (int i = 2; i < argc; i++) {
    if (isFlag(argv[i], "--config", NULL)) {
      i++;
      continue;
    }
    if (isFlag(argv[i], "--ruleset", "-r")) {
      if (i + 1 >= argc) {
        fprintf(stderr, "Missing value for --ruleset.\n");
        printHelp(exe);
        return 64;
      }
      if (!toolingApplyLintRuleset(&config, argv[i + 1])) {
        fprintf(stderr, "Unknown lint ruleset: %s\n", argv[i + 1]);
        return 64;
      }
      i++;
      continue;
    }
    if (isFlag(argv[i], "--rules", NULL)) {
      if (i + 1 >= argc) {
        fprintf(stderr, "Missing value for --rules.\n");
        printHelp(exe);
        return 64;
      }
      if (!toolingApplyLintRules(&config, argv[i + 1])) {
        fprintf(stderr, "Unknown lint rules: %s\n", argv[i + 1]);
        return 64;
      }
      i++;
      continue;
    }
    if (isFlag(argv[i], "--max-line", "-m")) {
      if (i + 1 >= argc) {
        fprintf(stderr, "Missing value for --max-line.\n");
        printHelp(exe);
        return 64;
      }
      int maxLine = atoi(argv[i + 1]);
      if (maxLine <= 0) {
        fprintf(stderr, "Invalid max line length: %s\n", argv[i + 1]);
        return 64;
      }
      config.lintMaxLine = maxLine;
      i++;
      continue;
    }
    if (argv[i][0] == '-') {
      fprintf(stderr, "Unknown option for 'lint': %s\n", argv[i]);
      printHelp(exe);
      return 64;
    }
    int result = lintFileWithConfig(argv[i], &config);
    if (result < 0) return 1;
    issues += result;
    files++;
  }

  if (files == 0) {
    fprintf(stderr, "Missing file for 'lint'.\n");
    printHelp(exe);
    return 64;
  }

  return issues > 0 ? 1 : 0;
}

static int runWithArgs(VM* vm, const char* path, int argc, const char** argv) {
  int argStart = 0;
  if (argc > 0 && strcmp(argv[0], "--") == 0) {
    argStart = 1;
  }
  return runFile(vm, path, argc - argStart, argv + argStart);
}

static int runFileCommand(VM* vm, const char* exe, int argc, const char** argv, int startIndex) {
  int index = startIndex;
  bool debugBytecode = false;
  while (index < argc) {
    if (isDebugFlag(argv[index])) {
      debugBytecode = true;
      index++;
      continue;
    }
    if (isFlag(argv[index], "--module-path", "-M")) {
      if (index + 1 >= argc) {
        fprintf(stderr, "Missing value for --module-path.\n");
        printHelp(exe);
        return 64;
      }
      vmAddModulePath(vm, argv[index + 1]);
      index += 2;
      continue;
    }
    break;
  }

  if (index >= argc || isFlag(argv[index], "--help", "-h")) {
    printHelp(exe);
    return index >= argc ? 64 : 0;
  }

  vm->debugBytecode = debugBytecode;
  const char* path = argv[index++];
  return runWithArgs(vm, path, argc - index, argv + index);
}

static int runTypecheckCommand(VM* vm, const char* exe, int argc, const char** argv, int startIndex) {
  int index = startIndex;
  while (index < argc) {
    if (isFlag(argv[index], "--module-path", "-M")) {
      if (index + 1 >= argc) {
        fprintf(stderr, "Missing value for --module-path.\n");
        printHelp(exe);
        return 64;
      }
      vmAddModulePath(vm, argv[index + 1]);
      index += 2;
      continue;
    }
    break;
  }

  if (index >= argc || isFlag(argv[index], "--help", "-h")) {
    printHelp(exe);
    return index >= argc ? 64 : 0;
  }

  const char* path = argv[index++];
  if (index < argc) {
    fprintf(stderr, "Unexpected extra arguments for 'typecheck'.\n");
    printHelp(exe);
    return 64;
  }
  return typecheckFile(vm, path);
}

int main(int argc, const char** argv) {
  const char* exe = exeName(argv[0]);
  if (argc > 1 && isFlag(argv[1], "--help", "-h")) {
    printHelp(exe);
    return 0;
  }
  if (argc > 1 && isFlag(argv[1], "--version", "-v")) {
    printVersion();
    return 0;
  }
  if (argc > 1 && (strcmp(argv[1], "fmt") == 0 || strcmp(argv[1], "format") == 0)) {
    return runFormatCommand(exe, argc, argv);
  }
  if (argc > 1 && strcmp(argv[1], "lint") == 0) {
    return runLintCommand(exe, argc, argv);
  }
  if (argc > 1 && strcmp(argv[1], "pkg") == 0) {
    return runPackageCommand(exe, argc, argv);
  }

  VM vm;
  vmInit(&vm);

  int result = 0;
  if (argc == 1) {
    repl(&vm);
  } else if (strcmp(argv[1], "repl") == 0) {
    if (argc > 2) {
      fprintf(stderr, "Unexpected arguments for 'repl'.\n");
      printHelp(exe);
      result = 64;
    } else {
      repl(&vm);
    }
  } else if (strcmp(argv[1], "typecheck") == 0) {
    result = runTypecheckCommand(&vm, exe, argc, argv, 2);
  } else if (strcmp(argv[1], "run") == 0) {
    result = runFileCommand(&vm, exe, argc, argv, 2);
  } else {
    int index = 1;
    while (index < argc && isDebugFlag(argv[index])) {
      index++;
    }
    if (index < argc && strcmp(argv[index], "run") == 0) {
      result = runFileCommand(&vm, exe, argc, argv, index + 1);
    } else if (argv[1][0] == '-' && argv[1][1] != '\0') {
      fprintf(stderr, "Unknown option: %s\n", argv[1]);
      printHelp(exe);
      result = 64;
    } else {
      result = runFileCommand(&vm, exe, argc, argv, 1);
    }
  }

  vmFree(&vm);
  return result;
}

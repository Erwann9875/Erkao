#include "lexer.h"
#include "parser.h"
#include "interpreter.h"
#include "program.h"

#include <stdio.h>
#include <string.h>

#ifndef ERKAO_VERSION
#define ERKAO_VERSION "dev"
#endif

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

static void freeStatements(StmtArray* statements) {
  for (int i = 0; i < statements->count; i++) {
    freeStmt(statements->items[i]);
  }
  freeStmtArray(statements);
}

static bool runSource(VM* vm, const char* path, char* source) {
  bool lexError = false;
  TokenArray tokens = scanTokens(source, &lexError);
  if (lexError) {
    freeTokenArray(&tokens);
    free(source);
    return false;
  }

  StmtArray statements;
  bool parseOk = parseTokens(&tokens, source, &statements);
  freeTokenArray(&tokens);
  if (!parseOk) {
    freeStatements(&statements);
    free(source);
    return false;
  }

  Program* program = programCreate(vm, source, path, statements);
  return interpret(vm, program);
}

static int runFile(VM* vm, const char* path, int argc, const char** argv) {
  char* source = readFile(path);
  if (!source) return 74;

  vmSetArgs(vm, argc, argv);
  bool ok = runSource(vm, path, source);

  return ok ? 0 : 65;
}

static void repl(VM* vm) {
  char line[1024];
  vmSetArgs(vm, 0, NULL);
  for (;;) {
    printf("> ");
    if (!fgets(line, sizeof(line), stdin)) {
      printf("\n");
      break;
    }
    size_t length = strlen(line);
    char* copy = (char*)malloc(length + 1);
    if (!copy) {
      fprintf(stderr, "Out of memory.\n");
      break;
    }
    memcpy(copy, line, length + 1);
    runSource(vm, NULL, copy);
    vm->hadError = false;
  }
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

static void printHelp(const char* exe) {
  fprintf(stdout,
          "Usage:\n"
          "  %s [--help|-h] [--version|-v]\n"
          "  %s repl\n"
          "  %s run <file> [-- args...]\n"
          "  %s <file> [args...]\n"
          "\n"
          "Commands:\n"
          "  run   Run a script file.\n"
          "  repl  Start the interactive REPL.\n"
          "\n"
          "Options:\n"
          "  -h, --help     Show this help.\n"
          "  -v, --version  Show the version.\n",
          exe, exe, exe, exe);
}

static void printVersion(void) {
  fprintf(stdout, "Erkao %s\n", ERKAO_VERSION);
}

static int runWithArgs(VM* vm, const char* path, int argc, const char** argv) {
  int argStart = 0;
  if (argc > 0 && strcmp(argv[0], "--") == 0) {
    argStart = 1;
  }
  return runFile(vm, path, argc - argStart, argv + argStart);
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
  } else if (strcmp(argv[1], "run") == 0) {
    if (argc < 3 || isFlag(argv[2], "--help", "-h")) {
      printHelp(exe);
      result = (argc < 3) ? 64 : 0;
    } else {
      result = runWithArgs(&vm, argv[2], argc - 3, argv + 3);
    }
  } else {
    if (argv[1][0] == '-' && argv[1][1] != '\0') {
      fprintf(stderr, "Unknown option: %s\n", argv[1]);
      printHelp(exe);
      result = 64;
    } else {
      result = runWithArgs(&vm, argv[1], argc - 2, argv + 2);
    }
  }

  vmFree(&vm);
  return result;
}

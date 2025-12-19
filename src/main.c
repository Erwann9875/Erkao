#include "lexer.h"
#include "parser.h"
#include "interpreter.h"
#include "program.h"

#include <stdio.h>

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

static bool runSource(VM* vm, char* source) {
  bool lexError = false;
  TokenArray tokens = scanTokens(source, &lexError);
  if (lexError) {
    freeTokenArray(&tokens);
    free(source);
    return false;
  }

  StmtArray statements;
  bool parseOk = parseTokens(&tokens, &statements);
  freeTokenArray(&tokens);
  if (!parseOk) {
    freeStatements(&statements);
    free(source);
    return false;
  }

  Program* program = programCreate(vm, source, statements);
  return interpret(vm, program);
}

static int runFile(VM* vm, const char* path, int argc, const char** argv) {
  char* source = readFile(path);
  if (!source) return 74;

  vmSetArgs(vm, argc, argv);
  bool ok = runSource(vm, source);

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
    runSource(vm, copy);
    vm->hadError = false;
  }
}

int main(int argc, const char** argv) {
  VM vm;
  vmInit(&vm);

  int result = 0;
  if (argc == 1) {
    repl(&vm);
  } else {
    result = runFile(&vm, argv[1], argc - 2, argv + 2);
  }

  vmFree(&vm);
  return result;
}

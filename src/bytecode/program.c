#include "program.h"
#include "interpreter.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void programUnlink(VM* vm, Program* program) {
  Program* previous = NULL;
  Program* current = vm->programs;
  while (current) {
    if (current == program) {
      if (previous) {
        previous->next = current->next;
      } else {
        vm->programs = current->next;
      }
      current->next = NULL;
      return;
    }
    previous = current;
    current = current->next;
  }
}

static void programFree(VM* vm, Program* program) {
  if (!program) return;
  programUnlink(vm, program);
  free(program->source);
  free(program->path);
  free(program);
}

Program* programCreate(VM* vm, char* source, const char* path, ObjFunction* function) {
  Program* program = (Program*)malloc(sizeof(Program));
  if (!program) {
    if (vm) vm->hadError = true;
    fprintf(stderr, "RuntimeError: Out of memory while creating program.\n");
    return NULL;
  }
  program->source = source;
  if (path) {
    size_t length = strlen(path);
    program->path = (char*)malloc(length + 1);
    if (!program->path) {
      if (vm) vm->hadError = true;
      fprintf(stderr, "RuntimeError: Out of memory while copying program path.\n");
      free(program);
      return NULL;
    }
    memcpy(program->path, path, length + 1);
  } else {
    program->path = NULL;
  }
  program->function = function;
  program->refCount = 0;
  program->running = 0;
  program->next = vm->programs;
  vm->programs = program;
  return program;
}

void programRetain(Program* program) {
  if (!program) return;
  program->refCount++;
}

void programRelease(VM* vm, Program* program) {
  if (!program) return;
  if (program->refCount > 0) {
    program->refCount--;
  }
  if (program->refCount == 0 && program->running == 0) {
    programFree(vm, program);
  }
}

void programRunBegin(Program* program) {
  if (!program) return;
  program->running++;
}

void programRunEnd(VM* vm, Program* program) {
  if (!program) return;
  if (program->running > 0) {
    program->running--;
  }
  if (program->refCount == 0 && program->running == 0) {
    programFree(vm, program);
  }
}

void programFreeAll(VM* vm) {
  Program* current = vm->programs;
  while (current) {
    Program* next = current->next;
    free(current->source);
    free(current->path);
    free(current);
    current = next;
  }
  vm->programs = NULL;
}

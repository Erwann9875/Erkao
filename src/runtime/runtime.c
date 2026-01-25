#include "interpreter_internal.h"
#include "chunk.h"
#include "program.h"

#include <ctype.h>

#ifdef ERKAO_FUZZING
static void printStackTrace(VM* vm, const char* fallbackPath) {
  (void)vm;
  (void)fallbackPath;
}
#else
static const char* frameFunctionName(const CallFrame* frame) {
  if (!frame || !frame->function) return "<unknown>";
  if (frame->function->name && frame->function->name->chars) {
    return frame->function->name->chars;
  }
  if (frame->isModule) return "<module>";
  return "<script>";
}

static const char* framePath(const CallFrame* frame, const char* fallback) {
  if (frame && frame->function && frame->function->program && frame->function->program->path) {
    return frame->function->program->path;
  }
  return fallback ? fallback : "<repl>";
}

static Token frameToken(const CallFrame* frame) {
  Token token;
  memset(&token, 0, sizeof(Token));
  if (!frame || !frame->function || !frame->function->chunk) return token;

  Chunk* chunk = frame->function->chunk;
  if (!chunk->code || !chunk->tokens || chunk->count <= 0) return token;

  size_t offset = 0;
  if (frame->ip > chunk->code) {
    offset = (size_t)(frame->ip - chunk->code - 1);
  }
  if (offset < (size_t)chunk->count) {
    token = chunk->tokens[offset];
  }
  return token;
}

static void printStackTrace(VM* vm, const char* fallbackPath) {
  if (!vm || vm->frameCount <= 0) return;

  fprintf(stderr, "Stack trace (most recent call last):\n");
  int depth = 0;
  for (int i = vm->frameCount - 1; i >= 0; i--, depth++) {
    CallFrame* frame = &vm->frames[i];
    const char* name = frameFunctionName(frame);
    const char* path = framePath(frame, fallbackPath);
    Token token = frameToken(frame);
    if (token.line > 0 && token.column > 0) {
      fprintf(stderr, "  #%d %s (%s:%d:%d)", depth, name, path, token.line, token.column);
    } else {
      fprintf(stderr, "  #%d %s (%s)", depth, name, path);
    }
    if (token.length > 0) {
      fprintf(stderr, " -> '%.*s'\n", token.length, token.start);
    } else {
      fputc('\n', stderr);
    }
  }
}
#endif

static bool stackTraceEnabled(void) {
  const char* value = getenv("ERKAO_STACK_TRACE");
  if (!value || value[0] == '\0') return true;

  char lower[6];
  size_t i = 0;
  while (value[i] && i < sizeof(lower) - 1) {
    lower[i] = (char)tolower((unsigned char)value[i]);
    i++;
  }
  lower[i] = '\0';

  if (strcmp(lower, "0") == 0 || strcmp(lower, "no") == 0 ||
      strcmp(lower, "off") == 0 || strcmp(lower, "false") == 0) {
    return false;
  }

  return true;
}

ObjArray* captureStackTrace(VM* vm, const char* fallbackPath) {
  ObjArray* trace = newArray(vm);
#ifdef ERKAO_FUZZING
  (void)vm;
  (void)fallbackPath;
  return trace;
#else
  if (!vm || vm->frameCount <= 0) return trace;
  for (int i = vm->frameCount - 1, depth = 0; i >= 0; i--, depth++) {
    CallFrame* frame = &vm->frames[i];
    const char* name = frameFunctionName(frame);
    const char* path = framePath(frame, fallbackPath);
    Token token = frameToken(frame);
    char buffer[256];
    if (token.line > 0 && token.column > 0) {
      if (token.length > 0) {
        snprintf(buffer, sizeof(buffer), "#%d %s (%s:%d:%d) -> '%.*s'",
                 depth, name, path, token.line, token.column,
                 token.length, token.start);
      } else {
        snprintf(buffer, sizeof(buffer), "#%d %s (%s:%d:%d)",
                 depth, name, path, token.line, token.column);
      }
    } else {
      snprintf(buffer, sizeof(buffer), "#%d %s (%s)", depth, name, path);
    }
    arrayWrite(trace, OBJ_VAL(copyString(vm, buffer)));
  }
  return trace;
#endif
}

#ifdef _WIN32
#define ERKAO_FFI_EXPORT __declspec(dllexport)
#else
#define ERKAO_FFI_EXPORT __attribute__((visibility("default")))
#endif

ERKAO_FFI_EXPORT double erkao_ffi_add(double a, double b) {
  return a + b;
}

void runtimeError(VM* vm, Token token, const char* message) {
  const char* displayPath = "<repl>";
  if (vm->currentProgram && vm->currentProgram->path) {
    displayPath = vm->currentProgram->path;
  }

#ifdef ERKAO_FUZZING
  (void)token;
  (void)message;
#else
  if (token.line > 0 && token.column > 0) {
    fprintf(stderr, "%s:%d:%d: RuntimeError", displayPath, token.line, token.column);
    if (token.length > 0) {
      fprintf(stderr, " at '%.*s'", token.length, token.start);
    }
    fprintf(stderr, ": %s\n", message);
    if (vm->currentProgram) {
      int length = token.length > 0 ? token.length : 1;
      printErrorContext(vm->currentProgram->source, token.line, token.column, length);
    }
  } else {
    fprintf(stderr, "%s: RuntimeError: %s\n", displayPath, message);
  }
#endif
  if (stackTraceEnabled()) {
    printStackTrace(vm, displayPath);
  }
  vm->hadError = true;
}

bool isTruthy(Value value) {
  if (IS_NULL(value)) return false;
  if (IS_BOOL(value)) return AS_BOOL(value);
  return true;
}

bool isString(Value value) {
  return isObjType(value, OBJ_STRING);
}

ObjString* asString(Value value) {
  return (ObjString*)AS_OBJ(value);
}

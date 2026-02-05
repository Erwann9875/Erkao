#include "stdlib_internal.h"

#include <ctype.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <process.h>
#else
#include <errno.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

static bool procLooksLikeProgramPath(const char* text) {
  if (!text || text[0] == '\0') return false;
  for (const char* c = text; *c; c++) {
    unsigned char ch = (unsigned char)*c;
    if (isspace(ch)) return false;
    if (ch == '|' || ch == '&' || ch == ';' || ch == '<' || ch == '>' ||
        ch == '$' || ch == '`' || ch == '(' || ch == ')' || ch == '*' ||
        ch == '?' || ch == '!') {
      return false;
    }
  }
  return true;
}

static Value nativeProcRun(VM* vm, int argc, Value* args) {
  if (!stdlibUnsafeEnabled(vm, ERKAO_UNSAFE_PROC, "ERKAO_ALLOW_PROC")) {
    return runtimeErrorValue(vm,
                             "proc.run is disabled. Use --allow-unsafe=proc or set ERKAO_ALLOW_PROC=1.");
  }
  if (argc < 1 || argc > 2) {
    return runtimeErrorValue(vm, "proc.run expects (program[, args]).");
  }
  if (!isObjType(args[0], OBJ_STRING)) {
    return runtimeErrorValue(vm, "proc.run expects (program[, args]).");
  }
  ObjString* program = (ObjString*)AS_OBJ(args[0]);
  ObjArray* procArgs = NULL;

  if (argc >= 2) {
    if (!isObjType(args[1], OBJ_ARRAY)) {
      return runtimeErrorValue(vm, "proc.run expects args to be an array of strings.");
    }
    procArgs = (ObjArray*)AS_OBJ(args[1]);
  } else if (!procLooksLikeProgramPath(program->chars)) {
    return runtimeErrorValue(vm,
                             "proc.run executes a program directly (no shell). "
                             "Pass arguments using an array.");
  }

  int extra = procArgs ? procArgs->count : 0;
  char** argv = (char**)erkaoAllocArray((size_t)extra + 2, sizeof(char*));
  if (!argv) {
    return runtimeErrorValue(vm, "proc.run out of memory.");
  }
  memset(argv, 0, sizeof(char*) * ((size_t)extra + 2));
  argv[0] = program->chars;
  for (int i = 0; i < extra; i++) {
    if (!isObjType(procArgs->items[i], OBJ_STRING)) {
      free(argv);
      return runtimeErrorValue(vm, "proc.run expects args to be an array of strings.");
    }
    ObjString* part = (ObjString*)AS_OBJ(procArgs->items[i]);
    argv[i + 1] = part->chars;
  }
  argv[extra + 1] = NULL;

#ifdef _WIN32
  intptr_t result = _spawnvp(_P_WAIT, argv[0], (const char* const*)argv);
  free(argv);
  if (result == -1) {
    return runtimeErrorValue(vm, "proc.run failed to execute program.");
  }
  if (result > INT_MAX) {
    return runtimeErrorValue(vm, "proc.run exit code overflow.");
  }
  return NUMBER_VAL((double)result);
#else
  pid_t pid = fork();
  if (pid < 0) {
    free(argv);
    return runtimeErrorValue(vm, "proc.run failed to fork process.");
  }
  if (pid == 0) {
    execvp(argv[0], argv);
    _exit(127);
  }

  int status = 0;
  for (;;) {
    pid_t waited = waitpid(pid, &status, 0);
    if (waited == pid) break;
    if (waited < 0 && errno == EINTR) continue;
    free(argv);
    return runtimeErrorValue(vm, "proc.run failed while waiting for process.");
  }
  free(argv);

  if (WIFEXITED(status)) {
    return NUMBER_VAL((double)WEXITSTATUS(status));
  }
  if (WIFSIGNALED(status)) {
    return NUMBER_VAL((double)(128 + WTERMSIG(status)));
  }
  return NUMBER_VAL((double)status);
#endif
}


void stdlib_register_proc(VM* vm, ObjInstance* module) {
  moduleAdd(vm, module, "run", nativeProcRun, -1);
}

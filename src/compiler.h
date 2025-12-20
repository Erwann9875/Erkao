#ifndef ERKAO_COMPILER_H
#define ERKAO_COMPILER_H

#include "interpreter.h"
#include "program.h"

ObjFunction* compileProgram(VM* vm, Program* program);

#endif

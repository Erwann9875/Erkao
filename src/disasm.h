#ifndef ERKAO_DISASM_H
#define ERKAO_DISASM_H

#include "chunk.h"

void disassembleChunk(const Chunk* chunk, const char* name);
void disassembleFunction(const ObjFunction* function);

#endif

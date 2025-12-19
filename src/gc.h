#ifndef ERKAO_GC_H
#define ERKAO_GC_H

#include "interpreter.h"

void gcTrackAlloc(VM* vm);
void gcMaybe(VM* vm);
void gcCollect(VM* vm);
void freeObject(VM* vm, Obj* object);

#endif

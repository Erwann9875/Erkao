#ifndef ERKAO_GC_INTERNAL_H
#define ERKAO_GC_INTERNAL_H

#include "gc.h"

size_t gcTotalHeapBytes(const VM* vm);
void updateYoungNext(VM* vm);
void updateFullNext(VM* vm);

void markRoots(VM* vm);
void markYoungRoots(VM* vm);
void traceFull(VM* vm);
void traceYoung(VM* vm);
void blackenYoungObject(VM* vm, Obj* object);
bool gcObjectHasYoungRefs(Obj* object);

void sweepYoung(VM* vm, bool fullGc);
bool sweepOldStep(VM* vm, size_t budget);
void gcCollectYoung(VM* vm);

#endif

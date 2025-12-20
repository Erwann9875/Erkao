#ifndef ERKAO_GC_H
#define ERKAO_GC_H

#include "interpreter.h"

#define GC_MIN_HEAP_BYTES (4 * 1024 * 1024)
#define GC_MIN_YOUNG_HEAP_BYTES (1024 * 1024)
#define GC_HEAP_GROW_FACTOR 3
#define GC_YOUNG_GROW_FACTOR 3
#define GC_SWEEP_BATCH 256
#define GC_PROMOTION_AGE 2

void gcTrackAlloc(VM* vm, Obj* object);
void gcTrackResize(VM* vm, Obj* object, size_t oldSize, size_t newSize);
void gcTrackEnvAlloc(VM* vm, size_t size);
void gcWriteBarrier(VM* vm, Obj* owner, Value value);
void gcRememberObjectIfYoungRefs(VM* vm, Obj* object);
void gcMaybe(VM* vm);
void gcCollect(VM* vm);
void freeObject(VM* vm, Obj* object);

#endif

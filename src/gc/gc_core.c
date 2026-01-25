#include "gc_internal.h"

#include <stdio.h>
#include <time.h>

size_t gcTotalHeapBytes(const VM* vm) {
  return vm->gcYoungBytes + vm->gcOldBytes + vm->gcEnvBytes;
}

void updateYoungNext(VM* vm) {
  size_t next = vm->gcYoungBytes * GC_YOUNG_GROW_FACTOR;
  if (next < GC_MIN_YOUNG_HEAP_BYTES) {
    next = GC_MIN_YOUNG_HEAP_BYTES;
  }
  vm->gcYoungNext = next;
}

void updateFullNext(VM* vm) {
  size_t total = gcTotalHeapBytes(vm);
  size_t next = total * GC_HEAP_GROW_FACTOR;
  if (next < GC_MIN_HEAP_BYTES) {
    next = GC_MIN_HEAP_BYTES;
  }
  vm->gcNext = next;
}

void gcTrackAlloc(VM* vm, Obj* object) {
  if (!vm || !object) return;

  if (object->generation == OBJ_GEN_OLD) {
    vm->gcOldBytes += object->size;
  } else {
    vm->gcYoungBytes += object->size;
    if (!vm->gcPendingYoung && vm->gcYoungBytes > vm->gcYoungNext) {
      if (vm->gcLog) {
        fprintf(stderr, "[gc] young threshold reached: bytes=%zu next=%zu\n",
                vm->gcYoungBytes, vm->gcYoungNext);
      }
      vm->gcPendingYoung = true;
    }
  }

  size_t total = gcTotalHeapBytes(vm);
  if (!vm->gcPendingFull && total > vm->gcNext) {
    if (vm->gcLog) {
      fprintf(stderr, "[gc] full threshold reached: bytes=%zu next=%zu\n",
              total, vm->gcNext);
    }
    vm->gcPendingFull = true;
  }
}

void gcTrackResize(VM* vm, Obj* object, size_t oldSize, size_t newSize) {
  if (!vm || !object || oldSize == newSize) return;

  if (newSize > oldSize) {
    size_t delta = newSize - oldSize;
    if (object->generation == OBJ_GEN_OLD) {
      vm->gcOldBytes += delta;
    } else {
      vm->gcYoungBytes += delta;
      if (!vm->gcPendingYoung && vm->gcYoungBytes > vm->gcYoungNext) {
        if (vm->gcLog) {
          fprintf(stderr, "[gc] young threshold reached: bytes=%zu next=%zu\n",
                  vm->gcYoungBytes, vm->gcYoungNext);
        }
        vm->gcPendingYoung = true;
      }
    }
  } else {
    size_t delta = oldSize - newSize;
    if (object->generation == OBJ_GEN_OLD) {
      if (vm->gcOldBytes > delta) vm->gcOldBytes -= delta;
    } else {
      if (vm->gcYoungBytes > delta) vm->gcYoungBytes -= delta;
    }
  }

  size_t total = gcTotalHeapBytes(vm);
  if (!vm->gcPendingFull && total > vm->gcNext) {
    if (vm->gcLog) {
      fprintf(stderr, "[gc] full threshold reached: bytes=%zu next=%zu\n",
              total, vm->gcNext);
    }
    vm->gcPendingFull = true;
  }
}

void gcTrackEnvAlloc(VM* vm, size_t size) {
  if (!vm) return;
  vm->gcEnvBytes += size;

  size_t total = gcTotalHeapBytes(vm);
  if (!vm->gcPendingFull && total > vm->gcNext) {
    if (vm->gcLog) {
      fprintf(stderr, "[gc] full threshold reached: bytes=%zu next=%zu\n",
              total, vm->gcNext);
    }
    vm->gcPendingFull = true;
  }
}

static void rememberObject(VM* vm, Obj* object) {
  if (!vm || !object) return;
  if (object->generation != OBJ_GEN_OLD) return;
  if (object->remembered) return;

  if (vm->gcRememberedCapacity < vm->gcRememberedCount + 1) {
    size_t oldCapacity = vm->gcRememberedCapacity;
    vm->gcRememberedCapacity = GROW_CAPACITY(oldCapacity);
    vm->gcRemembered = GROW_ARRAY(Obj*, vm->gcRemembered, oldCapacity,
                                  vm->gcRememberedCapacity);
  }

  object->remembered = true;
  vm->gcRemembered[vm->gcRememberedCount++] = object;
}

static void rebuildRemembered(VM* vm) {
  if (!vm) return;
  vm->gcRememberedCount = 0;
  for (Obj* object = vm->oldObjects; object; object = object->next) {
    object->remembered = false;
    if (gcObjectHasYoungRefs(object)) {
      rememberObject(vm, object);
    }
  }
}

void gcWriteBarrier(VM* vm, Obj* owner, Value value) {
  if (!vm || !owner) return;
  if (owner->generation != OBJ_GEN_OLD) return;
  if (!IS_OBJ(value)) return;

  Obj* child = AS_OBJ(value);
  if (child->generation != OBJ_GEN_YOUNG) return;
  rememberObject(vm, owner);
}

void gcRememberObjectIfYoungRefs(VM* vm, Obj* object) {
  if (!vm || !object) return;
  if (object->generation != OBJ_GEN_OLD) return;
  if (gcObjectHasYoungRefs(object)) {
    rememberObject(vm, object);
  }
}

static void finishFullSweep(VM* vm) {
  rebuildRemembered(vm);
  vm->gcSweeping = false;
  updateFullNext(vm);

  if (vm->gcLog && vm->gcLogFullActive) {
    clock_t end = clock();
    size_t afterYoung = vm->gcYoungBytes;
    size_t afterOld = vm->gcOldBytes;
    size_t afterEnv = vm->gcEnvBytes;
    double ms = (double)(end - vm->gcLogStart) * 1000.0 / (double)CLOCKS_PER_SEC;
    fprintf(stderr,
            "[gc] full end: young=%zu->%zu old=%zu->%zu env=%zu->%zu next=%zu time=%.2fms\n",
            vm->gcLogBeforeYoung, afterYoung,
            vm->gcLogBeforeOld, afterOld,
            vm->gcLogBeforeEnv, afterEnv,
            vm->gcNext, ms);
    vm->gcLogFullActive = false;
  }
}

void gcMaybe(VM* vm) {
  if (!vm) return;

  if (vm->gcSweeping) {
    if (sweepOldStep(vm, GC_SWEEP_BATCH)) {
      finishFullSweep(vm);
    }
    return;
  }

  if (vm->gcPendingFull) {
    gcCollect(vm);
    return;
  }

  if (vm->gcPendingYoung) {
    gcCollectYoung(vm);
  }
}

void gcCollect(VM* vm) {
  if (!vm) return;
  vm->gcPendingFull = false;
  vm->gcPendingYoung = false;
  vm->gcGrayObjectCount = 0;
  vm->gcGrayEnvCount = 0;

  if (vm->gcLog) {
    vm->gcLogBeforeYoung = vm->gcYoungBytes;
    vm->gcLogBeforeOld = vm->gcOldBytes;
    vm->gcLogBeforeEnv = vm->gcEnvBytes;
    vm->gcLogStart = clock();
    vm->gcLogFullActive = true;
    fprintf(stderr, "[gc] full begin: young=%zu old=%zu env=%zu total=%zu next=%zu\n",
            vm->gcYoungBytes, vm->gcOldBytes, vm->gcEnvBytes,
            gcTotalHeapBytes(vm), vm->gcNext);
  }

  markRoots(vm);
  traceFull(vm);
  sweepYoung(vm, true);
  updateYoungNext(vm);

  vm->gcSweepOld = &vm->oldObjects;
  vm->gcSweepEnv = &vm->envs;
  vm->gcSweeping = true;

  while (!sweepOldStep(vm, GC_SWEEP_BATCH)) {
    // Sweep old objects/envs to completion in this collection cycle.
  }
  finishFullSweep(vm);
}

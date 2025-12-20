#include "gc_internal.h"

#include <stdio.h>

static void pruneRemembered(VM* vm) {
  if (!vm || vm->gcRememberedCount == 0) return;

  size_t write = 0;
  for (size_t i = 0; i < vm->gcRememberedCount; i++) {
    Obj* object = vm->gcRemembered[i];
    if (!object || object->generation != OBJ_GEN_OLD) continue;
    if (gcObjectHasYoungRefs(object)) {
      object->remembered = true;
      vm->gcRemembered[write++] = object;
    } else {
      object->remembered = false;
    }
  }
  vm->gcRememberedCount = write;
}

void gcCollectYoung(VM* vm) {
  if (!vm) return;
  vm->gcPendingYoung = false;
  vm->gcGrayObjectCount = 0;
  vm->gcGrayEnvCount = 0;

  size_t beforeYoung = vm->gcYoungBytes;
  if (vm->gcLog) {
    fprintf(stderr, "[gc] minor begin: young=%zu old=%zu env=%zu nextY=%zu\n",
            vm->gcYoungBytes, vm->gcOldBytes, vm->gcEnvBytes, vm->gcYoungNext);
  }

  markYoungRoots(vm);

  for (size_t i = 0; i < vm->gcRememberedCount; i++) {
    Obj* object = vm->gcRemembered[i];
    if (object && object->generation == OBJ_GEN_OLD) {
      blackenYoungObject(vm, object);
    }
  }

  traceYoung(vm);
  sweepYoung(vm, false);
  pruneRemembered(vm);
  updateYoungNext(vm);

  if (vm->gcLog) {
    fprintf(stderr, "[gc] minor end: young=%zu->%zu old=%zu env=%zu nextY=%zu\n",
            beforeYoung, vm->gcYoungBytes, vm->gcOldBytes, vm->gcEnvBytes, vm->gcYoungNext);
  }

  if (!vm->gcPendingFull && gcTotalHeapBytes(vm) > vm->gcNext) {
    vm->gcPendingFull = true;
  }
}

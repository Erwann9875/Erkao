#include "gc_internal.h"

#include <stdio.h>

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

  for (Obj* object = vm->oldObjects; object; object = object->next) {
    blackenYoungObject(vm, object);
  }

  traceYoung(vm);
  sweepYoung(vm, false);
  updateYoungNext(vm);

  if (vm->gcLog) {
    fprintf(stderr, "[gc] minor end: young=%zu->%zu old=%zu env=%zu nextY=%zu\n",
            beforeYoung, vm->gcYoungBytes, vm->gcOldBytes, vm->gcEnvBytes, vm->gcYoungNext);
  }

  if (!vm->gcPendingFull && gcTotalHeapBytes(vm) > vm->gcNext) {
    vm->gcPendingFull = true;
  }
}

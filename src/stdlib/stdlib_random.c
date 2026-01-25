#include "stdlib_internal.h"

#include <math.h>

static uint64_t gRandomState = 0;
static bool gRandomSeeded = false;
static bool gRandomHasSpare = false;
static double gRandomSpare = 0.0;

static void randomSeedIfNeeded(void) {
  if (gRandomSeeded) return;
  uint64_t seed = (uint64_t)time(NULL);
  seed ^= (uint64_t)clock() << 32;
  if (seed == 0) {
    seed = 0x9e3779b97f4a7c15ULL;
  }
  gRandomState = seed;
  gRandomSeeded = true;
}

static uint64_t randomNext(void) {
  randomSeedIfNeeded();
  uint64_t x = gRandomState;
  x ^= x >> 12;
  x ^= x << 25;
  x ^= x >> 27;
  gRandomState = x;
  return x * 2685821657736338717ULL;
}

static double randomNextDouble(void) {
  uint64_t value = randomNext();
  return (double)(value >> 11) * (1.0 / 9007199254740992.0);
}

static double randomNextNormal(void) {
  if (gRandomHasSpare) {
    gRandomHasSpare = false;
    return gRandomSpare;
  }

  double u = 0.0;
  double v = 0.0;
  double s = 0.0;
  do {
    u = randomNextDouble() * 2.0 - 1.0;
    v = randomNextDouble() * 2.0 - 1.0;
    s = u * u + v * v;
  } while (s <= 0.0 || s >= 1.0);

  double factor = sqrt(-2.0 * log(s) / s);
  gRandomSpare = v * factor;
  gRandomHasSpare = true;
  return u * factor;
}

static Value nativeRandomSeed(VM* vm, int argc, Value* args) {
  (void)argc;
  if (!IS_NUMBER(args[0])) {
    return runtimeErrorValue(vm, "random.seed expects a number.");
  }
  int64_t seed = (int64_t)AS_NUMBER(args[0]);
  if (seed == 0) {
    seed = (int64_t)0x9e3779b97f4a7c15ULL;
  }
  gRandomState = (uint64_t)seed;
  gRandomSeeded = true;
  gRandomHasSpare = false;
  return NULL_VAL;
}

static Value nativeRandomInt(VM* vm, int argc, Value* args) {
  if (argc < 1) {
    return runtimeErrorValue(vm, "random.int expects (max) or (min, max).");
  }
  if (!IS_NUMBER(args[0])) {
    return runtimeErrorValue(vm, "random.int expects numeric bounds.");
  }

  if (argc == 1) {
    int max = (int)AS_NUMBER(args[0]);
    if (max <= 0) {
      return runtimeErrorValue(vm, "random.int expects max > 0.");
    }
    uint64_t value = randomNext();
    return NUMBER_VAL((double)(value % (uint64_t)max));
  }

  if (!IS_NUMBER(args[1])) {
    return runtimeErrorValue(vm, "random.int expects numeric bounds.");
  }
  int min = (int)AS_NUMBER(args[0]);
  int max = (int)AS_NUMBER(args[1]);
  if (max <= min) {
    return runtimeErrorValue(vm, "random.int expects max > min.");
  }
  uint64_t span = (uint64_t)(max - min);
  uint64_t value = randomNext() % span;
  return NUMBER_VAL((double)(min + (int)value));
}

static Value nativeRandomFloat(VM* vm, int argc, Value* args) {
  if (argc == 0) {
    return NUMBER_VAL(randomNextDouble());
  }
  if (!IS_NUMBER(args[0])) {
    return runtimeErrorValue(vm, "random.float expects numeric bounds.");
  }
  double min = 0.0;
  double max = AS_NUMBER(args[0]);
  if (argc >= 2) {
    if (!IS_NUMBER(args[1])) {
      return runtimeErrorValue(vm, "random.float expects numeric bounds.");
    }
    min = AS_NUMBER(args[0]);
    max = AS_NUMBER(args[1]);
  }
  if (max <= min) {
    return runtimeErrorValue(vm, "random.float expects max > min.");
  }
  double unit = randomNextDouble();
  return NUMBER_VAL(min + unit * (max - min));
}

static Value nativeRandomChoice(VM* vm, int argc, Value* args) {
  (void)argc;
  if (!isObjType(args[0], OBJ_ARRAY)) {
    return runtimeErrorValue(vm, "random.choice expects an array.");
  }
  ObjArray* array = (ObjArray*)AS_OBJ(args[0]);
  if (array->count <= 0) {
    return runtimeErrorValue(vm, "random.choice expects a non-empty array.");
  }
  uint64_t index = randomNext() % (uint64_t)array->count;
  return array->items[(int)index];
}

static Value nativeRandomNormal(VM* vm, int argc, Value* args) {
  if (argc < 2) {
    return runtimeErrorValue(vm, "random.normal expects (mean, stddev).");
  }
  if (!IS_NUMBER(args[0]) || !IS_NUMBER(args[1])) {
    return runtimeErrorValue(vm, "random.normal expects numeric bounds.");
  }
  double mean = AS_NUMBER(args[0]);
  double stddev = AS_NUMBER(args[1]);
  if (stddev < 0.0) {
    return runtimeErrorValue(vm, "random.normal expects stddev >= 0.");
  }
  return NUMBER_VAL(mean + randomNextNormal() * stddev);
}

static Value nativeRandomGaussian(VM* vm, int argc, Value* args) {
  return nativeRandomNormal(vm, argc, args);
}

static Value nativeRandomExponential(VM* vm, int argc, Value* args) {
  if (argc < 1) {
    return runtimeErrorValue(vm, "random.exponential expects (lambda).");
  }
  if (!IS_NUMBER(args[0])) {
    return runtimeErrorValue(vm, "random.exponential expects a number.");
  }
  double lambda = AS_NUMBER(args[0]);
  if (lambda <= 0.0) {
    return runtimeErrorValue(vm, "random.exponential expects lambda > 0.");
  }
  double u = randomNextDouble();
  if (u <= 0.0) {
    u = 1e-12;
  }
  return NUMBER_VAL(-log(1.0 - u) / lambda);
}

static Value nativeRandomUniform(VM* vm, int argc, Value* args) {
  return nativeRandomFloat(vm, argc, args);
}


void stdlib_register_random(VM* vm, ObjInstance* module) {
  moduleAdd(vm, module, "seed", nativeRandomSeed, 1);
  moduleAdd(vm, module, "int", nativeRandomInt, -1);
  moduleAdd(vm, module, "float", nativeRandomFloat, -1);
  moduleAdd(vm, module, "choice", nativeRandomChoice, 1);
  moduleAdd(vm, module, "normal", nativeRandomNormal, 2);
  moduleAdd(vm, module, "gaussian", nativeRandomGaussian, 2);
  moduleAdd(vm, module, "exponential", nativeRandomExponential, 1);
  moduleAdd(vm, module, "uniform", nativeRandomUniform, -1);
}

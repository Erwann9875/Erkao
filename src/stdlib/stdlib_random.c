#include "stdlib_internal.h"

#include <errno.h>
#include <float.h>
#include <limits.h>
#include <math.h>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <bcrypt.h>
#endif
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/random.h>
#endif
#endif

static uint64_t gRandomState = 0;
static bool gRandomSeeded = false;
static bool gRandomDeterministic = false;
static bool gRandomHasSpare = false;
static double gRandomSpare = 0.0;

static void randomSeedFallbackIfNeeded(void) {
  if (gRandomSeeded) return;
  uint64_t seed = (uint64_t)time(NULL);
  seed ^= (uint64_t)clock() << 32;
  if (seed == 0) {
    seed = 0x9e3779b97f4a7c15ULL;
  }
  gRandomState = seed;
  gRandomSeeded = true;
}

static uint64_t randomNextDeterministic(void) {
  randomSeedFallbackIfNeeded();
  uint64_t x = gRandomState;
  x ^= x >> 12;
  x ^= x << 25;
  x ^= x >> 27;
  gRandomState = x;
  return x * 2685821657736338717ULL;
}

static bool randomFillFromOs(void* buffer, size_t length) {
#ifdef _WIN32
  unsigned char* out = (unsigned char*)buffer;
  size_t offset = 0;
  while (offset < length) {
    size_t chunk = length - offset;
    if (chunk > (size_t)ULONG_MAX) chunk = (size_t)ULONG_MAX;
    NTSTATUS status = BCryptGenRandom(NULL, out + offset, (ULONG)chunk,
                                      BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (status != 0) return false;
    offset += chunk;
  }
  return true;
#else
  unsigned char* out = (unsigned char*)buffer;
  size_t offset = 0;
#ifdef __linux__
  while (offset < length) {
    ssize_t readBytes = getrandom(out + offset, length - offset, 0);
    if (readBytes > 0) {
      offset += (size_t)readBytes;
      continue;
    }
    if (readBytes < 0 && errno == EINTR) {
      continue;
    }
    break;
  }
  if (offset == length) return true;
#endif
  int fd = open("/dev/urandom", O_RDONLY);
  if (fd < 0) return false;
  while (offset < length) {
    ssize_t readBytes = read(fd, out + offset, length - offset);
    if (readBytes > 0) {
      offset += (size_t)readBytes;
      continue;
    }
    if (readBytes < 0 && errno == EINTR) {
      continue;
    }
    close(fd);
    return false;
  }
  close(fd);
  return true;
#endif
}

static uint64_t randomNext(void) {
  if (gRandomDeterministic) {
    return randomNextDeterministic();
  }
  uint64_t value = 0;
  if (randomFillFromOs(&value, sizeof(value))) {
    return value;
  }
  return randomNextDeterministic();
}

static uint64_t randomNextBounded(uint64_t bound) {
  if (bound <= 1) return 0;
  uint64_t threshold = (UINT64_MAX - bound + 1ULL) % bound;
  for (;;) {
    uint64_t value = randomNext();
    if (value >= threshold) {
      return value % bound;
    }
  }
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
  if (IS_NULL(args[0])) {
    gRandomDeterministic = false;
    gRandomHasSpare = false;
    return NULL_VAL;
  }
  if (!IS_NUMBER(args[0])) {
    return runtimeErrorValue(vm, "random.seed expects a number or null.");
  }
  int64_t seed = (int64_t)AS_NUMBER(args[0]);
  if (seed == 0) {
    seed = (int64_t)0x9e3779b97f4a7c15ULL;
  }
  gRandomState = (uint64_t)seed;
  gRandomSeeded = true;
  gRandomDeterministic = true;
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
    return NUMBER_VAL((double)randomNextBounded((uint64_t)max));
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
  uint64_t value = randomNextBounded(span);
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
  uint64_t index = randomNextBounded((uint64_t)array->count);
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

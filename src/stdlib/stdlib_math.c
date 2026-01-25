#include "stdlib_internal.h"

#include <math.h>

static bool expectNumberArg(VM* vm, Value value, const char* message) {
  if (!IS_NUMBER(value)) {
    runtimeErrorValue(vm, message);
    return false;
  }
  return true;
}

static double roundNumber(double value) {
  if (value >= 0.0) {
    return floor(value + 0.5);
  }
  return ceil(value - 0.5);
}

static Value nativeMathAbs(VM* vm, int argc, Value* args) {
  (void)argc;
  if (!expectNumberArg(vm, args[0], "math.abs expects a number.")) return NULL_VAL;
  return NUMBER_VAL(fabs(AS_NUMBER(args[0])));
}

static Value nativeMathFloor(VM* vm, int argc, Value* args) {
  (void)argc;
  if (!expectNumberArg(vm, args[0], "math.floor expects a number.")) return NULL_VAL;
  return NUMBER_VAL(floor(AS_NUMBER(args[0])));
}

static Value nativeMathCeil(VM* vm, int argc, Value* args) {
  (void)argc;
  if (!expectNumberArg(vm, args[0], "math.ceil expects a number.")) return NULL_VAL;
  return NUMBER_VAL(ceil(AS_NUMBER(args[0])));
}

static Value nativeMathRound(VM* vm, int argc, Value* args) {
  (void)argc;
  if (!expectNumberArg(vm, args[0], "math.round expects a number.")) return NULL_VAL;
  return NUMBER_VAL(roundNumber(AS_NUMBER(args[0])));
}

static Value nativeMathSqrt(VM* vm, int argc, Value* args) {
  (void)argc;
  if (!expectNumberArg(vm, args[0], "math.sqrt expects a number.")) return NULL_VAL;
  double value = AS_NUMBER(args[0]);
  if (value < 0.0) {
    return runtimeErrorValue(vm, "math.sqrt expects a non-negative number.");
  }
  return NUMBER_VAL(sqrt(value));
}

static Value nativeMathPow(VM* vm, int argc, Value* args) {
  (void)argc;
  if (!expectNumberArg(vm, args[0], "math.pow expects numbers.")) return NULL_VAL;
  if (!expectNumberArg(vm, args[1], "math.pow expects numbers.")) return NULL_VAL;
  return NUMBER_VAL(pow(AS_NUMBER(args[0]), AS_NUMBER(args[1])));
}

static Value nativeMathMin(VM* vm, int argc, Value* args) {
  if (argc < 1) {
    return runtimeErrorValue(vm, "math.min expects at least one number.");
  }
  if (!expectNumberArg(vm, args[0], "math.min expects numbers.")) return NULL_VAL;
  double result = AS_NUMBER(args[0]);
  for (int i = 1; i < argc; i++) {
    if (!expectNumberArg(vm, args[i], "math.min expects numbers.")) return NULL_VAL;
    double value = AS_NUMBER(args[i]);
    if (value < result) result = value;
  }
  return NUMBER_VAL(result);
}

static Value nativeMathMax(VM* vm, int argc, Value* args) {
  if (argc < 1) {
    return runtimeErrorValue(vm, "math.max expects at least one number.");
  }
  if (!expectNumberArg(vm, args[0], "math.max expects numbers.")) return NULL_VAL;
  double result = AS_NUMBER(args[0]);
  for (int i = 1; i < argc; i++) {
    if (!expectNumberArg(vm, args[i], "math.max expects numbers.")) return NULL_VAL;
    double value = AS_NUMBER(args[i]);
    if (value > result) result = value;
  }
  return NUMBER_VAL(result);
}

static Value nativeMathClamp(VM* vm, int argc, Value* args) {
  (void)argc;
  if (!expectNumberArg(vm, args[0], "math.clamp expects numbers.")) return NULL_VAL;
  if (!expectNumberArg(vm, args[1], "math.clamp expects numbers.")) return NULL_VAL;
  if (!expectNumberArg(vm, args[2], "math.clamp expects numbers.")) return NULL_VAL;
  double value = AS_NUMBER(args[0]);
  double minValue = AS_NUMBER(args[1]);
  double maxValue = AS_NUMBER(args[2]);
  if (minValue > maxValue) {
    return runtimeErrorValue(vm, "math.clamp expects min <= max.");
  }
  if (value < minValue) value = minValue;
  if (value > maxValue) value = maxValue;
  return NUMBER_VAL(value);
}


void stdlib_register_math(VM* vm, ObjInstance* module) {
  moduleAdd(vm, module, "abs", nativeMathAbs, 1);
  moduleAdd(vm, module, "floor", nativeMathFloor, 1);
  moduleAdd(vm, module, "ceil", nativeMathCeil, 1);
  moduleAdd(vm, module, "round", nativeMathRound, 1);
  moduleAdd(vm, module, "sqrt", nativeMathSqrt, 1);
  moduleAdd(vm, module, "pow", nativeMathPow, 2);
  moduleAdd(vm, module, "min", nativeMathMin, -1);
  moduleAdd(vm, module, "max", nativeMathMax, -1);
  moduleAdd(vm, module, "clamp", nativeMathClamp, 3);
  moduleAddValue(vm, module, "PI", NUMBER_VAL(3.141592653589793));
  moduleAddValue(vm, module, "E", NUMBER_VAL(2.718281828459045));
}

#include "stdlib_internal.h"

#include <math.h>

static bool vecRead(VM* vm, Value value, int dims, double* out, const char* message) {
  if (!isObjType(value, OBJ_ARRAY)) {
    runtimeErrorValue(vm, message);
    return false;
  }
  ObjArray* array = (ObjArray*)AS_OBJ(value);
  if (array->count < dims) {
    runtimeErrorValue(vm, message);
    return false;
  }
  for (int i = 0; i < dims; i++) {
    if (!IS_NUMBER(array->items[i])) {
      runtimeErrorValue(vm, message);
      return false;
    }
    out[i] = AS_NUMBER(array->items[i]);
  }
  return true;
}

static Value vecMake(VM* vm, int dims, const double* values) {
  ObjArray* array = newArrayWithCapacity(vm, dims);
  for (int i = 0; i < dims; i++) {
    arrayWrite(array, NUMBER_VAL(values[i]));
  }
  return OBJ_VAL(array);
}

static Value vecAddN(VM* vm, int dims, Value* args, const char* message) {
  double a[4];
  double b[4];
  if (!vecRead(vm, args[0], dims, a, message)) return NULL_VAL;
  if (!vecRead(vm, args[1], dims, b, message)) return NULL_VAL;
  double out[4];
  for (int i = 0; i < dims; i++) {
    out[i] = a[i] + b[i];
  }
  return vecMake(vm, dims, out);
}

static Value vecSubN(VM* vm, int dims, Value* args, const char* message) {
  double a[4];
  double b[4];
  if (!vecRead(vm, args[0], dims, a, message)) return NULL_VAL;
  if (!vecRead(vm, args[1], dims, b, message)) return NULL_VAL;
  double out[4];
  for (int i = 0; i < dims; i++) {
    out[i] = a[i] - b[i];
  }
  return vecMake(vm, dims, out);
}

static Value vecScaleN(VM* vm, int dims, Value* args, const char* message) {
  double a[4];
  if (!vecRead(vm, args[0], dims, a, message)) return NULL_VAL;
  if (!IS_NUMBER(args[1])) {
    return runtimeErrorValue(vm, message);
  }
  double scale = AS_NUMBER(args[1]);
  double out[4];
  for (int i = 0; i < dims; i++) {
    out[i] = a[i] * scale;
  }
  return vecMake(vm, dims, out);
}

static Value vecDotN(VM* vm, int dims, Value* args, const char* message) {
  double a[4];
  double b[4];
  if (!vecRead(vm, args[0], dims, a, message)) return NULL_VAL;
  if (!vecRead(vm, args[1], dims, b, message)) return NULL_VAL;
  double sum = 0.0;
  for (int i = 0; i < dims; i++) {
    sum += a[i] * b[i];
  }
  return NUMBER_VAL(sum);
}

static Value vecLenN(VM* vm, int dims, Value* args, const char* message) {
  double a[4];
  if (!vecRead(vm, args[0], dims, a, message)) return NULL_VAL;
  double sum = 0.0;
  for (int i = 0; i < dims; i++) {
    sum += a[i] * a[i];
  }
  return NUMBER_VAL(sqrt(sum));
}

static Value vecNormN(VM* vm, int dims, Value* args, const char* message) {
  double a[4];
  if (!vecRead(vm, args[0], dims, a, message)) return NULL_VAL;
  double sum = 0.0;
  for (int i = 0; i < dims; i++) {
    sum += a[i] * a[i];
  }
  double len = sqrt(sum);
  double out[4];
  if (len <= 0.0) {
    for (int i = 0; i < dims; i++) out[i] = 0.0;
  } else {
    for (int i = 0; i < dims; i++) out[i] = a[i] / len;
  }
  return vecMake(vm, dims, out);
}

static Value vecLerpN(VM* vm, int dims, Value* args, const char* message) {
  double a[4];
  double b[4];
  if (!vecRead(vm, args[0], dims, a, message)) return NULL_VAL;
  if (!vecRead(vm, args[1], dims, b, message)) return NULL_VAL;
  if (!IS_NUMBER(args[2])) {
    return runtimeErrorValue(vm, message);
  }
  double t = AS_NUMBER(args[2]);
  double out[4];
  for (int i = 0; i < dims; i++) {
    out[i] = a[i] + (b[i] - a[i]) * t;
  }
  return vecMake(vm, dims, out);
}

static Value vecDistN(VM* vm, int dims, Value* args, const char* message) {
  double a[4];
  double b[4];
  if (!vecRead(vm, args[0], dims, a, message)) return NULL_VAL;
  if (!vecRead(vm, args[1], dims, b, message)) return NULL_VAL;
  double sum = 0.0;
  for (int i = 0; i < dims; i++) {
    double d = b[i] - a[i];
    sum += d * d;
  }
  return NUMBER_VAL(sqrt(sum));
}

static Value nativeVec2Make(VM* vm, int argc, Value* args) {
  (void)argc;
  if (!IS_NUMBER(args[0]) || !IS_NUMBER(args[1])) {
    return runtimeErrorValue(vm, "vec2.make expects (x, y) numbers.");
  }
  double values[2] = { AS_NUMBER(args[0]), AS_NUMBER(args[1]) };
  return vecMake(vm, 2, values);
}

static Value nativeVec2Add(VM* vm, int argc, Value* args) {
  (void)argc;
  return vecAddN(vm, 2, args, "vec2.add expects two vec2 arrays.");
}

static Value nativeVec2Sub(VM* vm, int argc, Value* args) {
  (void)argc;
  return vecSubN(vm, 2, args, "vec2.sub expects two vec2 arrays.");
}

static Value nativeVec2Scale(VM* vm, int argc, Value* args) {
  (void)argc;
  return vecScaleN(vm, 2, args, "vec2.scale expects (vec2, scalar).");
}

static Value nativeVec2Dot(VM* vm, int argc, Value* args) {
  (void)argc;
  return vecDotN(vm, 2, args, "vec2.dot expects two vec2 arrays.");
}

static Value nativeVec2Len(VM* vm, int argc, Value* args) {
  (void)argc;
  return vecLenN(vm, 2, args, "vec2.len expects a vec2 array.");
}

static Value nativeVec2Norm(VM* vm, int argc, Value* args) {
  (void)argc;
  return vecNormN(vm, 2, args, "vec2.norm expects a vec2 array.");
}

static Value nativeVec2Lerp(VM* vm, int argc, Value* args) {
  (void)argc;
  return vecLerpN(vm, 2, args, "vec2.lerp expects (a, b, t).");
}

static Value nativeVec2Dist(VM* vm, int argc, Value* args) {
  (void)argc;
  return vecDistN(vm, 2, args, "vec2.dist expects two vec2 arrays.");
}

static Value nativeVec3Make(VM* vm, int argc, Value* args) {
  (void)argc;
  if (!IS_NUMBER(args[0]) || !IS_NUMBER(args[1]) || !IS_NUMBER(args[2])) {
    return runtimeErrorValue(vm, "vec3.make expects (x, y, z) numbers.");
  }
  double values[3] = { AS_NUMBER(args[0]), AS_NUMBER(args[1]), AS_NUMBER(args[2]) };
  return vecMake(vm, 3, values);
}

static Value nativeVec3Add(VM* vm, int argc, Value* args) {
  (void)argc;
  return vecAddN(vm, 3, args, "vec3.add expects two vec3 arrays.");
}

static Value nativeVec3Sub(VM* vm, int argc, Value* args) {
  (void)argc;
  return vecSubN(vm, 3, args, "vec3.sub expects two vec3 arrays.");
}

static Value nativeVec3Scale(VM* vm, int argc, Value* args) {
  (void)argc;
  return vecScaleN(vm, 3, args, "vec3.scale expects (vec3, scalar).");
}

static Value nativeVec3Dot(VM* vm, int argc, Value* args) {
  (void)argc;
  return vecDotN(vm, 3, args, "vec3.dot expects two vec3 arrays.");
}

static Value nativeVec3Len(VM* vm, int argc, Value* args) {
  (void)argc;
  return vecLenN(vm, 3, args, "vec3.len expects a vec3 array.");
}

static Value nativeVec3Norm(VM* vm, int argc, Value* args) {
  (void)argc;
  return vecNormN(vm, 3, args, "vec3.norm expects a vec3 array.");
}

static Value nativeVec3Lerp(VM* vm, int argc, Value* args) {
  (void)argc;
  return vecLerpN(vm, 3, args, "vec3.lerp expects (a, b, t).");
}

static Value nativeVec3Dist(VM* vm, int argc, Value* args) {
  (void)argc;
  return vecDistN(vm, 3, args, "vec3.dist expects two vec3 arrays.");
}

static Value nativeVec3Cross(VM* vm, int argc, Value* args) {
  (void)argc;
  double a[3];
  double b[3];
  if (!vecRead(vm, args[0], 3, a, "vec3.cross expects two vec3 arrays.")) return NULL_VAL;
  if (!vecRead(vm, args[1], 3, b, "vec3.cross expects two vec3 arrays.")) return NULL_VAL;
  double out[3] = {
    a[1] * b[2] - a[2] * b[1],
    a[2] * b[0] - a[0] * b[2],
    a[0] * b[1] - a[1] * b[0]
  };
  return vecMake(vm, 3, out);
}

static Value nativeVec4Make(VM* vm, int argc, Value* args) {
  (void)argc;
  if (!IS_NUMBER(args[0]) || !IS_NUMBER(args[1]) ||
      !IS_NUMBER(args[2]) || !IS_NUMBER(args[3])) {
    return runtimeErrorValue(vm, "vec4.make expects (x, y, z, w) numbers.");
  }
  double values[4] = {
    AS_NUMBER(args[0]), AS_NUMBER(args[1]),
    AS_NUMBER(args[2]), AS_NUMBER(args[3])
  };
  return vecMake(vm, 4, values);
}

static Value nativeVec4Add(VM* vm, int argc, Value* args) {
  (void)argc;
  return vecAddN(vm, 4, args, "vec4.add expects two vec4 arrays.");
}

static Value nativeVec4Sub(VM* vm, int argc, Value* args) {
  (void)argc;
  return vecSubN(vm, 4, args, "vec4.sub expects two vec4 arrays.");
}

static Value nativeVec4Scale(VM* vm, int argc, Value* args) {
  (void)argc;
  return vecScaleN(vm, 4, args, "vec4.scale expects (vec4, scalar).");
}

static Value nativeVec4Dot(VM* vm, int argc, Value* args) {
  (void)argc;
  return vecDotN(vm, 4, args, "vec4.dot expects two vec4 arrays.");
}

static Value nativeVec4Len(VM* vm, int argc, Value* args) {
  (void)argc;
  return vecLenN(vm, 4, args, "vec4.len expects a vec4 array.");
}

static Value nativeVec4Norm(VM* vm, int argc, Value* args) {
  (void)argc;
  return vecNormN(vm, 4, args, "vec4.norm expects a vec4 array.");
}

static Value nativeVec4Lerp(VM* vm, int argc, Value* args) {
  (void)argc;
  return vecLerpN(vm, 4, args, "vec4.lerp expects (a, b, t).");
}

static Value nativeVec4Dist(VM* vm, int argc, Value* args) {
  (void)argc;
  return vecDistN(vm, 4, args, "vec4.dist expects two vec4 arrays.");
}


void stdlib_register_vec(VM* vm, ObjInstance* vec2, ObjInstance* vec3, ObjInstance* vec4) {
  moduleAdd(vm, vec2, "make", nativeVec2Make, 2);
  moduleAdd(vm, vec2, "add", nativeVec2Add, 2);
  moduleAdd(vm, vec2, "sub", nativeVec2Sub, 2);
  moduleAdd(vm, vec2, "scale", nativeVec2Scale, 2);
  moduleAdd(vm, vec2, "dot", nativeVec2Dot, 2);
  moduleAdd(vm, vec2, "len", nativeVec2Len, 1);
  moduleAdd(vm, vec2, "norm", nativeVec2Norm, 1);
  moduleAdd(vm, vec2, "lerp", nativeVec2Lerp, 3);
  moduleAdd(vm, vec2, "dist", nativeVec2Dist, 2);

  moduleAdd(vm, vec3, "make", nativeVec3Make, 3);
  moduleAdd(vm, vec3, "add", nativeVec3Add, 2);
  moduleAdd(vm, vec3, "sub", nativeVec3Sub, 2);
  moduleAdd(vm, vec3, "scale", nativeVec3Scale, 2);
  moduleAdd(vm, vec3, "dot", nativeVec3Dot, 2);
  moduleAdd(vm, vec3, "len", nativeVec3Len, 1);
  moduleAdd(vm, vec3, "norm", nativeVec3Norm, 1);
  moduleAdd(vm, vec3, "lerp", nativeVec3Lerp, 3);
  moduleAdd(vm, vec3, "dist", nativeVec3Dist, 2);
  moduleAdd(vm, vec3, "cross", nativeVec3Cross, 2);

  moduleAdd(vm, vec4, "make", nativeVec4Make, 4);
  moduleAdd(vm, vec4, "add", nativeVec4Add, 2);
  moduleAdd(vm, vec4, "sub", nativeVec4Sub, 2);
  moduleAdd(vm, vec4, "scale", nativeVec4Scale, 2);
  moduleAdd(vm, vec4, "dot", nativeVec4Dot, 2);
  moduleAdd(vm, vec4, "len", nativeVec4Len, 1);
  moduleAdd(vm, vec4, "norm", nativeVec4Norm, 1);
  moduleAdd(vm, vec4, "lerp", nativeVec4Lerp, 3);
  moduleAdd(vm, vec4, "dist", nativeVec4Dist, 2);
}

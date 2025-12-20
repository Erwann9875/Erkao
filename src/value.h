#ifndef ERKAO_VALUE_H
#define ERKAO_VALUE_H

#include "common.h"
#include "lexer.h"

typedef struct Obj Obj;
typedef struct ObjString ObjString;
typedef struct ObjFunction ObjFunction;
typedef struct ObjNative ObjNative;
typedef struct ObjClass ObjClass;
typedef struct ObjInstance ObjInstance;
typedef struct ObjArray ObjArray;
typedef struct ObjMap ObjMap;
typedef struct ObjBoundMethod ObjBoundMethod;
typedef struct Chunk Chunk;

typedef struct VM VM;
typedef struct Env Env;
typedef struct Program Program;

typedef enum {
  VAL_NULL,
  VAL_BOOL,
  VAL_NUMBER,
  VAL_OBJ
} ValueType;

typedef struct {
  ValueType type;
  union {
    bool boolean;
    double number;
    Obj* obj;
  } as;
} Value;

#define BOOL_VAL(value) ((Value){ VAL_BOOL, { .boolean = (value) } })
#define NUMBER_VAL(value) ((Value){ VAL_NUMBER, { .number = (value) } })
#define NULL_VAL ((Value){ VAL_NULL, { .number = 0 } })
#define OBJ_VAL(object) ((Value){ VAL_OBJ, { .obj = (Obj*)(object) } })

#define IS_BOOL(value) ((value).type == VAL_BOOL)
#define IS_NUMBER(value) ((value).type == VAL_NUMBER)
#define IS_NULL(value) ((value).type == VAL_NULL)
#define IS_OBJ(value) ((value).type == VAL_OBJ)

#define AS_BOOL(value) ((value).as.boolean)
#define AS_NUMBER(value) ((value).as.number)
#define AS_OBJ(value) ((value).as.obj)

typedef Value (*NativeFn)(VM* vm, int argc, Value* args);

typedef enum {
  OBJ_STRING,
  OBJ_FUNCTION,
  OBJ_NATIVE,
  OBJ_CLASS,
  OBJ_INSTANCE,
  OBJ_ARRAY,
  OBJ_MAP,
  OBJ_BOUND_METHOD
} ObjType;

typedef enum {
  OBJ_GEN_YOUNG = 0,
  OBJ_GEN_OLD = 1
} ObjGen;

struct Obj {
  ObjType type;
  Obj* next;
  bool marked;
  ObjGen generation;
  uint8_t age;
  size_t size;
};

struct ObjString {
  Obj obj;
  int length;
  char* chars;
};

struct ObjFunction {
  Obj obj;
  int arity;
  bool isInitializer;
  ObjString* name;
  Chunk* chunk;
  ObjString** params;
  Env* closure;
  Program* program;
};

struct ObjNative {
  Obj obj;
  NativeFn function;
  int arity;
  ObjString* name;
};

typedef struct {
  ObjString* key;
  Value value;
} MapEntryValue;

struct ObjMap {
  Obj obj;
  VM* vm;
  MapEntryValue* entries;
  int count;
  int capacity;
};

struct ObjClass {
  Obj obj;
  ObjString* name;
  ObjMap* methods;
};

struct ObjInstance {
  Obj obj;
  ObjClass* klass;
  ObjMap* fields;
};

struct ObjArray {
  Obj obj;
  VM* vm;
  Value* items;
  int count;
  int capacity;
};

struct ObjBoundMethod {
  Obj obj;
  Value receiver;
  ObjFunction* method;
};

ObjString* copyString(VM* vm, const char* chars);
ObjString* copyStringWithLength(VM* vm, const char* chars, int length);
ObjString* stringFromToken(VM* vm, Token token);

ObjFunction* newFunction(VM* vm, ObjString* name, int arity, bool isInitializer,
                         ObjString** params, Chunk* chunk, Env* closure, Program* program);
ObjFunction* cloneFunction(VM* vm, ObjFunction* proto, Env* closure);
ObjNative* newNative(VM* vm, NativeFn function, int arity, ObjString* name);
ObjClass* newClass(VM* vm, ObjString* name, ObjMap* methods);
ObjInstance* newInstance(VM* vm, ObjClass* klass);
ObjInstance* newInstanceWithFields(VM* vm, ObjClass* klass, ObjMap* fields);
ObjArray* newArray(VM* vm);
ObjMap* newMap(VM* vm);
ObjBoundMethod* newBoundMethod(VM* vm, Value receiver, ObjFunction* method);

void arrayWrite(ObjArray* array, Value value);
bool arrayGet(ObjArray* array, int index, Value* out);
bool arraySet(ObjArray* array, int index, Value value);

bool mapGet(ObjMap* map, ObjString* key, Value* out);
bool mapGetByToken(ObjMap* map, Token key, Value* out);
void mapSet(ObjMap* map, ObjString* key, Value value);
bool mapSetByTokenIfExists(ObjMap* map, Token key, Value value);
bool mapSetIfExists(ObjMap* map, ObjString* key, Value value);
int mapCount(ObjMap* map);

bool isObjType(Value value, ObjType type);
const char* valueTypeName(Value value);
bool valuesEqual(Value a, Value b);
void printValue(Value value);

#endif

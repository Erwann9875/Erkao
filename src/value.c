#include "value.h"
#include "interpreter.h"

static Obj* allocateObject(VM* vm, size_t size, ObjType type) {
  Obj* object = (Obj*)malloc(size);
  if (!object) {
    fprintf(stderr, "Out of memory.\n");
    exit(1);
  }
  object->type = type;
  object->next = vm->objects;
  vm->objects = object;
  return object;
}

static ObjString* allocateString(VM* vm, char* chars, int length) {
  ObjString* string = (ObjString*)allocateObject(vm, sizeof(ObjString), OBJ_STRING);
  string->length = length;
  string->chars = chars;
  return string;
}

ObjString* copyStringWithLength(VM* vm, const char* chars, int length) {
  char* heap = (char*)malloc((size_t)length + 1);
  if (!heap) {
    fprintf(stderr, "Out of memory.\n");
    exit(1);
  }
  memcpy(heap, chars, (size_t)length);
  heap[length] = '\0';
  return allocateString(vm, heap, length);
}

ObjString* copyString(VM* vm, const char* chars) {
  return copyStringWithLength(vm, chars, (int)strlen(chars));
}

ObjString* stringFromToken(VM* vm, Token token) {
  return copyStringWithLength(vm, token.start, token.length);
}

ObjFunction* newFunction(VM* vm, Stmt* declaration, ObjString* name, int arity,
                         bool isInitializer, Env* closure) {
  ObjFunction* function = (ObjFunction*)allocateObject(vm, sizeof(ObjFunction), OBJ_FUNCTION);
  function->arity = arity;
  function->isInitializer = isInitializer;
  function->name = name;
  function->declaration = declaration;
  function->closure = closure;
  return function;
}

ObjNative* newNative(VM* vm, NativeFn function, int arity, ObjString* name) {
  ObjNative* native = (ObjNative*)allocateObject(vm, sizeof(ObjNative), OBJ_NATIVE);
  native->function = function;
  native->arity = arity;
  native->name = name;
  return native;
}

ObjClass* newClass(VM* vm, ObjString* name, ObjMap* methods) {
  ObjClass* klass = (ObjClass*)allocateObject(vm, sizeof(ObjClass), OBJ_CLASS);
  klass->name = name;
  klass->methods = methods;
  return klass;
}

ObjInstance* newInstance(VM* vm, ObjClass* klass) {
  ObjInstance* instance = (ObjInstance*)allocateObject(vm, sizeof(ObjInstance), OBJ_INSTANCE);
  instance->klass = klass;
  instance->fields = newMap(vm);
  return instance;
}

ObjArray* newArray(VM* vm) {
  ObjArray* array = (ObjArray*)allocateObject(vm, sizeof(ObjArray), OBJ_ARRAY);
  array->items = NULL;
  array->count = 0;
  array->capacity = 0;
  return array;
}

ObjMap* newMap(VM* vm) {
  ObjMap* map = (ObjMap*)allocateObject(vm, sizeof(ObjMap), OBJ_MAP);
  map->entries = NULL;
  map->count = 0;
  map->capacity = 0;
  return map;
}

ObjBoundMethod* newBoundMethod(VM* vm, Value receiver, ObjFunction* method) {
  ObjBoundMethod* bound = (ObjBoundMethod*)allocateObject(vm, sizeof(ObjBoundMethod), OBJ_BOUND_METHOD);
  bound->receiver = receiver;
  bound->method = method;
  return bound;
}

void arrayWrite(ObjArray* array, Value value) {
  if (array->capacity < array->count + 1) {
    int oldCapacity = array->capacity;
    array->capacity = GROW_CAPACITY(oldCapacity);
    array->items = GROW_ARRAY(Value, array->items, oldCapacity, array->capacity);
  }
  array->items[array->count++] = value;
}

bool arrayGet(ObjArray* array, int index, Value* out) {
  if (index < 0 || index >= array->count) return false;
  *out = array->items[index];
  return true;
}

bool arraySet(ObjArray* array, int index, Value value) {
  if (index < 0) return false;
  if (index < array->count) {
    array->items[index] = value;
    return true;
  }
  if (index == array->count) {
    arrayWrite(array, value);
    return true;
  }
  return false;
}

static bool stringsEqual(ObjString* a, ObjString* b) {
  if (a->length != b->length) return false;
  return memcmp(a->chars, b->chars, (size_t)a->length) == 0;
}

static bool stringMatchesToken(ObjString* string, Token token) {
  if (string->length != token.length) return false;
  return memcmp(string->chars, token.start, (size_t)token.length) == 0;
}

bool mapGet(ObjMap* map, ObjString* key, Value* out) {
  for (int i = 0; i < map->count; i++) {
    if (stringsEqual(map->entries[i].key, key)) {
      *out = map->entries[i].value;
      return true;
    }
  }
  return false;
}

bool mapGetByToken(ObjMap* map, Token key, Value* out) {
  for (int i = 0; i < map->count; i++) {
    if (stringMatchesToken(map->entries[i].key, key)) {
      *out = map->entries[i].value;
      return true;
    }
  }
  return false;
}

void mapSet(ObjMap* map, ObjString* key, Value value) {
  for (int i = 0; i < map->count; i++) {
    if (stringsEqual(map->entries[i].key, key)) {
      map->entries[i].value = value;
      return;
    }
  }

  if (map->capacity < map->count + 1) {
    int oldCapacity = map->capacity;
    map->capacity = GROW_CAPACITY(oldCapacity);
    map->entries = GROW_ARRAY(MapEntryValue, map->entries, oldCapacity, map->capacity);
  }

  map->entries[map->count].key = key;
  map->entries[map->count].value = value;
  map->count++;
}

bool mapSetByTokenIfExists(ObjMap* map, Token key, Value value) {
  for (int i = 0; i < map->count; i++) {
    if (stringMatchesToken(map->entries[i].key, key)) {
      map->entries[i].value = value;
      return true;
    }
  }
  return false;
}

int mapCount(ObjMap* map) {
  return map->count;
}

bool isObjType(Value value, ObjType type) {
  return IS_OBJ(value) && AS_OBJ(value)->type == type;
}

static const char* objTypeName(ObjType type) {
  switch (type) {
    case OBJ_STRING: return "string";
    case OBJ_FUNCTION: return "function";
    case OBJ_NATIVE: return "native";
    case OBJ_CLASS: return "class";
    case OBJ_INSTANCE: return "instance";
    case OBJ_ARRAY: return "array";
    case OBJ_MAP: return "map";
    case OBJ_BOUND_METHOD: return "bound_method";
    default: return "object";
  }
}

const char* valueTypeName(Value value) {
  switch (value.type) {
    case VAL_NULL: return "null";
    case VAL_BOOL: return "bool";
    case VAL_NUMBER: return "number";
    case VAL_OBJ:
      return objTypeName(AS_OBJ(value)->type);
    default:
      return "unknown";
  }
}

bool valuesEqual(Value a, Value b) {
  if (a.type != b.type) return false;
  switch (a.type) {
    case VAL_NULL: return true;
    case VAL_BOOL: return AS_BOOL(a) == AS_BOOL(b);
    case VAL_NUMBER: return AS_NUMBER(a) == AS_NUMBER(b);
    case VAL_OBJ: {
      Obj* objA = AS_OBJ(a);
      Obj* objB = AS_OBJ(b);
      if (objA->type != objB->type) return false;
      if (objA->type == OBJ_STRING) {
        return stringsEqual((ObjString*)objA, (ObjString*)objB);
      }
      return objA == objB;
    }
    default:
      return false;
  }
}

static void printObject(Value value);

void printValue(Value value) {
  switch (value.type) {
    case VAL_NULL:
      printf("null");
      break;
    case VAL_BOOL:
      printf(AS_BOOL(value) ? "true" : "false");
      break;
    case VAL_NUMBER:
      printf("%g", AS_NUMBER(value));
      break;
    case VAL_OBJ:
      printObject(value);
      break;
  }
}

static void printArray(ObjArray* array) {
  printf("[");
  for (int i = 0; i < array->count; i++) {
    if (i > 0) printf(", ");
    printValue(array->items[i]);
  }
  printf("]");
}

static void printMap(ObjMap* map) {
  printf("{");
  for (int i = 0; i < map->count; i++) {
    if (i > 0) printf(", ");
    printf("%s: ", map->entries[i].key->chars);
    printValue(map->entries[i].value);
  }
  printf("}");
}

static void printObject(Value value) {
  switch (AS_OBJ(value)->type) {
    case OBJ_STRING:
      printf("%s", ((ObjString*)AS_OBJ(value))->chars);
      break;
    case OBJ_FUNCTION: {
      ObjFunction* function = (ObjFunction*)AS_OBJ(value);
      if (function->name) {
        printf("<fun %s>", function->name->chars);
      } else {
        printf("<fun>");
      }
      break;
    }
    case OBJ_NATIVE: {
      ObjNative* native = (ObjNative*)AS_OBJ(value);
      if (native->name) {
        printf("<native %s>", native->name->chars);
      } else {
        printf("<native>");
      }
      break;
    }
    case OBJ_CLASS: {
      ObjClass* klass = (ObjClass*)AS_OBJ(value);
      printf("<class %s>", klass->name->chars);
      break;
    }
    case OBJ_INSTANCE: {
      ObjInstance* instance = (ObjInstance*)AS_OBJ(value);
      printf("<%s instance>", instance->klass->name->chars);
      break;
    }
    case OBJ_ARRAY:
      printArray((ObjArray*)AS_OBJ(value));
      break;
    case OBJ_MAP:
      printMap((ObjMap*)AS_OBJ(value));
      break;
    case OBJ_BOUND_METHOD:
      printf("<bound method>");
      break;
  }
}

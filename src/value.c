#include "value.h"
#include "chunk.h"
#include "interpreter.h"
#include "gc.h"
#include "program.h"

static uint32_t hashBytes(const char* chars, int length) {
  uint32_t hash = 2166136261u;
  for (int i = 0; i < length; i++) {
    hash ^= (uint8_t)chars[i];
    hash *= 16777619u;
  }
  return hash;
}

static ObjString* findInternedString(VM* vm, const char* chars, int length) {
  if (!vm || !vm->strings) return NULL;
  if (!chars && length > 0) return NULL;
  Token token;
  memset(&token, 0, sizeof(Token));
  token.start = chars ? chars : "";
  token.length = length;
  Value value;
  if (mapGetByToken(vm->strings, token, &value)) {
    if (isObjType(value, OBJ_STRING)) {
      return (ObjString*)AS_OBJ(value);
    }
  }
  return NULL;
}

static bool stringsEqual(ObjString* a, ObjString* b);
static MapEntryValue* mapFindEntry(MapEntryValue* entries, int capacity, ObjString* key);
static MapEntryValue* mapFindEntryByToken(MapEntryValue* entries, int capacity,
                                          Token key, uint32_t keyHash);
static int mapCapacityForCount(int count);
static void adjustMapCapacity(ObjMap* map, int capacity);

static Obj* allocateObject(VM* vm, size_t size, ObjType type, ObjGen generation) {
  Obj* object = (Obj*)malloc(size);
  if (!object) {
    fprintf(stderr, "Out of memory.\n");
    exit(1);
  }
  object->type = type;
  object->marked = false;
  object->remembered = false;
  object->generation = generation;
  object->age = 0;
  object->size = size;
  if (generation == OBJ_GEN_OLD) {
    object->next = vm->oldObjects;
    vm->oldObjects = object;
  } else {
    object->next = vm->youngObjects;
    vm->youngObjects = object;
  }
  gcTrackAlloc(vm, object);
  return object;
}

static ObjString* allocateString(VM* vm, char* chars, int length) {
  size_t size = sizeof(ObjString) + (size_t)length + 1;
  ObjString* string = (ObjString*)allocateObject(vm, size, OBJ_STRING, OBJ_GEN_OLD);
  string->length = length;
  string->chars = chars;
  string->hash = hashBytes(chars, length);
  return string;
}

ObjString* copyStringWithLength(VM* vm, const char* chars, int length) {
  if (length < 0) length = 0;
  if (!chars) chars = "";
  ObjString* interned = findInternedString(vm, chars, length);
  if (interned) return interned;

  char* heap = (char*)malloc((size_t)length + 1);
  if (!heap) {
    fprintf(stderr, "Out of memory.\n");
    exit(1);
  }
  if (length > 0) {
    memcpy(heap, chars, (size_t)length);
  }
  heap[length] = '\0';
  ObjString* string = allocateString(vm, heap, length);
  if (vm && vm->strings) {
    mapSet(vm->strings, string, OBJ_VAL(string));
  }
  return string;
}

ObjString* takeStringWithLength(VM* vm, char* chars, int length) {
  if (length < 0) length = 0;
  if (!chars) {
    return copyStringWithLength(vm, "", 0);
  }

  ObjString* interned = findInternedString(vm, chars, length);
  if (interned) {
    free(chars);
    return interned;
  }
  chars[length] = '\0';
  ObjString* string = allocateString(vm, chars, length);
  if (vm && vm->strings) {
    mapSet(vm->strings, string, OBJ_VAL(string));
  }
  return string;
}

ObjString* copyString(VM* vm, const char* chars) {
  return copyStringWithLength(vm, chars, (int)strlen(chars));
}

ObjString* stringFromToken(VM* vm, Token token) {
  return copyStringWithLength(vm, token.start, token.length);
}

ObjFunction* newFunction(VM* vm, ObjString* name, int arity, int minArity,
                         bool isInitializer, ObjString** params, Chunk* chunk,
                         Env* closure, Program* program) {
  ObjFunction* function = (ObjFunction*)allocateObject(vm, sizeof(ObjFunction), OBJ_FUNCTION,
                                                      OBJ_GEN_OLD);
  function->arity = arity;
  function->minArity = minArity;
  function->isInitializer = isInitializer;
  function->name = name;
  function->chunk = chunk;
  function->params = params;
  function->closure = closure;
  function->program = program;
  programRetain(program);
  gcRememberObjectIfYoungRefs(vm, (Obj*)function);
  return function;
}

ObjFunction* cloneFunction(VM* vm, ObjFunction* proto, Env* closure) {
  Chunk* chunk = cloneChunk(proto->chunk);
  ObjString** params = NULL;
  if (proto->arity > 0) {
    params = (ObjString**)malloc(sizeof(ObjString*) * (size_t)proto->arity);
    if (!params) {
      fprintf(stderr, "Out of memory.\n");
      exit(1);
    }
    for (int i = 0; i < proto->arity; i++) {
      params[i] = proto->params[i];
    }
  }
  return newFunction(vm, proto->name, proto->arity, proto->minArity, proto->isInitializer,
                     params, chunk, closure, proto->program);
}

ObjNative* newNative(VM* vm, NativeFn function, int arity, ObjString* name) {
  ObjNative* native = (ObjNative*)allocateObject(vm, sizeof(ObjNative), OBJ_NATIVE, OBJ_GEN_OLD);
  native->function = function;
  native->arity = arity;
  native->name = name;
  gcRememberObjectIfYoungRefs(vm, (Obj*)native);
  return native;
}

ObjClass* newClass(VM* vm, ObjString* name, ObjMap* methods) {
  ObjClass* klass = (ObjClass*)allocateObject(vm, sizeof(ObjClass), OBJ_CLASS, OBJ_GEN_OLD);
  klass->name = name;
  klass->methods = methods;
  gcRememberObjectIfYoungRefs(vm, (Obj*)klass);
  return klass;
}

ObjInstance* newInstance(VM* vm, ObjClass* klass) {
  ObjInstance* instance = (ObjInstance*)allocateObject(vm, sizeof(ObjInstance), OBJ_INSTANCE,
                                                       OBJ_GEN_YOUNG);
  instance->klass = klass;
  instance->fields = newMap(vm);
  return instance;
}

ObjInstance* newInstanceWithFields(VM* vm, ObjClass* klass, ObjMap* fields) {
  ObjInstance* instance = (ObjInstance*)allocateObject(vm, sizeof(ObjInstance), OBJ_INSTANCE,
                                                       OBJ_GEN_YOUNG);
  instance->klass = klass;
  instance->fields = fields;
  return instance;
}

ObjArray* newArray(VM* vm) {
  return newArrayWithCapacity(vm, 0);
}

ObjArray* newArrayWithCapacity(VM* vm, int capacity) {
  ObjArray* array = (ObjArray*)allocateObject(vm, sizeof(ObjArray), OBJ_ARRAY, OBJ_GEN_YOUNG);
  array->vm = vm;
  array->items = NULL;
  array->count = 0;
  array->capacity = 0;
  if (capacity > 0) {
    array->items = (Value*)malloc(sizeof(Value) * (size_t)capacity);
    if (!array->items) {
      fprintf(stderr, "Out of memory.\n");
      exit(1);
    }
    array->capacity = capacity;
    size_t oldSize = array->obj.size;
    size_t extra = sizeof(Value) * (size_t)capacity;
    array->obj.size = oldSize + extra;
    if (array->vm) {
      gcTrackResize(array->vm, (Obj*)array, oldSize, array->obj.size);
    }
  }
  return array;
}

ObjMap* newMap(VM* vm) {
  return newMapWithCapacity(vm, 0);
}

ObjMap* newMapWithCapacity(VM* vm, int capacity) {
  ObjMap* map = (ObjMap*)allocateObject(vm, sizeof(ObjMap), OBJ_MAP, OBJ_GEN_YOUNG);
  map->vm = vm;
  map->entries = NULL;
  map->count = 0;
  map->capacity = 0;
  int target = mapCapacityForCount(capacity);
  if (target > 0) {
    adjustMapCapacity(map, target);
  }
  return map;
}

ObjBoundMethod* newBoundMethod(VM* vm, Value receiver, ObjFunction* method) {
  ObjBoundMethod* bound = (ObjBoundMethod*)allocateObject(vm, sizeof(ObjBoundMethod),
                                                         OBJ_BOUND_METHOD, OBJ_GEN_YOUNG);
  bound->receiver = receiver;
  bound->method = method;
  return bound;
}

void arrayWrite(ObjArray* array, Value value) {
  if (array->capacity < array->count + 1) {
    int oldCapacity = array->capacity;
    array->capacity = GROW_CAPACITY(oldCapacity);
    array->items = GROW_ARRAY(Value, array->items, oldCapacity, array->capacity);
    size_t oldSize = array->obj.size;
    size_t extra = sizeof(Value) * (size_t)(array->capacity - oldCapacity);
    size_t newSize = oldSize + extra;
    array->obj.size = newSize;
    if (array->vm) {
      gcTrackResize(array->vm, (Obj*)array, oldSize, newSize);
    }
  }
  array->items[array->count++] = value;
  if (array->vm) {
    gcWriteBarrier(array->vm, (Obj*)array, value);
  }
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
    if (array->vm) {
      gcWriteBarrier(array->vm, (Obj*)array, value);
    }
    return true;
  }
  if (index == array->count) {
    arrayWrite(array, value);
    return true;
  }
  return false;
}

static bool stringsEqual(ObjString* a, ObjString* b) {
  if (a == b) return true;
  if (a->length != b->length) return false;
  if (a->hash != b->hash) return false;
  return memcmp(a->chars, b->chars, (size_t)a->length) == 0;
}

#define MAP_MAX_LOAD 0.75

static MapEntryValue* mapFindEntry(MapEntryValue* entries, int capacity, ObjString* key) {
  uint32_t index = key->hash & (uint32_t)(capacity - 1);
  for (;;) {
    MapEntryValue* entry = &entries[index];
    if (!entry->key || entry->key == key || stringsEqual(entry->key, key)) {
      return entry;
    }
    index = (index + 1) & (uint32_t)(capacity - 1);
  }
}

static MapEntryValue* mapFindEntryByToken(MapEntryValue* entries, int capacity,
                                          Token key, uint32_t keyHash) {
  uint32_t index = keyHash & (uint32_t)(capacity - 1);
  for (;;) {
    MapEntryValue* entry = &entries[index];
    if (!entry->key) return entry;
    if (entry->key->hash == keyHash && entry->key->length == key.length &&
        memcmp(entry->key->chars, key.start, (size_t)key.length) == 0) {
      return entry;
    }
    index = (index + 1) & (uint32_t)(capacity - 1);
  }
}

static int mapCapacityForCount(int count) {
  if (count <= 0) return 0;
  int capacity = 8;
  while ((int)(capacity * MAP_MAX_LOAD) < count) {
    capacity *= 2;
  }
  return capacity;
}

static void adjustMapCapacity(ObjMap* map, int capacity) {
  MapEntryValue* entries = (MapEntryValue*)malloc(sizeof(MapEntryValue) * (size_t)capacity);
  if (!entries) {
    fprintf(stderr, "Out of memory.\n");
    exit(1);
  }
  for (int i = 0; i < capacity; i++) {
    entries[i].key = NULL;
    entries[i].value = NULL_VAL;
  }

  int oldCapacity = map->capacity;
  MapEntryValue* oldEntries = map->entries;
  map->entries = entries;
  map->capacity = capacity;
  map->count = 0;

  for (int i = 0; i < oldCapacity; i++) {
    MapEntryValue* entry = &oldEntries[i];
    if (!entry->key) continue;
    MapEntryValue* dest = mapFindEntry(entries, capacity, entry->key);
    dest->key = entry->key;
    dest->value = entry->value;
    map->count++;
  }

  if (oldEntries) {
    free(oldEntries);
  }

  size_t oldSize = map->obj.size;
  size_t newSize = sizeof(ObjMap) + sizeof(MapEntryValue) * (size_t)capacity;
  map->obj.size = newSize;
  if (map->vm) {
    gcTrackResize(map->vm, (Obj*)map, oldSize, newSize);
  }
}

bool mapGet(ObjMap* map, ObjString* key, Value* out) {
  if (map->count == 0 || map->capacity == 0) return false;
  MapEntryValue* entry = mapFindEntry(map->entries, map->capacity, key);
  if (!entry->key) return false;
  *out = entry->value;
  return true;
}

bool mapGetIndex(ObjMap* map, ObjString* key, Value* out, int* outIndex) {
  if (map->count == 0 || map->capacity == 0) return false;
  MapEntryValue* entry = mapFindEntry(map->entries, map->capacity, key);
  if (!entry->key) return false;
  if (out) *out = entry->value;
  if (outIndex) *outIndex = (int)(entry - map->entries);
  return true;
}

bool mapGetByToken(ObjMap* map, Token key, Value* out) {
  if (map->count == 0 || map->capacity == 0) return false;
  uint32_t tokenHash = hashBytes(key.start, key.length);
  MapEntryValue* entry = mapFindEntryByToken(map->entries, map->capacity, key, tokenHash);
  if (!entry->key) return false;
  *out = entry->value;
  return true;
}

void mapSet(ObjMap* map, ObjString* key, Value value) {
  (void)mapSetIndex(map, key, value);
}

int mapSetIndex(ObjMap* map, ObjString* key, Value value) {
  if ((map->count + 1) > (int)(map->capacity * MAP_MAX_LOAD)) {
    int capacity = map->capacity < 8 ? 8 : map->capacity * 2;
    adjustMapCapacity(map, capacity);
  }

  MapEntryValue* entry = mapFindEntry(map->entries, map->capacity, key);
  bool isNewKey = entry->key == NULL;
  if (isNewKey) {
    map->count++;
  }
  entry->key = key;
  entry->value = value;
  if (map->vm) {
    gcWriteBarrier(map->vm, (Obj*)map, OBJ_VAL(key));
    gcWriteBarrier(map->vm, (Obj*)map, value);
  }
  return (int)(entry - map->entries);
}

bool mapSetByTokenIfExists(ObjMap* map, Token key, Value value) {
  if (map->count == 0 || map->capacity == 0) return false;
  uint32_t tokenHash = hashBytes(key.start, key.length);
  MapEntryValue* entry = mapFindEntryByToken(map->entries, map->capacity, key, tokenHash);
  if (!entry->key) return false;
  entry->value = value;
  if (map->vm) {
    gcWriteBarrier(map->vm, (Obj*)map, value);
  }
  return true;
}

bool mapSetIfExists(ObjMap* map, ObjString* key, Value value) {
  if (map->count == 0 || map->capacity == 0) return false;
  MapEntryValue* entry = mapFindEntry(map->entries, map->capacity, key);
  if (!entry->key) return false;
  entry->value = value;
  if (map->vm) {
    gcWriteBarrier(map->vm, (Obj*)map, value);
  }
  return true;
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
  int printed = 0;
  for (int i = 0; i < map->capacity; i++) {
    if (!map->entries[i].key) continue;
    if (printed > 0) printf(", ");
    printf("%s: ", map->entries[i].key->chars);
    printValue(map->entries[i].value);
    printed++;
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

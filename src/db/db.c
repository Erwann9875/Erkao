
#include "db.h"
#include "gc.h"
#include "interpreter_internal.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  int id;
  const DbDriver* driver;
  void* handle;
  bool open;
} DbConnection;

struct DbState {
  const DbDriver** drivers;
  int driverCount;
  int driverCapacity;
  DbConnection** connections;
  int connectionCount;
  int connectionCapacity;
  int nextId;
  ObjClass* connectionClass;
};

typedef struct {
  ObjMap* collections;
} DbMemoryHandle;

static Value runtimeErrorValue(VM* vm, const char* message) {
  Token token;
  memset(&token, 0, sizeof(Token));
  runtimeError(vm, token, message);
  return NULL_VAL;
}

static ObjInstance* makeModule(VM* vm, const char* name) {
  ObjString* className = copyString(vm, name);
  ObjMap* methods = newMap(vm);
  ObjClass* klass = newClass(vm, className, methods);
  return newInstance(vm, klass);
}

static void moduleAdd(VM* vm, ObjInstance* module, const char* name, NativeFn fn, int arity) {
  ObjString* fieldName = copyString(vm, name);
  ObjNative* native = newNative(vm, fn, arity, fieldName);
  mapSet(module->fields, fieldName, OBJ_VAL(native));
}

static DbState* dbStateEnsure(VM* vm) {
  if (vm->dbState) return vm->dbState;
  DbState* state = (DbState*)malloc(sizeof(DbState));
  if (!state) {
    fprintf(stderr, "Out of memory.\n");
    exit(1);
  }
  state->drivers = NULL;
  state->driverCount = 0;
  state->driverCapacity = 0;
  state->connections = NULL;
  state->connectionCount = 0;
  state->connectionCapacity = 0;
  state->nextId = 1;
  state->connectionClass = NULL;
  vm->dbState = state;
  return state;
}

static void dbStateAddDriver(DbState* state, const DbDriver* driver) {
  for (int i = 0; i < state->driverCount; i++) {
    if (strcmp(state->drivers[i]->name, driver->name) == 0) {
      state->drivers[i] = driver;
      return;
    }
  }
  if (state->driverCapacity < state->driverCount + 1) {
    int oldCap = state->driverCapacity;
    state->driverCapacity = GROW_CAPACITY(oldCap);
    state->drivers = (const DbDriver**)realloc(state->drivers,
                                               sizeof(DbDriver*) * (size_t)state->driverCapacity);
    if (!state->drivers) {
      fprintf(stderr, "Out of memory.\n");
      exit(1);
    }
  }
  state->drivers[state->driverCount++] = driver;
}

void dbRegisterDriver(VM* vm, const DbDriver* driver) {
  if (!vm || !driver || !driver->name) return;
  DbState* state = dbStateEnsure(vm);
  dbStateAddDriver(state, driver);
}

static const DbDriver* dbFindDriver(DbState* state, const char* name) {
  if (!state || !name) return NULL;
  for (int i = 0; i < state->driverCount; i++) {
    if (strcmp(state->drivers[i]->name, name) == 0) {
      return state->drivers[i];
    }
  }
  return NULL;
}

static void dbStateAddConnection(DbState* state, DbConnection* connection) {
  if (state->connectionCapacity < state->connectionCount + 1) {
    int oldCap = state->connectionCapacity;
    state->connectionCapacity = GROW_CAPACITY(oldCap);
    state->connections = (DbConnection**)realloc(state->connections,
                                                 sizeof(DbConnection*) * (size_t)state->connectionCapacity);
    if (!state->connections) {
      fprintf(stderr, "Out of memory.\n");
      exit(1);
    }
  }
  state->connections[state->connectionCount++] = connection;
}

static DbConnection* dbFindConnection(DbState* state, int id) {
  if (!state || id <= 0) return NULL;
  for (int i = 0; i < state->connectionCount; i++) {
    if (state->connections[i]->id == id) {
      return state->connections[i];
    }
  }
  return NULL;
}

static ObjInstance* dbExpectInstance(VM* vm, Value value, const char* message) {
  if (!isObjType(value, OBJ_INSTANCE)) {
    runtimeErrorValue(vm, message);
    return NULL;
  }
  return (ObjInstance*)AS_OBJ(value);
}

static ObjString* dbExpectString(VM* vm, Value value, const char* message) {
  if (!isObjType(value, OBJ_STRING)) {
    runtimeErrorValue(vm, message);
    return NULL;
  }
  return (ObjString*)AS_OBJ(value);
}

static ObjMap* dbExpectMap(VM* vm, Value value, const char* message) {
  if (!isObjType(value, OBJ_MAP)) {
    runtimeErrorValue(vm, message);
    return NULL;
  }
  return (ObjMap*)AS_OBJ(value);
}

static ObjMap* dbMaybeMap(Value value) {
  if (IS_NULL(value)) return NULL;
  if (!isObjType(value, OBJ_MAP)) return NULL;
  return (ObjMap*)AS_OBJ(value);
}
static bool dbParseScheme(const char* uri, char* out, size_t outSize) {
  const char* sep = strstr(uri, "://");
  if (!sep) return false;
  size_t length = (size_t)(sep - uri);
  if (length == 0 || length >= outSize) return false;
  for (size_t i = 0; i < length; i++) {
    out[i] = (char)tolower((unsigned char)uri[i]);
  }
  out[length] = '\0';
  return true;
}

static const char* dbNormalizeScheme(const char* scheme) {
  if (strcmp(scheme, "postgresql") == 0) return "postgres";
  if (strcmp(scheme, "mongodb") == 0) return "mongo";
  return scheme;
}

static ObjMap* dbCloneMap(VM* vm, ObjMap* source) {
  ObjMap* copy = newMap(vm);
  if (!source) return copy;
  for (int i = 0; i < source->capacity; i++) {
    if (!source->entries[i].key) continue;
    mapSet(copy, source->entries[i].key, source->entries[i].value);
  }
  return copy;
}

static bool dbMemoryRowMatches(ObjMap* row, ObjMap* query) {
  if (!query || mapCount(query) == 0) return true;
  for (int i = 0; i < query->capacity; i++) {
    if (!query->entries[i].key) continue;
    Value existing;
    if (!mapGet(row, query->entries[i].key, &existing)) return false;
    if (!valuesEqual(existing, query->entries[i].value)) return false;
  }
  return true;
}

static ObjArray* dbMemoryGetCollection(VM* vm, DbMemoryHandle* handle,
                                       const char* name, bool create) {
  ObjString* key = copyString(vm, name);
  Value value;
  if (mapGet(handle->collections, key, &value)) {
    if (isObjType(value, OBJ_ARRAY)) {
      return (ObjArray*)AS_OBJ(value);
    }
  }
  if (!create) return NULL;
  ObjArray* array = newArray(vm);
  mapSet(handle->collections, key, OBJ_VAL(array));
  return array;
}

static bool dbOptionNumber(ObjMap* options, const char* name, double* out) {
  if (!options) return false;
  ObjString* key = copyString(options->vm, name);
  Value value;
  if (!mapGet(options, key, &value)) return false;
  if (!IS_NUMBER(value)) return false;
  *out = AS_NUMBER(value);
  return true;
}

static bool dbOptionBool(ObjMap* options, const char* name, bool* out) {
  if (!options) return false;
  ObjString* key = copyString(options->vm, name);
  Value value;
  if (!mapGet(options, key, &value)) return false;
  if (!IS_BOOL(value)) return false;
  *out = AS_BOOL(value);
  return true;
}

static bool dbMemoryConnect(VM* vm, const char* uri, ObjMap* options,
                            void** outHandle, char* error, size_t errorSize) {
  (void)uri;
  (void)options;
  DbMemoryHandle* handle = (DbMemoryHandle*)malloc(sizeof(DbMemoryHandle));
  if (!handle) {
    snprintf(error, errorSize, "db.connect out of memory.");
    return false;
  }
  handle->collections = newMap(vm);
  *outHandle = handle;
  return true;
}

static void dbMemoryClose(VM* vm, void* handle) {
  (void)vm;
  DbMemoryHandle* mem = (DbMemoryHandle*)handle;
  free(mem);
}

static bool dbMemoryInsert(VM* vm, void* handle, const char* collection,
                           ObjMap* doc, Value* out, char* error, size_t errorSize) {
  (void)error;
  (void)errorSize;
  DbMemoryHandle* mem = (DbMemoryHandle*)handle;
  ObjArray* rows = dbMemoryGetCollection(vm, mem, collection, true);
  if (!rows) return false;
  ObjMap* stored = dbCloneMap(vm, doc);
  arrayWrite(rows, OBJ_VAL(stored));
  if (out) {
    ObjMap* result = dbCloneMap(vm, stored);
    *out = OBJ_VAL(result);
  }
  return true;
}

static bool dbMemoryFind(VM* vm, void* handle, const char* collection,
                         ObjMap* query, ObjMap* options, ObjArray** out,
                         char* error, size_t errorSize) {
  (void)error;
  (void)errorSize;
  DbMemoryHandle* mem = (DbMemoryHandle*)handle;
  ObjArray* rows = dbMemoryGetCollection(vm, mem, collection, false);
  ObjArray* results = newArray(vm);
  if (!rows) {
    *out = results;
    return true;
  }

  double limitValue = 0;
  double skipValue = 0;
  bool hasLimit = dbOptionNumber(options, "limit", &limitValue);
  bool hasSkip = dbOptionNumber(options, "skip", &skipValue);
  int limit = hasLimit ? (int)limitValue : -1;
  int skip = hasSkip ? (int)skipValue : 0;
  if (skip < 0) skip = 0;

  int matched = 0;
  for (int i = 0; i < rows->count; i++) {
    Value rowValue = rows->items[i];
    if (!isObjType(rowValue, OBJ_MAP)) continue;
    ObjMap* row = (ObjMap*)AS_OBJ(rowValue);
    if (!dbMemoryRowMatches(row, query)) continue;
    if (skip > 0) {
      skip--;
      continue;
    }
    ObjMap* clone = dbCloneMap(vm, row);
    arrayWrite(results, OBJ_VAL(clone));
    matched++;
    if (limit >= 0 && matched >= limit) break;
  }

  *out = results;
  return true;
}

static bool dbMemoryUpdate(VM* vm, void* handle, const char* collection,
                           ObjMap* query, ObjMap* update, ObjMap* options,
                           int* outCount, char* error, size_t errorSize) {
  (void)vm;
  (void)error;
  (void)errorSize;
  DbMemoryHandle* mem = (DbMemoryHandle*)handle;
  ObjArray* rows = dbMemoryGetCollection(vm, mem, collection, false);
  if (!rows) {
    *outCount = 0;
    return true;
  }
  bool multi = true;
  dbOptionBool(options, "multi", &multi);
  int updated = 0;
  for (int i = 0; i < rows->count; i++) {
    Value rowValue = rows->items[i];
    if (!isObjType(rowValue, OBJ_MAP)) continue;
    ObjMap* row = (ObjMap*)AS_OBJ(rowValue);
    if (!dbMemoryRowMatches(row, query)) continue;
    for (int j = 0; j < update->capacity; j++) {
      if (!update->entries[j].key) continue;
      mapSet(row, update->entries[j].key, update->entries[j].value);
    }
    updated++;
    if (!multi) break;
  }
  *outCount = updated;
  return true;
}

static bool dbMemoryRemove(VM* vm, void* handle, const char* collection,
                           ObjMap* query, ObjMap* options, int* outCount,
                           char* error, size_t errorSize) {
  (void)vm;
  (void)error;
  (void)errorSize;
  DbMemoryHandle* mem = (DbMemoryHandle*)handle;
  ObjArray* rows = dbMemoryGetCollection(vm, mem, collection, false);
  if (!rows) {
    *outCount = 0;
    return true;
  }
  bool multi = true;
  dbOptionBool(options, "multi", &multi);
  int removed = 0;
  int write = 0;
  for (int i = 0; i < rows->count; i++) {
    Value rowValue = rows->items[i];
    bool keep = true;
    if (isObjType(rowValue, OBJ_MAP)) {
      ObjMap* row = (ObjMap*)AS_OBJ(rowValue);
      if (dbMemoryRowMatches(row, query)) {
        if (multi || removed == 0) {
          keep = false;
          removed++;
        }
      }
    }
    if (keep) {
      rows->items[write++] = rowValue;
    }
  }
  rows->count = write;
  *outCount = removed;
  return true;
}

static const DbDriver DB_MEMORY_DRIVER = {
  "memory",
  DB_KIND_DOCUMENT,
  DB_PARAM_QMARK,
  dbMemoryConnect,
  dbMemoryClose,
  NULL,
  dbMemoryInsert,
  dbMemoryFind,
  dbMemoryUpdate,
  dbMemoryRemove
};

static bool dbStubConnect(VM* vm, const char* uri, ObjMap* options, void** outHandle,
                          char* error, size_t errorSize, const char* name,
                          const char* hint) {
  (void)vm;
  (void)uri;
  (void)options;
  (void)outHandle;
  if (hint) {
    snprintf(error, errorSize, "%s driver not available. %s", name, hint);
  } else {
    snprintf(error, errorSize, "%s driver not available.", name);
  }
  return false;
}

static bool dbStubPostgresConnect(VM* vm, const char* uri, ObjMap* options,
                                  void** outHandle, char* error, size_t errorSize) {
  return dbStubConnect(vm, uri, options, outHandle, error, errorSize,
                       "postgres", "Build with ERKAO_DB_POSTGRES.");
}

static bool dbStubMySqlConnect(VM* vm, const char* uri, ObjMap* options,
                               void** outHandle, char* error, size_t errorSize) {
  return dbStubConnect(vm, uri, options, outHandle, error, errorSize,
                       "mysql", "Build with ERKAO_DB_MYSQL.");
}

static bool dbStubMongoConnect(VM* vm, const char* uri, ObjMap* options,
                               void** outHandle, char* error, size_t errorSize) {
  return dbStubConnect(vm, uri, options, outHandle, error, errorSize,
                       "mongo", "Build with ERKAO_DB_MONGO.");
}

static const DbDriver DB_POSTGRES_STUB = {
  "postgres",
  DB_KIND_SQL,
  DB_PARAM_DOLLAR,
  dbStubPostgresConnect,
  NULL,
  NULL,
  NULL,
  NULL,
  NULL,
  NULL
};

static const DbDriver DB_MYSQL_STUB = {
  "mysql",
  DB_KIND_SQL,
  DB_PARAM_QMARK,
  dbStubMySqlConnect,
  NULL,
  NULL,
  NULL,
  NULL,
  NULL,
  NULL
};

static const DbDriver DB_MONGO_STUB = {
  "mongo",
  DB_KIND_DOCUMENT,
  DB_PARAM_QMARK,
  dbStubMongoConnect,
  NULL,
  NULL,
  NULL,
  NULL,
  NULL,
  NULL
};
static bool dbSqlIdentValid(const char* name) {
  if (!name || name[0] == '\0') return false;
  for (const char* c = name; *c; c++) {
    if (*c == '.') continue;
    if (*c == '_' || (*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') ||
        (*c >= '0' && *c <= '9')) {
      continue;
    }
    return false;
  }
  return true;
}

typedef struct {
  char* data;
  size_t length;
  size_t capacity;
} DbString;

static void dbStringInit(DbString* sb) {
  sb->data = NULL;
  sb->length = 0;
  sb->capacity = 0;
}

static void dbStringEnsure(DbString* sb, size_t needed) {
  if (sb->capacity >= needed) return;
  size_t newCap = sb->capacity == 0 ? 64 : sb->capacity;
  while (newCap < needed) newCap *= 2;
  char* next = (char*)realloc(sb->data, newCap);
  if (!next) {
    fprintf(stderr, "Out of memory.\n");
    exit(1);
  }
  sb->data = next;
  sb->capacity = newCap;
}

static void dbStringAppend(DbString* sb, const char* text) {
  size_t length = strlen(text);
  dbStringEnsure(sb, sb->length + length + 1);
  memcpy(sb->data + sb->length, text, length);
  sb->length += length;
  sb->data[sb->length] = '\0';
}

static void dbStringAppendN(DbString* sb, const char* text, size_t length) {
  dbStringEnsure(sb, sb->length + length + 1);
  memcpy(sb->data + sb->length, text, length);
  sb->length += length;
  sb->data[sb->length] = '\0';
}

typedef struct {
  DbParamStyle style;
  int paramIndex;
  ObjArray* params;
  DbString sql;
} DbSqlBuilder;

static void dbSqlBuilderInit(VM* vm, DbSqlBuilder* builder, DbParamStyle style) {
  builder->style = style;
  builder->paramIndex = 0;
  builder->params = newArray(vm);
  dbStringInit(&builder->sql);
}

static void dbSqlBuilderFree(DbSqlBuilder* builder) {
  free(builder->sql.data);
  builder->sql.data = NULL;
  builder->sql.length = 0;
  builder->sql.capacity = 0;
}

static void dbSqlAppendPlaceholder(DbSqlBuilder* builder) {
  builder->paramIndex++;
  if (builder->style == DB_PARAM_DOLLAR) {
    char buffer[16];
    int length = snprintf(buffer, sizeof(buffer), "$%d", builder->paramIndex);
    if (length < 0) length = 0;
    dbStringAppendN(&builder->sql, buffer, (size_t)length);
  } else {
    dbStringAppend(&builder->sql, "?");
  }
}

static void dbSqlAddParam(DbSqlBuilder* builder, Value value) {
  arrayWrite(builder->params, value);
  dbSqlAppendPlaceholder(builder);
}

static bool dbSqlAppendWhere(DbSqlBuilder* builder, ObjMap* query, char* error, size_t errorSize) {
  if (!query || mapCount(query) == 0) return true;
  dbStringAppend(&builder->sql, " WHERE ");
  int clauses = 0;
  for (int i = 0; i < query->capacity; i++) {
    if (!query->entries[i].key) continue;
    ObjString* key = query->entries[i].key;
    if (!dbSqlIdentValid(key->chars)) {
      snprintf(error, errorSize, "Invalid column name '%s'.", key->chars);
      return false;
    }
    if (clauses > 0) dbStringAppend(&builder->sql, " AND ");
    dbStringAppend(&builder->sql, key->chars);
    Value value = query->entries[i].value;
    if (IS_NULL(value)) {
      dbStringAppend(&builder->sql, " IS NULL");
    } else {
      dbStringAppend(&builder->sql, " = ");
      dbSqlAddParam(builder, value);
    }
    clauses++;
  }
  return true;
}

static bool dbSqlBuildInsert(VM* vm, DbParamStyle style, const char* table,
                             ObjMap* data, DbSqlBuilder* out,
                             char* error, size_t errorSize) {
  if (!dbSqlIdentValid(table)) {
    snprintf(error, errorSize, "Invalid table name '%s'.", table);
    return false;
  }
  if (!data || mapCount(data) == 0) {
    snprintf(error, errorSize, "db.insert expects a non-empty map.");
    return false;
  }
  dbSqlBuilderInit(vm, out, style);
  dbStringAppend(&out->sql, "INSERT INTO ");
  dbStringAppend(&out->sql, table);
  dbStringAppend(&out->sql, " (");
  int cols = 0;
  for (int i = 0; i < data->capacity; i++) {
    if (!data->entries[i].key) continue;
    if (cols > 0) dbStringAppend(&out->sql, ", ");
    if (!dbSqlIdentValid(data->entries[i].key->chars)) {
      snprintf(error, errorSize, "Invalid column name '%s'.", data->entries[i].key->chars);
      dbSqlBuilderFree(out);
      return false;
    }
    dbStringAppend(&out->sql, data->entries[i].key->chars);
    cols++;
  }
  dbStringAppend(&out->sql, ") VALUES (");
  int vals = 0;
  for (int i = 0; i < data->capacity; i++) {
    if (!data->entries[i].key) continue;
    if (vals > 0) dbStringAppend(&out->sql, ", ");
    dbSqlAddParam(out, data->entries[i].value);
    vals++;
  }
  dbStringAppend(&out->sql, ")");
  return true;
}

static bool dbSqlBuildSelect(VM* vm, DbParamStyle style, const char* table,
                             ObjMap* query, ObjMap* options,
                             DbSqlBuilder* out, char* error, size_t errorSize) {
  if (!dbSqlIdentValid(table)) {
    snprintf(error, errorSize, "Invalid table name '%s'.", table);
    return false;
  }
  dbSqlBuilderInit(vm, out, style);
  dbStringAppend(&out->sql, "SELECT * FROM ");
  dbStringAppend(&out->sql, table);
  if (!dbSqlAppendWhere(out, query, error, errorSize)) {
    dbSqlBuilderFree(out);
    return false;
  }
  double limitValue = 0;
  double offsetValue = 0;
  if (dbOptionNumber(options, "limit", &limitValue)) {
    if (limitValue >= 0) {
      dbStringAppend(&out->sql, " LIMIT ");
      dbSqlAddParam(out, NUMBER_VAL(limitValue));
    }
  }
  if (dbOptionNumber(options, "offset", &offsetValue)) {
    if (offsetValue >= 0) {
      dbStringAppend(&out->sql, " OFFSET ");
      dbSqlAddParam(out, NUMBER_VAL(offsetValue));
    }
  }
  return true;
}

static bool dbSqlBuildUpdate(VM* vm, DbParamStyle style, const char* table,
                             ObjMap* query, ObjMap* update,
                             DbSqlBuilder* out, char* error, size_t errorSize) {
  if (!dbSqlIdentValid(table)) {
    snprintf(error, errorSize, "Invalid table name '%s'.", table);
    return false;
  }
  if (!update || mapCount(update) == 0) {
    snprintf(error, errorSize, "db.update expects a non-empty update map.");
    return false;
  }
  dbSqlBuilderInit(vm, out, style);
  dbStringAppend(&out->sql, "UPDATE ");
  dbStringAppend(&out->sql, table);
  dbStringAppend(&out->sql, " SET ");
  int cols = 0;
  for (int i = 0; i < update->capacity; i++) {
    if (!update->entries[i].key) continue;
    if (cols > 0) dbStringAppend(&out->sql, ", ");
    if (!dbSqlIdentValid(update->entries[i].key->chars)) {
      snprintf(error, errorSize, "Invalid column name '%s'.", update->entries[i].key->chars);
      dbSqlBuilderFree(out);
      return false;
    }
    dbStringAppend(&out->sql, update->entries[i].key->chars);
    dbStringAppend(&out->sql, " = ");
    dbSqlAddParam(out, update->entries[i].value);
    cols++;
  }
  if (!dbSqlAppendWhere(out, query, error, errorSize)) {
    dbSqlBuilderFree(out);
    return false;
  }
  return true;
}

static bool dbSqlBuildDelete(VM* vm, DbParamStyle style, const char* table,
                             ObjMap* query, DbSqlBuilder* out,
                             char* error, size_t errorSize) {
  if (!dbSqlIdentValid(table)) {
    snprintf(error, errorSize, "Invalid table name '%s'.", table);
    return false;
  }
  dbSqlBuilderInit(vm, out, style);
  dbStringAppend(&out->sql, "DELETE FROM ");
  dbStringAppend(&out->sql, table);
  if (!dbSqlAppendWhere(out, query, error, errorSize)) {
    dbSqlBuilderFree(out);
    return false;
  }
  return true;
}

static Value dbExecResultToValue(VM* vm, const DbExecResult* result) {
  ObjMap* map = newMap(vm);
  ObjString* rowsKey = copyString(vm, "rows");
  ObjString* affectedKey = copyString(vm, "affected");
  if (result && result->rows) {
    mapSet(map, rowsKey, OBJ_VAL(result->rows));
  } else {
    mapSet(map, rowsKey, NULL_VAL);
  }
  if (result && result->affected >= 0) {
    mapSet(map, affectedKey, NUMBER_VAL((double)result->affected));
  } else {
    mapSet(map, affectedKey, NULL_VAL);
  }
  return OBJ_VAL(map);
}

static DbConnection* dbConnectionFromValue(VM* vm, Value value, ObjInstance** outInstance) {
  DbState* state = dbStateEnsure(vm);
  ObjInstance* instance = dbExpectInstance(vm, value, "db expects a connection instance.");
  if (!instance) return NULL;
  if (state->connectionClass && instance->klass != state->connectionClass) {
    runtimeErrorValue(vm, "db expects a connection instance.");
    return NULL;
  }
  ObjString* idKey = copyString(vm, "id");
  Value idValue;
  if (!mapGet(instance->fields, idKey, &idValue) || !IS_NUMBER(idValue)) {
    runtimeErrorValue(vm, "db expects a connection instance.");
    return NULL;
  }
  int id = (int)AS_NUMBER(idValue);
  DbConnection* conn = dbFindConnection(state, id);
  if (!conn || !conn->open) {
    runtimeErrorValue(vm, "db connection is closed.");
    return NULL;
  }
  if (outInstance) *outInstance = instance;
  return conn;
}
static Value nativeDbConnect(VM* vm, int argc, Value* args) {
  if (argc < 1 || argc > 2) {
    return runtimeErrorValue(vm, "db.connect expects (uri[, options]).");
  }
  ObjString* uri = dbExpectString(vm, args[0], "db.connect expects a uri string.");
  if (!uri) return NULL_VAL;
  ObjMap* options = NULL;
  if (argc == 2) {
    if (!IS_NULL(args[1]) && !isObjType(args[1], OBJ_MAP)) {
      return runtimeErrorValue(vm, "db.connect expects options to be a map or null.");
    }
    options = dbMaybeMap(args[1]);
  }

  char scheme[32];
  if (!dbParseScheme(uri->chars, scheme, sizeof(scheme))) {
    return runtimeErrorValue(vm, "db.connect expects a uri like driver://...");
  }
  const char* driverName = dbNormalizeScheme(scheme);
  DbState* state = dbStateEnsure(vm);
  const DbDriver* driver = dbFindDriver(state, driverName);
  if (!driver) {
    return runtimeErrorValue(vm, "db.connect unknown driver.");
  }

  void* handle = NULL;
  char error[256] = {0};
  if (!driver->connect || !driver->connect(vm, uri->chars, options, &handle, error, sizeof(error))) {
    return runtimeErrorValue(vm, error[0] ? error : "db.connect failed.");
  }

  DbConnection* conn = (DbConnection*)malloc(sizeof(DbConnection));
  if (!conn) {
    if (driver->close) driver->close(vm, handle);
    return runtimeErrorValue(vm, "db.connect out of memory.");
  }
  conn->id = state->nextId++;
  conn->driver = driver;
  conn->handle = handle;
  conn->open = true;
  dbStateAddConnection(state, conn);

  if (!state->connectionClass) {
    ObjString* name = copyString(vm, "DbConnection");
    ObjClass* klass = newClass(vm, name, newMap(vm));
    state->connectionClass = klass;
  }

  ObjInstance* instance = newInstance(vm, state->connectionClass);
  mapSet(instance->fields, copyString(vm, "id"), NUMBER_VAL((double)conn->id));
  mapSet(instance->fields, copyString(vm, "driver"), OBJ_VAL(copyString(vm, driver->name)));
  mapSet(instance->fields, copyString(vm, "kind"),
         OBJ_VAL(copyString(vm, driver->kind == DB_KIND_SQL ? "sql" : "document")));
  mapSet(instance->fields, copyString(vm, "closed"), BOOL_VAL(false));

  if (driver == &DB_MEMORY_DRIVER && handle) {
    DbMemoryHandle* mem = (DbMemoryHandle*)handle;
    if (mem->collections) {
      mapSet(instance->fields, copyString(vm, "store"), OBJ_VAL(mem->collections));
    }
  }

  return OBJ_VAL(instance);
}

static Value nativeDbClose(VM* vm, int argc, Value* args) {
  (void)argc;
  ObjInstance* instance = NULL;
  DbConnection* conn = dbConnectionFromValue(vm, args[0], &instance);
  if (!conn) return NULL_VAL;
  if (conn->driver && conn->driver->close) {
    conn->driver->close(vm, conn->handle);
  }
  conn->open = false;
  conn->handle = NULL;
  if (instance) {
    mapSet(instance->fields, copyString(vm, "closed"), BOOL_VAL(true));
  }
  return BOOL_VAL(true);
}

static Value nativeDbDrivers(VM* vm, int argc, Value* args) {
  (void)argc;
  (void)args;
  DbState* state = dbStateEnsure(vm);
  ObjArray* array = newArrayWithCapacity(vm, state->driverCount);
  for (int i = 0; i < state->driverCount; i++) {
    const DbDriver* driver = state->drivers[i];
    arrayWrite(array, OBJ_VAL(copyString(vm, driver->name)));
  }
  return OBJ_VAL(array);
}

static Value nativeDbSupports(VM* vm, int argc, Value* args) {
  (void)argc;
  ObjString* name = dbExpectString(vm, args[0], "db.supports expects a driver name string.");
  if (!name) return NULL_VAL;
  DbState* state = dbStateEnsure(vm);
  return BOOL_VAL(dbFindDriver(state, name->chars) != NULL);
}

static Value nativeDbInsert(VM* vm, int argc, Value* args) {
  (void)argc;
  DbConnection* conn = dbConnectionFromValue(vm, args[0], NULL);
  if (!conn) return NULL_VAL;
  ObjString* collection = dbExpectString(vm, args[1], "db.insert expects a collection name.");
  if (!collection) return NULL_VAL;
  ObjMap* doc = dbExpectMap(vm, args[2], "db.insert expects a map.");
  if (!doc) return NULL_VAL;
  char error[256] = {0};
  if (conn->driver->insert) {
    Value result = NULL_VAL;
    if (!conn->driver->insert(vm, conn->handle, collection->chars, doc, &result,
                              error, sizeof(error))) {
      return runtimeErrorValue(vm, error[0] ? error : "db.insert failed.");
    }
    return result;
  }

  if (conn->driver->exec && conn->driver->kind == DB_KIND_SQL) {
    DbSqlBuilder builder;
    if (!dbSqlBuildInsert(vm, conn->driver->paramStyle, collection->chars, doc,
                          &builder, error, sizeof(error))) {
      return runtimeErrorValue(vm, error);
    }
    DbExecResult execResult = { NULL, -1 };
    bool ok = conn->driver->exec(vm, conn->handle, builder.sql.data, builder.params,
                                 &execResult, error, sizeof(error));
    dbSqlBuilderFree(&builder);
    if (!ok) {
      return runtimeErrorValue(vm, error[0] ? error : "db.insert failed.");
    }
    return OBJ_VAL(dbCloneMap(vm, doc));
  }

  return runtimeErrorValue(vm, "db.insert not supported by this driver.");
}

static Value nativeDbFind(VM* vm, int argc, Value* args) {
  if (argc < 2 || argc > 4) {
    return runtimeErrorValue(vm, "db.find expects (conn, collection[, query[, options]]).");
  }
  DbConnection* conn = dbConnectionFromValue(vm, args[0], NULL);
  if (!conn) return NULL_VAL;
  ObjString* collection = dbExpectString(vm, args[1], "db.find expects a collection name.");
  if (!collection) return NULL_VAL;
  ObjMap* query = NULL;
  ObjMap* options = NULL;
  if (argc >= 3 && !IS_NULL(args[2])) {
    query = dbExpectMap(vm, args[2], "db.find expects query to be a map or null.");
    if (!query) return NULL_VAL;
  }
  if (argc == 4 && !IS_NULL(args[3])) {
    options = dbExpectMap(vm, args[3], "db.find expects options to be a map or null.");
    if (!options) return NULL_VAL;
  }
  char error[256] = {0};
  if (conn->driver->find) {
    ObjArray* results = NULL;
    if (!conn->driver->find(vm, conn->handle, collection->chars, query, options,
                             &results, error, sizeof(error))) {
      return runtimeErrorValue(vm, error[0] ? error : "db.find failed.");
    }
    return results ? OBJ_VAL(results) : OBJ_VAL(newArray(vm));
  }

  if (conn->driver->exec && conn->driver->kind == DB_KIND_SQL) {
    DbSqlBuilder builder;
    if (!dbSqlBuildSelect(vm, conn->driver->paramStyle, collection->chars, query, options,
                          &builder, error, sizeof(error))) {
      return runtimeErrorValue(vm, error);
    }
    DbExecResult execResult = { NULL, -1 };
    bool ok = conn->driver->exec(vm, conn->handle, builder.sql.data, builder.params,
                                 &execResult, error, sizeof(error));
    dbSqlBuilderFree(&builder);
    if (!ok) {
      return runtimeErrorValue(vm, error[0] ? error : "db.find failed.");
    }
    if (!execResult.rows) execResult.rows = newArray(vm);
    return OBJ_VAL(execResult.rows);
  }

  return runtimeErrorValue(vm, "db.find not supported by this driver.");
}

static Value nativeDbUpdate(VM* vm, int argc, Value* args) {
  if (argc < 4 || argc > 5) {
    return runtimeErrorValue(vm, "db.update expects (conn, collection, query, update[, options]).");
  }
  DbConnection* conn = dbConnectionFromValue(vm, args[0], NULL);
  if (!conn) return NULL_VAL;
  ObjString* collection = dbExpectString(vm, args[1], "db.update expects a collection name.");
  if (!collection) return NULL_VAL;
  ObjMap* query = dbExpectMap(vm, args[2], "db.update expects a query map.");
  if (!query) return NULL_VAL;
  ObjMap* update = dbExpectMap(vm, args[3], "db.update expects an update map.");
  if (!update) return NULL_VAL;
  ObjMap* options = NULL;
  if (argc == 5 && !IS_NULL(args[4])) {
    options = dbExpectMap(vm, args[4], "db.update expects options to be a map or null.");
    if (!options) return NULL_VAL;
  }
  char error[256] = {0};
  if (conn->driver->update) {
    int updated = 0;
    if (!conn->driver->update(vm, conn->handle, collection->chars, query, update,
                              options, &updated, error, sizeof(error))) {
      return runtimeErrorValue(vm, error[0] ? error : "db.update failed.");
    }
    return NUMBER_VAL((double)updated);
  }

  if (conn->driver->exec && conn->driver->kind == DB_KIND_SQL) {
    DbSqlBuilder builder;
    if (!dbSqlBuildUpdate(vm, conn->driver->paramStyle, collection->chars, query, update,
                          &builder, error, sizeof(error))) {
      return runtimeErrorValue(vm, error);
    }
    DbExecResult execResult = { NULL, -1 };
    bool ok = conn->driver->exec(vm, conn->handle, builder.sql.data, builder.params,
                                 &execResult, error, sizeof(error));
    dbSqlBuilderFree(&builder);
    if (!ok) {
      return runtimeErrorValue(vm, error[0] ? error : "db.update failed.");
    }
    int affected = execResult.affected >= 0 ? execResult.affected : 0;
    return NUMBER_VAL((double)affected);
  }

  return runtimeErrorValue(vm, "db.update not supported by this driver.");
}

static Value nativeDbDelete(VM* vm, int argc, Value* args) {
  if (argc < 3 || argc > 4) {
    return runtimeErrorValue(vm, "db.delete expects (conn, collection, query[, options]).");
  }
  DbConnection* conn = dbConnectionFromValue(vm, args[0], NULL);
  if (!conn) return NULL_VAL;
  ObjString* collection = dbExpectString(vm, args[1], "db.delete expects a collection name.");
  if (!collection) return NULL_VAL;
  ObjMap* query = dbExpectMap(vm, args[2], "db.delete expects a query map.");
  if (!query) return NULL_VAL;
  ObjMap* options = NULL;
  if (argc == 4 && !IS_NULL(args[3])) {
    options = dbExpectMap(vm, args[3], "db.delete expects options to be a map or null.");
    if (!options) return NULL_VAL;
  }
  char error[256] = {0};
  if (conn->driver->remove) {
    int removed = 0;
    if (!conn->driver->remove(vm, conn->handle, collection->chars, query,
                              options, &removed, error, sizeof(error))) {
      return runtimeErrorValue(vm, error[0] ? error : "db.delete failed.");
    }
    return NUMBER_VAL((double)removed);
  }

  if (conn->driver->exec && conn->driver->kind == DB_KIND_SQL) {
    DbSqlBuilder builder;
    if (!dbSqlBuildDelete(vm, conn->driver->paramStyle, collection->chars, query,
                          &builder, error, sizeof(error))) {
      return runtimeErrorValue(vm, error);
    }
    DbExecResult execResult = { NULL, -1 };
    bool ok = conn->driver->exec(vm, conn->handle, builder.sql.data, builder.params,
                                 &execResult, error, sizeof(error));
    dbSqlBuilderFree(&builder);
    if (!ok) {
      return runtimeErrorValue(vm, error[0] ? error : "db.delete failed.");
    }
    int affected = execResult.affected >= 0 ? execResult.affected : 0;
    return NUMBER_VAL((double)affected);
  }

  return runtimeErrorValue(vm, "db.delete not supported by this driver.");
}

static Value nativeDbExec(VM* vm, int argc, Value* args) {
  if (argc < 2 || argc > 3) {
    return runtimeErrorValue(vm, "db.exec expects (conn, sql[, params]).");
  }
  DbConnection* conn = dbConnectionFromValue(vm, args[0], NULL);
  if (!conn) return NULL_VAL;
  ObjString* sql = dbExpectString(vm, args[1], "db.exec expects a sql string.");
  if (!sql) return NULL_VAL;
  ObjArray* params = NULL;
  if (argc == 3 && !IS_NULL(args[2])) {
    if (!isObjType(args[2], OBJ_ARRAY)) {
      return runtimeErrorValue(vm, "db.exec expects params to be an array or null.");
    }
    params = (ObjArray*)AS_OBJ(args[2]);
  } else {
    params = newArray(vm);
  }
  if (!conn->driver->exec) {
    return runtimeErrorValue(vm, "db.exec not supported by this driver.");
  }
  DbExecResult result = { NULL, -1 };
  char error[256] = {0};
  if (!conn->driver->exec(vm, conn->handle, sql->chars, params, &result,
                          error, sizeof(error))) {
    return runtimeErrorValue(vm, error[0] ? error : "db.exec failed.");
  }
  return dbExecResultToValue(vm, &result);
}

void dbShutdown(VM* vm) {
  if (!vm || !vm->dbState) return;
  DbState* state = vm->dbState;
  for (int i = 0; i < state->connectionCount; i++) {
    DbConnection* conn = state->connections[i];
    if (conn && conn->open && conn->driver && conn->driver->close) {
      conn->driver->close(vm, conn->handle);
    }
    free(conn);
  }
  free(state->connections);
  free(state->drivers);
  free(state);
  vm->dbState = NULL;
}

void dbRegisterPostgresDriver(VM* vm);
void dbRegisterMysqlDriver(VM* vm);
void dbRegisterMongoDriver(VM* vm);

void defineDbModule(VM* vm) {
  DbState* state = dbStateEnsure(vm);
  if (!state->connectionClass) {
    ObjString* name = copyString(vm, "DbConnection");
    state->connectionClass = newClass(vm, name, newMap(vm));
  }

  dbRegisterDriver(vm, &DB_MEMORY_DRIVER);
  dbRegisterDriver(vm, &DB_POSTGRES_STUB);
  dbRegisterDriver(vm, &DB_MYSQL_STUB);
  dbRegisterDriver(vm, &DB_MONGO_STUB);

  dbRegisterPostgresDriver(vm);
  dbRegisterMysqlDriver(vm);
  dbRegisterMongoDriver(vm);

  ObjInstance* module = makeModule(vm, "db");
  moduleAdd(vm, module, "connect", nativeDbConnect, -1);
  moduleAdd(vm, module, "close", nativeDbClose, 1);
  moduleAdd(vm, module, "drivers", nativeDbDrivers, 0);
  moduleAdd(vm, module, "supports", nativeDbSupports, 1);
  moduleAdd(vm, module, "insert", nativeDbInsert, 3);
  moduleAdd(vm, module, "find", nativeDbFind, -1);
  moduleAdd(vm, module, "update", nativeDbUpdate, -1);
  moduleAdd(vm, module, "delete", nativeDbDelete, -1);
  moduleAdd(vm, module, "exec", nativeDbExec, -1);
  mapSet(module->fields, copyString(vm, "Connection"), OBJ_VAL(state->connectionClass));
  defineGlobal(vm, "db", OBJ_VAL(module));
}

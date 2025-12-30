#include "db.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if ERKAO_DB_MONGO
#include <bson/bson.h>
#include <mongoc/mongoc.h>
#endif

#if ERKAO_DB_MONGO
typedef struct {
  mongoc_client_t* client;
  char* database;
} MongoHandle;

static char* mongoStrndup(const char* start, size_t length) {
  char* out = (char*)malloc(length + 1);
  if (!out) return NULL;
  memcpy(out, start, length);
  out[length] = '\0';
  return out;
}

static ObjMap* mongoCloneMap(VM* vm, ObjMap* source) {
  ObjMap* copy = newMap(vm);
  if (!source) return copy;
  for (int i = 0; i < source->capacity; i++) {
    if (!source->entries[i].key) continue;
    mapSet(copy, source->entries[i].key, source->entries[i].value);
  }
  return copy;
}

static bool mongoAppendValue(bson_t* doc, const char* key, Value value);

static bool mongoAppendArray(bson_t* doc, const char* key, ObjArray* array) {
  bson_t child;
  if (!bson_append_array_begin(doc, key, -1, &child)) return false;
  char indexKey[16];
  for (int i = 0; i < array->count; i++) {
    snprintf(indexKey, sizeof(indexKey), "%d", i);
    if (!mongoAppendValue(&child, indexKey, array->items[i])) {
      bson_append_array_end(doc, &child);
      return false;
    }
  }
  return bson_append_array_end(doc, &child);
}

static bool mongoAppendMap(bson_t* doc, const char* key, ObjMap* map) {
  bson_t child;
  if (!bson_append_document_begin(doc, key, -1, &child)) return false;
  for (int i = 0; i < map->capacity; i++) {
    if (!map->entries[i].key) continue;
    if (!mongoAppendValue(&child, map->entries[i].key->chars, map->entries[i].value)) {
      bson_append_document_end(doc, &child);
      return false;
    }
  }
  return bson_append_document_end(doc, &child);
}

static bool mongoAppendValue(bson_t* doc, const char* key, Value value) {
  if (IS_NULL(value)) {
    return bson_append_null(doc, key, -1);
  }
  if (IS_BOOL(value)) {
    return bson_append_bool(doc, key, -1, AS_BOOL(value));
  }
  if (IS_NUMBER(value)) {
    return bson_append_double(doc, key, -1, AS_NUMBER(value));
  }
  if (isObjType(value, OBJ_STRING)) {
    ObjString* str = (ObjString*)AS_OBJ(value);
    return bson_append_utf8(doc, key, -1, str->chars, str->length);
  }
  if (isObjType(value, OBJ_ARRAY)) {
    return mongoAppendArray(doc, key, (ObjArray*)AS_OBJ(value));
  }
  if (isObjType(value, OBJ_MAP)) {
    return mongoAppendMap(doc, key, (ObjMap*)AS_OBJ(value));
  }
  return false;
}

static Value mongoValueFromBson(VM* vm, bson_iter_t* iter);

static Value mongoArrayFromIter(VM* vm, bson_iter_t* iter) {
  bson_iter_t child;
  ObjArray* array = newArray(vm);
  if (bson_iter_recurse(iter, &child)) {
    while (bson_iter_next(&child)) {
      Value value = mongoValueFromBson(vm, &child);
      arrayWrite(array, value);
    }
  }
  return OBJ_VAL(array);
}

static Value mongoMapFromIter(VM* vm, bson_iter_t* iter) {
  bson_iter_t child;
  ObjMap* map = newMap(vm);
  if (bson_iter_recurse(iter, &child)) {
    while (bson_iter_next(&child)) {
      ObjString* key = copyString(vm, bson_iter_key(&child));
      Value value = mongoValueFromBson(vm, &child);
      mapSet(map, key, value);
    }
  }
  return OBJ_VAL(map);
}

static Value mongoValueFromBson(VM* vm, bson_iter_t* iter) {
  switch (bson_iter_type(iter)) {
    case BSON_TYPE_NULL:
      return NULL_VAL;
    case BSON_TYPE_BOOL:
      return BOOL_VAL(bson_iter_bool(iter));
    case BSON_TYPE_INT32:
      return NUMBER_VAL((double)bson_iter_int32(iter));
    case BSON_TYPE_INT64:
      return NUMBER_VAL((double)bson_iter_int64(iter));
    case BSON_TYPE_DOUBLE:
      return NUMBER_VAL(bson_iter_double(iter));
    case BSON_TYPE_UTF8: {
      uint32_t length = 0;
      const char* text = bson_iter_utf8(iter, &length);
      return OBJ_VAL(copyStringWithLength(vm, text, (int)length));
    }
    case BSON_TYPE_OID: {
      char buffer[25];
      bson_oid_to_string(bson_iter_oid(iter), buffer);
      return OBJ_VAL(copyString(vm, buffer));
    }
    case BSON_TYPE_DOCUMENT:
      return mongoMapFromIter(vm, iter);
    case BSON_TYPE_ARRAY:
      return mongoArrayFromIter(vm, iter);
    default:
      return NULL_VAL;
  }
}

static ObjMap* mongoMapFromBson(VM* vm, const bson_t* doc) {
  ObjMap* map = newMap(vm);
  bson_iter_t iter;
  if (bson_iter_init(&iter, doc)) {
    while (bson_iter_next(&iter)) {
      ObjString* key = copyString(vm, bson_iter_key(&iter));
      Value value = mongoValueFromBson(vm, &iter);
      mapSet(map, key, value);
    }
  }
  return map;
}

static int mongoReplyInt(const bson_t* reply, const char* key, const char* altKey) {
  bson_iter_t iter;
  if (bson_iter_init_find(&iter, reply, key)) {
    if (BSON_ITER_HOLDS_INT32(&iter)) return bson_iter_int32(&iter);
    if (BSON_ITER_HOLDS_INT64(&iter)) return (int)bson_iter_int64(&iter);
    if (BSON_ITER_HOLDS_DOUBLE(&iter)) return (int)bson_iter_double(&iter);
  }
  if (altKey && bson_iter_init_find(&iter, reply, altKey)) {
    if (BSON_ITER_HOLDS_INT32(&iter)) return bson_iter_int32(&iter);
    if (BSON_ITER_HOLDS_INT64(&iter)) return (int)bson_iter_int64(&iter);
    if (BSON_ITER_HOLDS_DOUBLE(&iter)) return (int)bson_iter_double(&iter);
  }
  return 0;
}

static bool mongoConnect(VM* vm, const char* uri, ObjMap* options,
                         void** outHandle, char* error, size_t errorSize) {
  (void)vm;
  (void)options;
  mongoc_uri_t* parsed = mongoc_uri_new(uri);
  if (!parsed) {
    snprintf(error, errorSize, "mongo uri invalid.");
    return false;
  }
  mongoc_client_t* client = mongoc_client_new_from_uri(parsed);
  const char* db = mongoc_uri_get_database(parsed);
  if (!db || db[0] == '\0') {
    db = "test";
  }
  MongoHandle* handle = (MongoHandle*)malloc(sizeof(MongoHandle));
  if (!handle) {
    snprintf(error, errorSize, "mongo connect out of memory.");
    mongoc_uri_destroy(parsed);
    mongoc_client_destroy(client);
    return false;
  }
  handle->client = client;
  handle->database = mongoStrndup(db, strlen(db));
  mongoc_uri_destroy(parsed);
  if (!handle->database) {
    snprintf(error, errorSize, "mongo connect out of memory.");
    mongoc_client_destroy(client);
    free(handle);
    return false;
  }
  *outHandle = handle;
  return true;
}

static void mongoClose(VM* vm, void* handle) {
  (void)vm;
  MongoHandle* mh = (MongoHandle*)handle;
  if (!mh) return;
  if (mh->client) mongoc_client_destroy(mh->client);
  free(mh->database);
  free(mh);
}

static bool mongoInsert(VM* vm, void* handle, const char* collection,
                        ObjMap* doc, Value* out, char* error, size_t errorSize) {
  (void)vm;
  MongoHandle* mh = (MongoHandle*)handle;
  mongoc_collection_t* coll = mongoc_client_get_collection(mh->client, mh->database, collection);
  bson_t bsonDoc;
  bson_init(&bsonDoc);
  for (int i = 0; i < doc->capacity; i++) {
    if (!doc->entries[i].key) continue;
    if (!mongoAppendValue(&bsonDoc, doc->entries[i].key->chars, doc->entries[i].value)) {
      snprintf(error, errorSize, "mongo insert failed to encode document.");
      bson_destroy(&bsonDoc);
      mongoc_collection_destroy(coll);
      return false;
    }
  }
  bson_error_t err;
  bool ok = mongoc_collection_insert_one(coll, &bsonDoc, NULL, NULL, &err);
  bson_destroy(&bsonDoc);
  mongoc_collection_destroy(coll);
  if (!ok) {
    snprintf(error, errorSize, "%s", err.message);
    return false;
  }
  if (out) {
    *out = OBJ_VAL(mongoCloneMap(vm, doc));
  }
  return true;
}

static bool mongoFind(VM* vm, void* handle, const char* collection,
                      ObjMap* query, ObjMap* options, ObjArray** out,
                      char* error, size_t errorSize) {
  MongoHandle* mh = (MongoHandle*)handle;
  mongoc_collection_t* coll = mongoc_client_get_collection(mh->client, mh->database, collection);
  bson_t filter;
  bson_init(&filter);
  if (query) {
    for (int i = 0; i < query->capacity; i++) {
      if (!query->entries[i].key) continue;
      if (!mongoAppendValue(&filter, query->entries[i].key->chars, query->entries[i].value)) {
        snprintf(error, errorSize, "mongo find failed to encode filter.");
        bson_destroy(&filter);
        mongoc_collection_destroy(coll);
        return false;
      }
    }
  }
  bson_t opts;
  bson_init(&opts);
  if (options) {
    Value limitVal;
    Value skipVal;
    if (mapGet(options, copyString(vm, "limit"), &limitVal) && IS_NUMBER(limitVal)) {
      bson_append_int64(&opts, "limit", -1, (int64_t)AS_NUMBER(limitVal));
    }
    if (mapGet(options, copyString(vm, "skip"), &skipVal) && IS_NUMBER(skipVal)) {
      bson_append_int64(&opts, "skip", -1, (int64_t)AS_NUMBER(skipVal));
    }
  }
  mongoc_cursor_t* cursor = mongoc_collection_find_with_opts(coll, &filter, &opts, NULL);
  ObjArray* results = newArray(vm);
  const bson_t* doc;
  while (mongoc_cursor_next(cursor, &doc)) {
    ObjMap* map = mongoMapFromBson(vm, doc);
    arrayWrite(results, OBJ_VAL(map));
  }
  bson_error_t err;
  if (mongoc_cursor_error(cursor, &err)) {
    snprintf(error, errorSize, "%s", err.message);
    bson_destroy(&filter);
    bson_destroy(&opts);
    mongoc_cursor_destroy(cursor);
    mongoc_collection_destroy(coll);
    return false;
  }
  bson_destroy(&filter);
  bson_destroy(&opts);
  mongoc_cursor_destroy(cursor);
  mongoc_collection_destroy(coll);
  *out = results;
  return true;
}

static bool mongoUpdate(VM* vm, void* handle, const char* collection,
                        ObjMap* query, ObjMap* update, ObjMap* options,
                        int* outCount, char* error, size_t errorSize) {
  MongoHandle* mh = (MongoHandle*)handle;
  mongoc_collection_t* coll = mongoc_client_get_collection(mh->client, mh->database, collection);
  bson_t filter;
  bson_init(&filter);
  for (int i = 0; i < query->capacity; i++) {
    if (!query->entries[i].key) continue;
    if (!mongoAppendValue(&filter, query->entries[i].key->chars, query->entries[i].value)) {
      snprintf(error, errorSize, "mongo update failed to encode filter.");
      bson_destroy(&filter);
      mongoc_collection_destroy(coll);
      return false;
    }
  }
  bson_t updateDoc;
  bson_init(&updateDoc);
  bson_t setDoc;
  bson_append_document_begin(&updateDoc, "$set", -1, &setDoc);
  for (int i = 0; i < update->capacity; i++) {
    if (!update->entries[i].key) continue;
    if (!mongoAppendValue(&setDoc, update->entries[i].key->chars, update->entries[i].value)) {
      snprintf(error, errorSize, "mongo update failed to encode document.");
      bson_append_document_end(&updateDoc, &setDoc);
      bson_destroy(&updateDoc);
      bson_destroy(&filter);
      mongoc_collection_destroy(coll);
      return false;
    }
  }
  bson_append_document_end(&updateDoc, &setDoc);

  bool multi = true;
  if (options) {
    Value multiVal;
    if (mapGet(options, copyString(vm, "multi"), &multiVal) && IS_BOOL(multiVal)) {
      multi = AS_BOOL(multiVal);
    }
  }
  bson_error_t err;
  bson_t reply;
  bool ok = multi ? mongoc_collection_update_many(coll, &filter, &updateDoc, NULL, &reply, &err)
                  : mongoc_collection_update_one(coll, &filter, &updateDoc, NULL, &reply, &err);
  bson_destroy(&filter);
  bson_destroy(&updateDoc);
  if (!ok) {
    snprintf(error, errorSize, "%s", err.message);
    bson_destroy(&reply);
    mongoc_collection_destroy(coll);
    return false;
  }
  int modified = mongoReplyInt(&reply, "modifiedCount", "nModified");
  bson_destroy(&reply);
  mongoc_collection_destroy(coll);
  *outCount = modified;
  return true;
}

static bool mongoRemove(VM* vm, void* handle, const char* collection,
                        ObjMap* query, ObjMap* options, int* outCount,
                        char* error, size_t errorSize) {
  MongoHandle* mh = (MongoHandle*)handle;
  mongoc_collection_t* coll = mongoc_client_get_collection(mh->client, mh->database, collection);
  bson_t filter;
  bson_init(&filter);
  for (int i = 0; i < query->capacity; i++) {
    if (!query->entries[i].key) continue;
    if (!mongoAppendValue(&filter, query->entries[i].key->chars, query->entries[i].value)) {
      snprintf(error, errorSize, "mongo delete failed to encode filter.");
      bson_destroy(&filter);
      mongoc_collection_destroy(coll);
      return false;
    }
  }
  bool multi = true;
  if (options) {
    Value multiVal;
    if (mapGet(options, copyString(vm, "multi"), &multiVal) && IS_BOOL(multiVal)) {
      multi = AS_BOOL(multiVal);
    }
  }
  bson_error_t err;
  bson_t reply;
  bool ok = multi ? mongoc_collection_delete_many(coll, &filter, NULL, &reply, &err)
                  : mongoc_collection_delete_one(coll, &filter, NULL, &reply, &err);
  bson_destroy(&filter);
  if (!ok) {
    snprintf(error, errorSize, "%s", err.message);
    bson_destroy(&reply);
    mongoc_collection_destroy(coll);
    return false;
  }
  int deleted = mongoReplyInt(&reply, "deletedCount", "n");
  bson_destroy(&reply);
  mongoc_collection_destroy(coll);
  *outCount = deleted;
  return true;
}

static const DbDriver DB_MONGO_DRIVER = {
  "mongo",
  DB_KIND_DOCUMENT,
  DB_PARAM_QMARK,
  mongoConnect,
  mongoClose,
  NULL,
  mongoInsert,
  mongoFind,
  mongoUpdate,
  mongoRemove
};
#endif

void dbRegisterMongoDriver(VM* vm) {
#if ERKAO_DB_MONGO
  mongoc_init();
  dbRegisterDriver(vm, &DB_MONGO_DRIVER);
#else
  (void)vm;
#endif
}

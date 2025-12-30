#ifndef ERKAO_DB_H
#define ERKAO_DB_H

#include "interpreter.h"

typedef enum {
  DB_KIND_SQL,
  DB_KIND_DOCUMENT
} DbDriverKind;

typedef enum {
  DB_PARAM_QMARK,
  DB_PARAM_DOLLAR
} DbParamStyle;

typedef struct {
  ObjArray* rows;
  int affected;
} DbExecResult;

typedef struct DbDriver {
  const char* name;
  DbDriverKind kind;
  DbParamStyle paramStyle;
  bool (*connect)(VM* vm, const char* uri, ObjMap* options, void** outHandle,
                  char* error, size_t errorSize);
  void (*close)(VM* vm, void* handle);
  bool (*exec)(VM* vm, void* handle, const char* sql, ObjArray* params,
               DbExecResult* out, char* error, size_t errorSize);
  bool (*insert)(VM* vm, void* handle, const char* collection, ObjMap* doc,
                 Value* out, char* error, size_t errorSize);
  bool (*find)(VM* vm, void* handle, const char* collection, ObjMap* query,
               ObjMap* options, ObjArray** out, char* error, size_t errorSize);
  bool (*update)(VM* vm, void* handle, const char* collection, ObjMap* query,
                 ObjMap* update, ObjMap* options, int* outCount,
                 char* error, size_t errorSize);
  bool (*remove)(VM* vm, void* handle, const char* collection, ObjMap* query,
                 ObjMap* options, int* outCount, char* error, size_t errorSize);
} DbDriver;

void dbRegisterDriver(VM* vm, const DbDriver* driver);
void dbShutdown(VM* vm);
void defineDbModule(VM* vm);

#endif

#include "db.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if ERKAO_DB_POSTGRES
#include <libpq-fe.h>
#endif

#if ERKAO_DB_POSTGRES
static bool pgValueIsTrue(const char* text) {
  return strcmp(text, "t") == 0 || strcmp(text, "true") == 0 || strcmp(text, "TRUE") == 0;
}

static bool pgValueIsFalse(const char* text) {
  return strcmp(text, "f") == 0 || strcmp(text, "false") == 0 || strcmp(text, "FALSE") == 0;
}

static Value pgValueFromString(VM* vm, const char* text) {
  if (!text) return NULL_VAL;
  if (pgValueIsTrue(text)) return BOOL_VAL(true);
  if (pgValueIsFalse(text)) return BOOL_VAL(false);
  char* end = NULL;
  double value = strtod(text, &end);
  if (end && *end == '\0') {
    return NUMBER_VAL(value);
  }
  return OBJ_VAL(copyString(vm, text));
}

static bool pgConnect(VM* vm, const char* uri, ObjMap* options,
                      void** outHandle, char* error, size_t errorSize) {
  (void)vm;
  (void)options;
  PGconn* conn = PQconnectdb(uri);
  if (!conn) {
    snprintf(error, errorSize, "postgres connect failed.");
    return false;
  }
  if (PQstatus(conn) != CONNECTION_OK) {
    snprintf(error, errorSize, "%s", PQerrorMessage(conn));
    PQfinish(conn);
    return false;
  }
  *outHandle = conn;
  return true;
}

static void pgClose(VM* vm, void* handle) {
  (void)vm;
  if (handle) PQfinish((PGconn*)handle);
}

static bool pgExec(VM* vm, void* handle, const char* sql, ObjArray* params,
                   DbExecResult* out, char* error, size_t errorSize) {
  PGconn* conn = (PGconn*)handle;
  if (error && errorSize > 0) {
    error[0] = '\0';
  }
  int paramCount = params ? params->count : 0;
  const char** values = NULL;
  char** allocated = NULL;
  if (paramCount > 0) {
    values = (const char**)calloc((size_t)paramCount, sizeof(char*));
    allocated = (char**)calloc((size_t)paramCount, sizeof(char*));
    if (!values || !allocated) {
      snprintf(error, errorSize, "postgres exec out of memory.");
      free(values);
      free(allocated);
      return false;
    }
  }

  for (int i = 0; i < paramCount; i++) {
    Value value = params->items[i];
    if (IS_NULL(value)) {
      values[i] = NULL;
      continue;
    }
    if (IS_NUMBER(value)) {
      char buffer[64];
      int length = snprintf(buffer, sizeof(buffer), "%g", AS_NUMBER(value));
      allocated[i] = (char*)malloc((size_t)length + 1);
      if (!allocated[i]) {
        snprintf(error, errorSize, "postgres exec out of memory.");
        paramCount = i + 1;
        goto cleanup;
      }
      memcpy(allocated[i], buffer, (size_t)length + 1);
      values[i] = allocated[i];
      continue;
    }
    if (IS_BOOL(value)) {
      const char* text = AS_BOOL(value) ? "true" : "false";
      allocated[i] = (char*)malloc(strlen(text) + 1);
      if (!allocated[i]) {
        snprintf(error, errorSize, "postgres exec out of memory.");
        paramCount = i + 1;
        goto cleanup;
      }
      strcpy(allocated[i], text);
      values[i] = allocated[i];
      continue;
    }
    if (isObjType(value, OBJ_STRING)) {
      ObjString* string = (ObjString*)AS_OBJ(value);
      values[i] = string->chars;
      continue;
    }
    snprintf(error, errorSize, "postgres exec unsupported param type.");
    paramCount = i + 1;
    goto cleanup;
  }

  PGresult* result = PQexecParams(conn, sql, paramCount, NULL, values, NULL, NULL, 0);
  if (!result) {
    snprintf(error, errorSize, "%s", PQerrorMessage(conn));
    goto cleanup;
  }
  ExecStatusType status = PQresultStatus(result);
  if (status != PGRES_TUPLES_OK && status != PGRES_COMMAND_OK) {
    snprintf(error, errorSize, "%s", PQresultErrorMessage(result));
    PQclear(result);
    goto cleanup;
  }

  if (out) {
    out->rows = NULL;
    out->affected = -1;
  }

  if (status == PGRES_TUPLES_OK) {
    int rows = PQntuples(result);
    int cols = PQnfields(result);
    ObjArray* array = newArrayWithCapacity(vm, rows);
    for (int r = 0; r < rows; r++) {
      ObjMap* row = newMap(vm);
      for (int c = 0; c < cols; c++) {
        const char* name = PQfname(result, c);
        ObjString* key = copyString(vm, name ? name : "");
        if (PQgetisnull(result, r, c)) {
          mapSet(row, key, NULL_VAL);
        } else {
          const char* text = PQgetvalue(result, r, c);
          mapSet(row, key, pgValueFromString(vm, text));
        }
      }
      arrayWrite(array, OBJ_VAL(row));
    }
    if (out) {
      out->rows = array;
      out->affected = rows;
    }
  } else if (status == PGRES_COMMAND_OK && out) {
    const char* count = PQcmdTuples(result);
    if (count && count[0] != '\0') {
      out->affected = atoi(count);
    } else {
      out->affected = 0;
    }
  }

  PQclear(result);

cleanup:
  if (allocated) {
    for (int i = 0; i < paramCount; i++) {
      free(allocated[i]);
    }
  }
  free(values);
  free(allocated);
  if (error[0] != '\0') return false;
  return true;
}

static const DbDriver DB_POSTGRES_DRIVER = {
  "postgres",
  DB_KIND_SQL,
  DB_PARAM_DOLLAR,
  pgConnect,
  pgClose,
  pgExec,
  NULL,
  NULL,
  NULL,
  NULL
};
#endif

void dbRegisterPostgresDriver(VM* vm) {
#if ERKAO_DB_POSTGRES
  dbRegisterDriver(vm, &DB_POSTGRES_DRIVER);
#else
  (void)vm;
#endif
}

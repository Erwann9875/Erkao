#include "db.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if ERKAO_DB_MYSQL
#include <mysql.h>
#endif

#if ERKAO_DB_MYSQL
typedef struct {
  char* host;
  char* user;
  char* pass;
  char* db;
  unsigned int port;
} MysqlConfig;

static char* dbStrndup(const char* start, size_t length) {
  char* out = (char*)malloc(length + 1);
  if (!out) return NULL;
  memcpy(out, start, length);
  out[length] = '\0';
  return out;
}

static void mysqlConfigFree(MysqlConfig* cfg) {
  if (!cfg) return;
  free(cfg->host);
  free(cfg->user);
  free(cfg->pass);
  free(cfg->db);
  cfg->host = NULL;
  cfg->user = NULL;
  cfg->pass = NULL;
  cfg->db = NULL;
}

static bool mysqlParseUri(const char* uri, MysqlConfig* cfg,
                          char* error, size_t errorSize) {
  const char* scheme = strstr(uri, "://");
  if (!scheme) {
    snprintf(error, errorSize, "mysql uri must include ://");
    return false;
  }
  const char* start = scheme + 3;
  const char* slash = strchr(start, '/');
  const char* authorityEnd = slash ? slash : uri + strlen(uri);
  const char* at = memchr(start, '@', (size_t)(authorityEnd - start));
  const char* hostStart = start;
  if (at) {
    const char* userStart = start;
    const char* userEnd = at;
    const char* colon = memchr(userStart, ':', (size_t)(userEnd - userStart));
    if (colon) {
      cfg->user = dbStrndup(userStart, (size_t)(colon - userStart));
      cfg->pass = dbStrndup(colon + 1, (size_t)(userEnd - colon - 1));
    } else {
      cfg->user = dbStrndup(userStart, (size_t)(userEnd - userStart));
    }
    hostStart = at + 1;
  }
  const char* hostEnd = authorityEnd;
  const char* colon = NULL;
  for (const char* p = hostStart; p < authorityEnd; p++) {
    if (*p == ':') colon = p;
  }
  if (colon) {
    hostEnd = colon;
    cfg->port = (unsigned int)atoi(colon + 1);
  }
  cfg->host = dbStrndup(hostStart, (size_t)(hostEnd - hostStart));
  if (!cfg->host || cfg->host[0] == '\0') {
    snprintf(error, errorSize, "mysql uri missing host");
    return false;
  }
  if (slash && slash[1] != '\0') {
    const char* dbStart = slash + 1;
    const char* dbEnd = strchr(dbStart, '?');
    if (!dbEnd) dbEnd = uri + strlen(uri);
    if (dbEnd > dbStart) {
      cfg->db = dbStrndup(dbStart, (size_t)(dbEnd - dbStart));
    }
  }
  return true;
}

static Value mysqlValueFromString(VM* vm, const char* text) {
  if (!text) return NULL_VAL;
  if (strcmp(text, "0") == 0) return NUMBER_VAL(0);
  if (strcmp(text, "1") == 0) return NUMBER_VAL(1);
  if (strcmp(text, "true") == 0 || strcmp(text, "TRUE") == 0) return BOOL_VAL(true);
  if (strcmp(text, "false") == 0 || strcmp(text, "FALSE") == 0) return BOOL_VAL(false);
  char* end = NULL;
  double value = strtod(text, &end);
  if (end && *end == '\0') {
    return NUMBER_VAL(value);
  }
  return OBJ_VAL(copyString(vm, text));
}

static bool mysqlConnect(VM* vm, const char* uri, ObjMap* options,
                         void** outHandle, char* error, size_t errorSize) {
  (void)vm;
  (void)options;
  MysqlConfig cfg;
  memset(&cfg, 0, sizeof(cfg));
  if (!mysqlParseUri(uri, &cfg, error, errorSize)) {
    mysqlConfigFree(&cfg);
    return false;
  }
  MYSQL* conn = mysql_init(NULL);
  if (!conn) {
    mysqlConfigFree(&cfg);
    snprintf(error, errorSize, "mysql init failed.");
    return false;
  }
  if (!mysql_real_connect(conn, cfg.host, cfg.user, cfg.pass, cfg.db,
                          cfg.port, NULL, 0)) {
    snprintf(error, errorSize, "%s", mysql_error(conn));
    mysql_close(conn);
    mysqlConfigFree(&cfg);
    return false;
  }
  mysqlConfigFree(&cfg);
  *outHandle = conn;
  return true;
}

static void mysqlClose(VM* vm, void* handle) {
  (void)vm;
  if (handle) mysql_close((MYSQL*)handle);
}

typedef struct {
  char* data;
  size_t length;
  size_t capacity;
} MysqlString;

static void mysqlStringInit(MysqlString* sb) {
  sb->data = NULL;
  sb->length = 0;
  sb->capacity = 0;
}

static void mysqlStringEnsure(MysqlString* sb, size_t needed) {
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

static void mysqlStringAppend(MysqlString* sb, const char* text) {
  size_t length = strlen(text);
  mysqlStringEnsure(sb, sb->length + length + 1);
  memcpy(sb->data + sb->length, text, length);
  sb->length += length;
  sb->data[sb->length] = '\0';
}

static void mysqlStringAppendN(MysqlString* sb, const char* text, size_t length) {
  mysqlStringEnsure(sb, sb->length + length + 1);
  memcpy(sb->data + sb->length, text, length);
  sb->length += length;
  sb->data[sb->length] = '\0';
}

static char* mysqlValueLiteral(MYSQL* conn, Value value, char* error, size_t errorSize) {
  if (IS_NULL(value)) {
    return dbStrndup("NULL", 4);
  }
  if (IS_NUMBER(value)) {
    char buffer[64];
    int length = snprintf(buffer, sizeof(buffer), "%g", AS_NUMBER(value));
    if (length < 0) length = 0;
    return dbStrndup(buffer, (size_t)length);
  }
  if (IS_BOOL(value)) {
    return dbStrndup(AS_BOOL(value) ? "1" : "0", 1);
  }
  if (isObjType(value, OBJ_STRING)) {
    ObjString* str = (ObjString*)AS_OBJ(value);
    size_t needed = (size_t)str->length * 2 + 3;
    char* buffer = (char*)malloc(needed);
    if (!buffer) {
      snprintf(error, errorSize, "mysql exec out of memory.");
      return NULL;
    }
    buffer[0] = '\'';
    unsigned long written = mysql_real_escape_string(conn, buffer + 1, str->chars,
                                                     (unsigned long)str->length);
    buffer[1 + written] = '\'';
    buffer[2 + written] = '\0';
    return buffer;
  }
  snprintf(error, errorSize, "mysql exec unsupported param type.");
  return NULL;
}

static bool mysqlBuildQuery(MYSQL* conn, const char* sql, ObjArray* params,
                            char** outQuery, char* error, size_t errorSize) {
  MysqlString sb;
  mysqlStringInit(&sb);
  int paramIndex = 0;
  int paramCount = params ? params->count : 0;
  for (const char* c = sql; *c; c++) {
    if (*c == '?') {
      if (paramIndex >= paramCount) {
        free(sb.data);
        snprintf(error, errorSize, "mysql exec param count mismatch.");
        return false;
      }
      char* literal = mysqlValueLiteral(conn, params->items[paramIndex], error, errorSize);
      if (!literal) {
        free(sb.data);
        return false;
      }
      mysqlStringAppend(&sb, literal);
      free(literal);
      paramIndex++;
    } else {
      mysqlStringAppendN(&sb, c, 1);
    }
  }
  if (paramIndex != paramCount) {
    free(sb.data);
    snprintf(error, errorSize, "mysql exec param count mismatch.");
    return false;
  }
  *outQuery = sb.data;
  return true;
}

static bool mysqlExec(VM* vm, void* handle, const char* sql, ObjArray* params,
                      DbExecResult* out, char* error, size_t errorSize) {
  MYSQL* conn = (MYSQL*)handle;
  char* query = NULL;
  if (!mysqlBuildQuery(conn, sql, params, &query, error, errorSize)) {
    return false;
  }
  if (mysql_query(conn, query) != 0) {
    snprintf(error, errorSize, "%s", mysql_error(conn));
    free(query);
    return false;
  }
  free(query);

  MYSQL_RES* result = mysql_store_result(conn);
  if (out) {
    out->rows = NULL;
    out->affected = -1;
  }

  if (result) {
    int rows = (int)mysql_num_rows(result);
    int cols = (int)mysql_num_fields(result);
    MYSQL_FIELD* fields = mysql_fetch_fields(result);
    ObjArray* array = newArrayWithCapacity(vm, rows);
    MYSQL_ROW row;
    while ((row = mysql_fetch_row(result)) != NULL) {
      unsigned long* lengths = mysql_fetch_lengths(result);
      ObjMap* map = newMap(vm);
      for (int i = 0; i < cols; i++) {
        ObjString* key = copyString(vm, fields[i].name ? fields[i].name : "");
        if (!row[i]) {
          mapSet(map, key, NULL_VAL);
        } else {
          mapSet(map, key, mysqlValueFromString(vm, row[i]));
        }
        (void)lengths;
      }
      arrayWrite(array, OBJ_VAL(map));
    }
    if (out) {
      out->rows = array;
      out->affected = rows;
    }
    mysql_free_result(result);
    return true;
  }

  if (mysql_field_count(conn) != 0) {
    snprintf(error, errorSize, "%s", mysql_error(conn));
    return false;
  }

  if (out) {
    out->affected = (int)mysql_affected_rows(conn);
  }
  return true;
}

static const DbDriver DB_MYSQL_DRIVER = {
  "mysql",
  DB_KIND_SQL,
  DB_PARAM_QMARK,
  mysqlConnect,
  mysqlClose,
  mysqlExec,
  NULL,
  NULL,
  NULL,
  NULL
};
#endif

void dbRegisterMysqlDriver(VM* vm) {
#if ERKAO_DB_MYSQL
  dbRegisterDriver(vm, &DB_MYSQL_DRIVER);
#else
  (void)vm;
#endif
}

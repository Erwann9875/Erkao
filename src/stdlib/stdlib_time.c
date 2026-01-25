#include "stdlib_internal.h"

#include <time.h>

static Value nativeTimeNow(VM* vm, int argc, Value* args) {
  (void)argc;
  (void)args;
  time_t now = time(NULL);
  if (now == (time_t)-1) {
    return runtimeErrorValue(vm, "time.now failed.");
  }
  return NUMBER_VAL((double)now);
}

static Value nativeTimeSleep(VM* vm, int argc, Value* args) {
  (void)argc;
  if (!IS_NUMBER(args[0])) {
    return runtimeErrorValue(vm, "time.sleep expects seconds as a number.");
  }
  double seconds = AS_NUMBER(args[0]);
  if (seconds < 0) {
    return runtimeErrorValue(vm, "time.sleep expects a non-negative number.");
  }
#ifdef _WIN32
  DWORD ms = (DWORD)(seconds * 1000.0);
  Sleep(ms);
#else
  struct timespec ts;
  ts.tv_sec = (time_t)seconds;
  ts.tv_nsec = (long)((seconds - (double)ts.tv_sec) * 1000000000.0);
  if (ts.tv_nsec < 0) ts.tv_nsec = 0;
  if (nanosleep(&ts, NULL) != 0) {
    return runtimeErrorValue(vm, "time.sleep failed.");
  }
#endif
  return NULL_VAL;
}

static bool timeGetTm(double seconds, bool utc, struct tm* out) {
  time_t raw = (time_t)seconds;
#ifdef _WIN32
  int err = utc ? gmtime_s(out, &raw) : localtime_s(out, &raw);
  return err == 0;
#else
  struct tm* result = utc ? gmtime_r(&raw, out) : localtime_r(&raw, out);
  return result != NULL;
#endif
}

static bool valueIsTruthy(Value value) {
  if (IS_NULL(value)) return false;
  if (IS_BOOL(value)) return AS_BOOL(value);
  if (IS_NUMBER(value)) return AS_NUMBER(value) != 0;
  return true;
}

static Value nativeTimeFormat(VM* vm, int argc, Value* args) {
  if (argc < 2) {
    return runtimeErrorValue(vm, "time.format expects (timestamp, format, utc?).");
  }
  if (!IS_NUMBER(args[0]) || !isObjType(args[1], OBJ_STRING)) {
    return runtimeErrorValue(vm, "time.format expects (timestamp, format, utc?).");
  }
  bool utc = false;
  if (argc >= 3) {
    utc = valueIsTruthy(args[2]);
  }
  ObjString* format = (ObjString*)AS_OBJ(args[1]);
  struct tm tmValue;
  if (!timeGetTm(AS_NUMBER(args[0]), utc, &tmValue)) {
    return runtimeErrorValue(vm, "time.format failed.");
  }
  char buffer[256];
  size_t written = strftime(buffer, sizeof(buffer), format->chars, &tmValue);
  if (written == 0) {
    return runtimeErrorValue(vm, "time.format failed to format.");
  }
  return OBJ_VAL(copyStringWithLength(vm, buffer, (int)written));
}

static Value nativeTimeIso(VM* vm, int argc, Value* args) {
  if (argc < 1 || !IS_NUMBER(args[0])) {
    return runtimeErrorValue(vm, "time.iso expects (timestamp, utc?).");
  }
  bool utc = false;
  if (argc >= 2) {
    utc = valueIsTruthy(args[1]);
  }
  struct tm tmValue;
  if (!timeGetTm(AS_NUMBER(args[0]), utc, &tmValue)) {
    return runtimeErrorValue(vm, "time.iso failed.");
  }
  char buffer[32];
  size_t written = strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%S", &tmValue);
  if (written == 0) {
    return runtimeErrorValue(vm, "time.iso failed to format.");
  }
  if (utc && written + 1 < sizeof(buffer)) {
    buffer[written++] = 'Z';
    buffer[written] = '\0';
  }
  return OBJ_VAL(copyStringWithLength(vm, buffer, (int)written));
}

static Value nativeTimeParts(VM* vm, int argc, Value* args) {
  if (argc < 1 || !IS_NUMBER(args[0])) {
    return runtimeErrorValue(vm, "time.parts expects (timestamp, utc?).");
  }
  bool utc = false;
  if (argc >= 2) {
    utc = valueIsTruthy(args[1]);
  }
  struct tm tmValue;
  if (!timeGetTm(AS_NUMBER(args[0]), utc, &tmValue)) {
    return runtimeErrorValue(vm, "time.parts failed.");
  }
  ObjMap* map = newMap(vm);
  mapSet(map, copyString(vm, "year"), NUMBER_VAL((double)(tmValue.tm_year + 1900)));
  mapSet(map, copyString(vm, "month"), NUMBER_VAL((double)(tmValue.tm_mon + 1)));
  mapSet(map, copyString(vm, "day"), NUMBER_VAL((double)tmValue.tm_mday));
  mapSet(map, copyString(vm, "hour"), NUMBER_VAL((double)tmValue.tm_hour));
  mapSet(map, copyString(vm, "min"), NUMBER_VAL((double)tmValue.tm_min));
  mapSet(map, copyString(vm, "sec"), NUMBER_VAL((double)tmValue.tm_sec));
  mapSet(map, copyString(vm, "wday"), NUMBER_VAL((double)tmValue.tm_wday));
  mapSet(map, copyString(vm, "yday"), NUMBER_VAL((double)tmValue.tm_yday));
  mapSet(map, copyString(vm, "isdst"), BOOL_VAL(tmValue.tm_isdst > 0));
  return OBJ_VAL(map);
}


void stdlib_register_time(VM* vm, ObjInstance* module) {
  moduleAdd(vm, module, "now", nativeTimeNow, 0);
  moduleAdd(vm, module, "sleep", nativeTimeSleep, 1);
  moduleAdd(vm, module, "format", nativeTimeFormat, -1);
  moduleAdd(vm, module, "iso", nativeTimeIso, -1);
  moduleAdd(vm, module, "parts", nativeTimeParts, -1);
}

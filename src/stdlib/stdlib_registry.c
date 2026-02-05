#include "erkao_stdlib.h"
#include "stdlib_internal.h"
#include "db.h"

#if ERKAO_HAS_GRAPHICS
#include "graphics.h"
#endif

void stdlib_register_globals(VM* vm);
void stdlib_register_fs(VM* vm, ObjInstance* module);
void stdlib_register_path(VM* vm, ObjInstance* module);
void stdlib_register_json(VM* vm, ObjInstance* module);
void stdlib_register_yaml(VM* vm, ObjInstance* module);
void stdlib_register_math(VM* vm, ObjInstance* module);
void stdlib_register_random(VM* vm, ObjInstance* module);
void stdlib_register_str(VM* vm, ObjInstance* module);
void stdlib_register_array(VM* vm, ObjInstance* module);
void stdlib_register_os(VM* vm, ObjInstance* module);
void stdlib_register_time(VM* vm, ObjInstance* module);
void stdlib_register_vec(VM* vm, ObjInstance* vec2, ObjInstance* vec3, ObjInstance* vec4);
void stdlib_register_http(VM* vm, ObjInstance* module);
void stdlib_register_proc(VM* vm, ObjInstance* module);
void stdlib_register_env(VM* vm, ObjInstance* module);
void stdlib_register_di(VM* vm, ObjInstance* module);
void stdlib_register_ffi(VM* vm, ObjInstance* module);
void stdlib_register_plugin(VM* vm, ObjInstance* module);

void defineStdlib(VM* vm) {
  stdlib_register_globals(vm);

  ObjInstance* fs = makeModule(vm, "fs");
  stdlib_register_fs(vm, fs);
  defineGlobal(vm, "fs", OBJ_VAL(fs));

  ObjInstance* path = makeModule(vm, "path");
  stdlib_register_path(vm, path);
  defineGlobal(vm, "path", OBJ_VAL(path));

  ObjInstance* json = makeModule(vm, "json");
  stdlib_register_json(vm, json);
  defineGlobal(vm, "json", OBJ_VAL(json));

  ObjInstance* yaml = makeModule(vm, "yaml");
  stdlib_register_yaml(vm, yaml);
  defineGlobal(vm, "yaml", OBJ_VAL(yaml));

  ObjInstance* math = makeModule(vm, "math");
  stdlib_register_math(vm, math);
  defineGlobal(vm, "math", OBJ_VAL(math));

  ObjInstance* random = makeModule(vm, "random");
  stdlib_register_random(vm, random);
  defineGlobal(vm, "random", OBJ_VAL(random));

  ObjInstance* str = makeModule(vm, "str");
  stdlib_register_str(vm, str);
  defineGlobal(vm, "str", OBJ_VAL(str));

  ObjInstance* array = makeModule(vm, "array");
  stdlib_register_array(vm, array);
  defineGlobal(vm, "array", OBJ_VAL(array));

  ObjInstance* os = makeModule(vm, "os");
  stdlib_register_os(vm, os);
  defineGlobal(vm, "os", OBJ_VAL(os));

  ObjInstance* timeModule = makeModule(vm, "time");
  stdlib_register_time(vm, timeModule);
  defineGlobal(vm, "time", OBJ_VAL(timeModule));

  ObjInstance* vec2 = makeModule(vm, "vec2");
  ObjInstance* vec3 = makeModule(vm, "vec3");
  ObjInstance* vec4 = makeModule(vm, "vec4");
  stdlib_register_vec(vm, vec2, vec3, vec4);
  defineGlobal(vm, "vec2", OBJ_VAL(vec2));
  defineGlobal(vm, "vec3", OBJ_VAL(vec3));
  defineGlobal(vm, "vec4", OBJ_VAL(vec4));

  ObjInstance* http = makeModule(vm, "http");
  stdlib_register_http(vm, http);
  defineGlobal(vm, "http", OBJ_VAL(http));

  ObjInstance* proc = makeModule(vm, "proc");
  stdlib_register_proc(vm, proc);
  defineGlobal(vm, "proc", OBJ_VAL(proc));

  ObjInstance* env = makeModule(vm, "env");
  stdlib_register_env(vm, env);
  defineGlobal(vm, "env", OBJ_VAL(env));

  ObjInstance* di = makeModule(vm, "di");
  stdlib_register_di(vm, di);
  defineGlobal(vm, "di", OBJ_VAL(di));

  defineDbModule(vm);

  ObjInstance* ffi = makeModule(vm, "ffi");
  stdlib_register_ffi(vm, ffi);
  defineGlobal(vm, "ffi", OBJ_VAL(ffi));

  ObjInstance* plugin = makeModule(vm, "plugin");
  stdlib_register_plugin(vm, plugin);
  defineGlobal(vm, "plugin", OBJ_VAL(plugin));

#if ERKAO_HAS_GRAPHICS
  defineGraphicsModule(vm, makeModule, moduleAdd, defineGlobal);
#endif
}

#include "erkao_plugin.h"

#include <stdio.h>

static Value helloSay(VM* vm, int argc, Value* args) {
  (void)vm;
  (void)argc;
  (void)args;
  printf("hello from plugin\n");
  return NULL_VAL;
}

static Value helloAdd(VM* vm, int argc, Value* args) {
  (void)vm;
  (void)argc;
  if (!IS_NUMBER(args[0]) || !IS_NUMBER(args[1])) return NULL_VAL;
  return NUMBER_VAL(AS_NUMBER(args[0]) + AS_NUMBER(args[1]));
}

ERKAO_PLUGIN_EXPORT bool erkao_init(ErkaoApi* api) {
  if (!api || api->apiVersion < ERKAO_PLUGIN_API_VERSION) return false;

  if ((api->features & ERKAO_PLUGIN_FEATURE_MODULES) != 0 &&
      api->createModule && api->defineModule && api->moduleAddNative) {
    ErkaoModule* module = api->createModule(api->vm, "hello");
    api->moduleAddNative(api->vm, module, "say", helloSay, 0);
    api->moduleAddNative(api->vm, module, "add", helloAdd, 2);
    api->defineModule(api->vm, "hello", module);
  } else {
    api->defineNative(api->vm, "helloSay", helloSay, 0);
    api->defineNative(api->vm, "helloAdd", helloAdd, 2);
  }

  return true;
}

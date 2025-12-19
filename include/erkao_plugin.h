#ifndef ERKAO_PLUGIN_H
#define ERKAO_PLUGIN_H

#include "interpreter.h"

#define ERKAO_PLUGIN_API_VERSION 1
#define ERKAO_PLUGIN_INIT "erkao_init"

typedef struct {
  int apiVersion;
  VM* vm;
  void (*defineNative)(VM* vm, const char* name, NativeFn function, int arity);
} ErkaoApi;

typedef bool (*ErkaoPluginInit)(ErkaoApi* api);

#endif

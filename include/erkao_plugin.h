#ifndef ERKAO_PLUGIN_H
#define ERKAO_PLUGIN_H

#include <stddef.h>
#include <stdint.h>

#include "interpreter.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ERKAO_PLUGIN_API_VERSION 1
#define ERKAO_PLUGIN_ABI_VERSION 1
#define ERKAO_PLUGIN_INIT "erkao_init"

#if defined(_WIN32)
#define ERKAO_PLUGIN_EXPORT __declspec(dllexport)
#elif defined(__GNUC__)
#define ERKAO_PLUGIN_EXPORT __attribute__((visibility("default")))
#else
#define ERKAO_PLUGIN_EXPORT
#endif

#define ERKAO_PLUGIN_FEATURE_MODULES (1u << 0)

typedef struct ErkaoModule ErkaoModule;

typedef struct {
  int apiVersion;
  VM* vm;
  void (*defineNative)(VM* vm, const char* name, NativeFn function, int arity);
  size_t size;
  uint32_t abiVersion;
  uint32_t features;
  ErkaoModule* (*createModule)(VM* vm, const char* name);
  void (*defineModule)(VM* vm, const char* name, ErkaoModule* module);
  void (*moduleAddNative)(VM* vm, ErkaoModule* module, const char* name,
                          NativeFn function, int arity);
  void (*moduleAddValue)(VM* vm, ErkaoModule* module, const char* name, Value value);
} ErkaoApi;

typedef bool (*ErkaoPluginInit)(ErkaoApi* api);

#ifdef __cplusplus
}
#endif

#endif

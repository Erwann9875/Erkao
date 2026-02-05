#ifndef ERKAO_PLUGIN_LOADER_H
#define ERKAO_PLUGIN_LOADER_H

#include "interpreter.h"

bool pluginLoad(VM* vm, const char* path, char* error, size_t errorSize);
void pluginUnloadAll(VM* vm);

#endif

#ifndef ERKAO_GRAPHICS_H
#define ERKAO_GRAPHICS_H

#include "value.h"

#if ERKAO_HAS_GRAPHICS

void defineGraphicsModule(VM* vm, 
                          ObjInstance* (*makeModuleFn)(VM*, const char*),
                          void (*moduleAddFn)(VM*, ObjInstance*, const char*, NativeFn, int),
                          void (*defineGlobalFn)(VM*, const char*, Value));

void graphicsCleanup(void);

#endif // ERKAO_HAS_GRAPHICS

#endif // ERKAO_GRAPHICS_H

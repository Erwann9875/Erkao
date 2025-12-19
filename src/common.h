#ifndef ERKAO_COMMON_H
#define ERKAO_COMMON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <ctype.h>

#define ERK_MAX_ARGS 255

#define GROW_CAPACITY(capacity) ((capacity) < 8 ? 8 : (capacity) * 2)

#define GROW_ARRAY(type, pointer, oldCount, newCount) \
  (type*)realloc((pointer), sizeof(type) * (newCount))

#define FREE_ARRAY(type, pointer, oldCount) \
  do { (void)(oldCount); free(pointer); } while (0)

#endif

#ifndef ERKAO_TOOLING_H
#define ERKAO_TOOLING_H

#include <stdbool.h>

bool formatFile(const char* path, bool checkOnly, bool* changed);
int lintFile(const char* path);

#endif

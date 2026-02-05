#ifndef ERKAO_PLATFORM_H
#define ERKAO_PLATFORM_H

#include <stdbool.h>
#include <stddef.h>

char* platform_strdup(const char* src);
char* platform_strndup(const char* src, size_t length);

char* platform_read_file(const char* path, size_t* out_size);

bool platform_path_exists(const char* path);
bool platform_path_is_dir(const char* path);
bool platform_path_is_file(const char* path);
bool platform_path_is_absolute(const char* path);

char platform_path_pick_separator(const char* left, const char* right);
char* platform_path_dirname(const char* path);
char* platform_path_basename(const char* path);
char* platform_path_join(const char* left, const char* right);

bool platform_make_dir(const char* path);
bool platform_ensure_dir(const char* path);
char* platform_get_cwd(void);

#endif

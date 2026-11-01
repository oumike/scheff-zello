#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

FILE *storage_fopen(const char *path, const char *mode);
int storage_rename(const char *from, const char *to);
int storage_remove(const char *path);
int storage_stat(const char *path, struct stat *info);
#ifndef __APPLE__
size_t strlcpy(char *dest, const char *src, size_t size);
#endif

#define fopen storage_fopen
#define rename storage_rename
#define remove storage_remove
#define stat(path, info) storage_stat(path, info)

extern unsigned storage_rename_failures;
extern unsigned storage_rename_calls;
extern bool storage_fail_backup_remove;
extern bool storage_fail_nvs_save;
extern int storage_nvs_init_error;
bool storage_corrupt_snapshot(void);

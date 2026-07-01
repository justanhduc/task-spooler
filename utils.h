/*
    Task Spooler PLUS - a multi-user job scheduler like slurm.
    Copyright (C) 2007-2026  Kylin JIANG - Lluís Batlle i Rossell

    Please find the license in the provided COPYING file.
*/
#ifndef UTILS_H
#define UTILS_H

#include <stdint.h>

char *charArray_string(int num, char **array);
char **split_str(const char *str0, int *size);
int64_t str2int64(const char *str);
char *ints_to_chars(int n, int *array, const char *delim);
int *chars_to_ints(int *size, char *str, const char *delim);
char *insert_chars_check(int pos, const char *input, const char *c);
int64_t i64abs(int64_t x);

// void debug_write(const char *str);

#endif /* UTILS_H */

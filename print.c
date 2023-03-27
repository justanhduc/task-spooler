/*
    Task Spooler - a task queue system for the unix user
    Copyright (C) 2007-2009  Lluís Batlle i Rossell

    Please find the license in the provided COPYING file.
*/
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>
#include <stdarg.h>
#include <sys/time.h>
#include "main.h"


/* maxsize: max buffer size, with '\0' */
int fd_nprintf(int fd, int maxsize, const char *fmt, ...) {
    va_list ap;
    char *out;
    int size;
    int rest;

    va_start(ap, fmt);

    out = (char *) malloc(maxsize * sizeof(char));
    if (out == 0) {
        warning("Not enough memory in fd_nprintf()");
        return -1;
    }

    size = vsnprintf(out, maxsize, fmt, ap);

    rest = size; /* We don't want the last null character */
    while (rest > 0) {
        int res;
        res = write(fd, out, rest);
        if (res == -1) {
            warning("Cannot write more chars in pinfo_dump");
            break;
        }
        rest -= res;
    }

    free(out);

    return size;
}


char *ints_to_chars(int n, int *array, const char *delim) {
    int size = n * 12 + n * strlen(delim) + 1;
    char *tmp = (char*) malloc(size * sizeof(char));
    tmp[0] = '\0';
    int j = 0;
    for (int i = 0; i < n; i++) {
        j += sprintf(tmp + j, "%d", array[i]);
        if (i < n - 1) {
            strcat(tmp, delim);
            j += strlen(delim);
        }
    }
    return tmp;
}

int* chars_to_ints(int *size, char* str, const char* delim) {
    int count = 0;
    for (int i = 0; str[i]; i++) {
        if (str[i] == delim[0]) {
            count++;
        }
    }
    count++;
    int *result = malloc(count * sizeof(int));
    result[0] = '\0';
    char *token = strtok(str, delim);
    int index = 0;
    while (token != NULL) {
        result[index++] = atoi(token);
        token = strtok(NULL, delim);
    }
    *size = count;
    return result;
}

char* insert_chars(int pos, const char* input, const char* c) {
    int len = strlen(input) + strlen(c) + 1; 
    char *str = (char *)calloc(len,  sizeof(char)); //用malloc函数分配内存
    if (str == NULL) //判断是否分配成功
    {
      error("Memory allocation failed.\n");
      return NULL;
    }
    strncpy(str, input, pos); //将s数组的前t个字符复制到str中
    strcat(str, c); //将数字字符串连接到str后面
    strcat(str, input + pos); //将s剩余的字符串连接到str后面
    return str;
}
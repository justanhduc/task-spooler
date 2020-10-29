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
int fd_nprintf(int fd, int maxsize, const char *fmt, ...)
{
    va_list ap;
    char *out;
    int size;
    int rest;

    va_start(ap, fmt);

    out = (char *) malloc(maxsize * sizeof(char));
    if (out == 0)
    {
        warning("Not enough memory in fd_nprintf()");
        return -1;
    }

    size = vsnprintf(out, maxsize, fmt, ap);

    rest = size; /* We don't want the last null character */
    while (rest > 0)
    {
        int res;
        res = write(fd, out, rest);
        if (res == -1)
        {
            warning("Cannot write more chars in pinfo_dump");
            break;
        }
        rest -= res;
    }

    free(out);

    return size;
}

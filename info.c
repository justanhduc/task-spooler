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

void pinfo_init(struct Procinfo *p)
{
    p->ptr = 0;
    p->nchars = 0;
    p->allocchars = 0;
    p->start_time.tv_sec = 0;
    p->start_time.tv_usec = 0;
    p->end_time.tv_sec = 0;
    p->end_time.tv_usec = 0;
    p->enqueue_time.tv_sec = 0;
    p->enqueue_time.tv_usec = 0;
}

void pinfo_free(struct Procinfo *p)
{
    if (p->ptr)
    {
        free(p->ptr);
    }
    p->nchars = 0;
    p->allocchars = 0;
}

void pinfo_addinfo(struct Procinfo *p, int maxsize, const char *line, ...)
{
    va_list ap;

    int newchars = p->nchars + maxsize;
    void *newptr;
    int res;

    va_start(ap, line);

    /* Ask for more memory for the array, if needed */
    if (newchars > p->allocchars)
    {
        int newmem;
        int newalloc;
        newalloc = newchars;
        newmem = newchars * sizeof(*p->ptr);
        newptr = realloc(p->ptr, newmem);
        if(newptr == 0)
        {
            warning("Cannot realloc more memory (%i) in pinfo_addline. "
                    "Not adding the content.", newmem);
            return;
        }
        p->ptr = (char *) newptr;
        p->allocchars = newalloc;
    }

    res = vsnprintf(p->ptr + p->nchars, (p->allocchars - p->nchars), line, ap);
    p->nchars += res; /* We don't store the final 0 */
}

void pinfo_dump(const struct Procinfo *p, int fd)
{
    if (p->ptr)
    {
        int res;
        int rest = p->nchars;
        while (rest > 0)
        {
            res = write(fd, p->ptr, rest);
            if (res == -1)
            {
                warning("Cannot write more chars in pinfo_dump");
                return;
            }
            rest -= res;
        }
    }
}

int pinfo_size(const struct Procinfo *p)
{
    return p->nchars;
}

void pinfo_set_enqueue_time(struct Procinfo *p)
{
    gettimeofday(&p->enqueue_time, 0);
    p->start_time.tv_sec = 0;
    p->start_time.tv_usec = 0;
    p->end_time.tv_sec = 0;
    p->end_time.tv_usec = 0;
}

void pinfo_set_start_time(struct Procinfo *p)
{
    gettimeofday(&p->start_time, 0);
    p->end_time.tv_sec = 0;
    p->end_time.tv_usec = 0;
}

void pinfo_set_end_time(struct Procinfo *p)
{
    gettimeofday(&p->end_time, 0);
}

float pinfo_time_until_now(const struct Procinfo *p)
{
    float t;
    struct timeval now;

    gettimeofday(&now, 0);

    t = now.tv_sec - p->start_time.tv_sec;
    t += (float) (now.tv_usec - p->start_time.tv_usec) / 1000000.;

    return t;
}

float pinfo_time_run(const struct Procinfo *p)
{
    float t;

    t = p->end_time.tv_sec - p->start_time.tv_sec;
    t += (float) (p->end_time.tv_usec - p->start_time.tv_usec) / 1000000.;

    return t;
}

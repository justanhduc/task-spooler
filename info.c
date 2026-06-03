/*
    Task Spooler PLUS - a multi-user job scheduler like slurm.
    Copyright (C) 2007-2026  Kylin JIANG - Lluís Batlle i Rossell

    Please find the license in the provided COPYING file.
*/
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>
#include <stdarg.h>
#include <sys/time.h>
#include <time.h>
#include "main.h"
#include "info.h"
#include "error.h"
#include "runtime_limit.h"

void pinfo_init(struct Procinfo *p)
{
    p->ptr = 0;
    p->nchars = 0;
    p->allocchars = 0;
    p->start_time = 0;
    p->end_time = 0;
    p->enqueue_time = 0;
    p->pause_time = 0;
    p->pause_duration = 0;
    p->boot_time = 0;
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
    int actual_write;
    // int available;

    /* 确保有足够空间（maxsize + 1 为了 null 终止符） */
    int needed = p->nchars + maxsize + 1;
    if (needed > p->allocchars)
    {
        int newalloc = needed;
        void *newptr = realloc(p->ptr, newalloc * sizeof(*p->ptr));
        if (newptr == NULL)
        {
            warning("Cannot realloc more memory (%i) in pinfo_addinfo.", newalloc);
            return;
        }
        p->ptr = (char *)newptr;
        p->allocchars = newalloc;
    }

    va_start(ap, line);
    // available = p->allocchars - p->nchars;
    
    /* 限制写入长度为 maxsize */
    actual_write = vsnprintf(p->ptr + p->nchars, maxsize + 1, line, ap);
    va_end(ap);

    if (actual_write < 0)
    {
        warning("vsnprintf encoding error in pinfo_addinfo");
        return;
    }

    /* 只更新实际写入的字节数（最多 maxsize） */
    if (actual_write > maxsize)
        actual_write = maxsize;
    
    p->nchars += actual_write;
}

void pinfo_dump(const struct Procinfo *p, int fd)
{
    if (p->ptr)
    {
        int res;
        int rest = p->nchars;
        const char *buf = p->ptr;  // 使用临时指针追踪写入位置
        while (rest > 0)
        {
            res = write(fd, buf, rest);  // 从当前位置写入
            if (res == -1)
            {
                warning("Cannot write more chars in pinfo_dump");
                return;
            }
            rest -= res;
            buf += res;  // 移动指针到未写入的数据位置
        }
    }
}

int pinfo_size(const struct Procinfo *p)
{
    return p->nchars;
}

void pinfo_set_enqueue_time(struct Procinfo *p)
{
    p->enqueue_time = get_monotonic_sec();
    p->start_time = 0;
    p->end_time = 0;
    if (p->boot_time == 0) {
        p->boot_time = time(NULL) - get_monotonic_sec();
    }
}

void pinfo_set_start_time_check(struct Procinfo *info) {
    if (info->start_time == 0) {
        pinfo_set_start_time(info);
    }
}
void pinfo_set_start_time(struct Procinfo *p)
{
    p->start_time = get_monotonic_sec();
    p->end_time = 0;
}

void pinfo_set_end_time(struct Procinfo *p)
{
    p->end_time = get_monotonic_sec();
}


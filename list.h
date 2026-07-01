/*
    Task Spooler PLUS - a multi-user job scheduler like slurm.
    Copyright (C) 2007-2026  Kylin JIANG - Lluís Batlle i Rossell

    Please find the license in the provided COPYING file.
*/
#ifndef LIST_H
#define LIST_H

#include "jobs.h"

char *joblist_headers(void);
char *joblist_line(const struct Job *p);
char *joblist_line_plain(const struct Job *p);
char *joblistdump_torun(const struct Job *p);
char *joblistdump_headers(void);
int is_sleep(const struct Job *p);

#endif /* LIST_H */

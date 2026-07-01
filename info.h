/*
    Task Spooler PLUS - a multi-user job scheduler like slurm.
    Copyright (C) 2007-2026  Kylin JIANG - Lluís Batlle i Rossell

    Please find the license in the provided COPYING file.
*/
#ifndef INFO_H
#define INFO_H

#include "jobs.h"

void pinfo_dump(const struct Procinfo *p, int fd);
void pinfo_addinfo(struct Procinfo *p, int maxsize, const char *line, ...);
void pinfo_free(struct Procinfo *p);
int pinfo_size(const struct Procinfo *p);
void pinfo_set_enqueue_time(struct Procinfo *p);
void pinfo_set_start_time(struct Procinfo *p);
void pinfo_set_start_time_check(struct Procinfo *info);
void pinfo_set_end_time(struct Procinfo *p);
void pinfo_init(struct Procinfo *p);

#endif /* INFO_H */

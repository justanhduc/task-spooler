/*
    Task Spooler PLUS - a multi-user job scheduler like slurm.
    Copyright (C) 2007-2026  Kylin JIANG - Lluís Batlle i Rossell

    Please find the license in the provided COPYING file.
*/
#ifndef CGROUPS_H
#define CGROUPS_H

#include "jobs.h"

void cgroups_create_job(const struct Job *p);
void cgroups_clean_job(const struct Job *p);
void cgroups_clean_all_finished(void);
int cgroups_thaw_job(const struct Job *p);
int cgroups_freeze_job(const struct Job *p);
int cgroups_is_frozen(const struct Job *p);

int cgroups_v1_freeze_ok(int jobid, pid_t pid);
int cgroups_freeze_ok(int jobid, pid_t pid);

#ifdef CGROUP_V2
void cgroups_v2_init(void);
#endif

#ifdef TS_CPU_BIND
void cgroups_set_cpuset(int jobid, pid_t pid, const void *alloc);
void cgroups_restore_all_cpu_bind(void);
#endif

#endif /* CGROUPS_H */

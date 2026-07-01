/*
    Task Spooler PLUS - a multi-user job scheduler like slurm.
    Copyright (C) 2007-2026  Kylin JIANG - Lluís Batlle i Rossell

    Please find the license in the provided COPYING file.
*/
#ifndef EXECUTE_H
#define EXECUTE_H

#include "jobs.h"

int run_job(int jobid, struct Result *res);
void run_on_finish(const char *tmpl, int jobid, const char *output,
                   int exitid, pid_t pid, const char *label,
                   const char *command,
                   time_t real_sec, time_t user_sec, time_t sys_sec,
                   time_t pause_duration,
                   time_t start_time, time_t enqueue_time, time_t end_time,
                   int num_slots);

#endif /* EXECUTE_H */

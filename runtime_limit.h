/*
    Task Spooler - a task queue system for the unix user
    Copyright (C) 2007-2013  Lluís Batlle i Rossell

    Please find the license in the provided COPYING file.
*/
#ifndef RUNTIME_LIMIT_H
#define RUNTIME_LIMIT_H

#include <stdint.h>
#include <time.h>
#include "jobs.h"

typedef struct {
  double value;
  char unit;
} time_repr_t;

time_repr_t format_time(time_t t);
time_t get_cpu_time_by_pid(int pid);
time_t get_max_wall_time(void);
int parse_time(const char *s, time_t *out);
time_t get_work_time_by_job(const struct Job *p);
time_t get_pause_time_by_job(const struct Job *p);
time_t get_monotonic_sec(void);

#endif /* RUNTIME_LIMIT_H */

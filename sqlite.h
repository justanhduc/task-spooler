/*
    Task Spooler PLUS - a multi-user job scheduler like slurm.
    Copyright (C) 2007-2026  Kylin JIANG - Lluís Batlle i Rossell

    Please find the license in the provided COPYING file.
*/
#ifndef SQLITE_H
#define SQLITE_H

#include <stdint.h>
#include "jobs.h"

const char *get_sqlite_path(void);
int open_sqlite(void);
int close_sqlite(void);
int update_field_int64(const char *tableName, int jobid, const char *columnName, int64_t newValue);
int insert_DB(struct Job *job, const char *table);
int insert_or_replace_DB(struct Job *job, const char *table);
struct Job *read_DB(int jobid, const char *table);
int read_jobid_DB(int **jobids, const char *table);
int delete_DB(int jobid, const char *table);
int movetop_DB(int jobid);
int swap_DB(int jobid0, int jobid1);
int set_jobids_DB(int value);
int get_jobids_DB(void);
int set_state_DB(int jobid, int state);

#endif /* SQLITE_H */

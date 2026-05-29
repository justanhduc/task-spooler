/*
    Task Spooler - a task queue system for the unix user
    Copyright (C) 2007-2013  Lluís Batlle i Rossell

    Please find the license in the provided COPYING file.
*/
#ifndef JOB_OPS_H
#define JOB_OPS_H

#include <stdint.h>
#include "msg.h"

void s_list(int s, struct User *user, enum ListFormat listFormat);
void s_list_all(int s, enum ListFormat listFormat);
void s_swap_jobs(int s, int jobid1, int jobid2);
void s_move_urgent(int s, int jobid);
void s_job_info(int s, int jobid);
void s_get_label(int s, int jobid);
void s_send_cmd(int s, int jobid);
void s_send_state(int s, int jobid);
void s_send_last_id(int s);
void s_send_output(int s, int jobid);
int s_remove_job(int s, int *jobid, struct User *client);
void s_sort_jobs(void);
void s_add_wtime(int s, int jobid, int64_t add_wtime);

#endif /* JOB_OPS_H */

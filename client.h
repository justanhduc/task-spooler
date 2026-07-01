/*
    Task Spooler PLUS - a multi-user job scheduler like slurm.
    Copyright (C) 2007-2026  Kylin JIANG - Lluís Batlle i Rossell

    Please find the license in the provided COPYING file.
*/
#ifndef CLIENT_H
#define CLIENT_H

#include "msg.h"

/* client.c */
void c_new_job(void);
void c_list_jobs(void);
void c_list_jobs_all(void);
void c_shutdown_server(void);
void c_wait_server_lines(void);
void c_clear_finished(void);
int c_wait_server_commands(void);
void c_send_runjob_ok(const char *ofname, pid_t pid);
int c_tail(void);
int c_cat(void);
void c_show_output_file(void);
void c_remove_job(void);
void c_show_pid(void);
void c_kill_job(void);
int c_wait_job(void);
int c_wait_running_job(void);
int c_wait_job_recv(void);
void c_move_urgent(void);
int c_wait_newjob_ok(void);
void c_get_state(void);
void c_swap_jobs(void);
void c_show_info(void);
void c_show_last_id(void);
char *build_command_string(void);
void c_send_max_slots(int max_slots);
void c_get_max_slots(void);
void c_check_version(void);
void c_get_count_running(void);
void c_show_label(void);
void c_add_wtime(void);
void c_kill_all_jobs(void);
void c_show_cmd(void);
void c_get_logdir(void);
void c_set_logdir(void);
char *get_logdir(void);
void c_get_env(void);
void c_set_env(void);
void c_unset_env(void);

/* client user operations */
void c_refresh_user(void);
void c_suspend_user(int uid);
void c_resume_user(int uid);
void c_hold_job(int jobid);
void c_cont_job(int jobid);
int c_lock_server(void);
int c_unlock_server(void);
void c_check_daemon(void);
int c_find_pid(pid_t pid, int deep_search);

#endif /* CLIENT_H */

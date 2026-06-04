/*
    Task Spooler PLUS - a multi-user job scheduler like slurm.
    Copyright (C) 2007-2026  Kylin JIANG - Lluís Batlle i Rossell

    Please find the license in the provided COPYING file.
*/
#ifndef MAIN_H
#define MAIN_H

#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include "msg.h"

struct User;

enum Request {
  c_QUEUE,
  c_TAIL,
  c_KILL_SERVER,
  c_LIST,
  c_LIST_ALL,
  c_DAEMON,
  c_CHECK_DAEMON,
  c_REFRESH_USER,
  c_SUSPEND_USER,
  c_RESUME_USER,
  c_LOCK_SERVER,
  c_UNLOCK_SERVER,
  c_HOLD_JOB,
  c_CONT_JOB,
  c_CLEAR_FINISHED,
  c_SHOW_HELP,
  c_SHOW_VERSION,
  c_CAT,
  c_SHOW_OUTPUT_FILE,
  c_SHOW_PID,
  c_REMOVEJOB,
  c_WAITJOB,
  c_URGENT,
  c_GET_STATE,
  c_SWAP_JOBS,
  c_INFO,
  c_SET_MAX_SLOTS,
  c_GET_MAX_SLOTS,
  c_KILL_JOB,
  c_COUNT_RUNNING,
  c_GET_LABEL,
  c_ADD_WTIME,
  c_LAST_ID,
  c_KILL_ALL,
  c_SHOW_CMD,
  c_GET_LOGDIR,
  c_SET_LOGDIR,
  c_GET_ENV,
  c_SET_ENV,
  c_UNSET_ENV,
  c_FIND_PID
};

struct CommandLine {
  enum Request request;
  int need_server;
  int store_output;
  int stderr_apart;
  int should_go_background;
  int should_keep_finished;
  int gzip;
  int *depend_on; /* -1 means depend on previous */
  int depend_on_size;
  int max_slots;
  int jobid;
  int jobid2;
  int wait_enqueuing;
  struct {
    char **array;
    int num;
  } command;
  char *linux_cmd;
  char *label;
  char *logfile;
  char *outfile;
  char *on_finish_cmd;
  pid_t rt_pid;
  char *rt_output;
  time_t rt_real_sec;
  time_t rt_user_sec;
  time_t rt_system_sec;
  time_t rt_pause_duration;
  time_t rt_start_time;
  time_t rt_enqueue_time;
  time_t rt_end_time;
  int rt_num_slots;
  int num_slots;
  int taskpid;
  int require_elevel;
  int no_cpu_binding;
  int no_bind_defrag;
  int n_retry;
  time_t start_time;
  int64_t wall_time;
  int64_t schedule_time;
  enum ListFormat list_format;
};

enum ProcessType { CLIENT, SERVER };

/* Global variables */
extern struct CommandLine command_line;
extern enum ProcessType process_type;
extern int server_socket;
extern char *logdir;
extern int term_width;
extern struct User *user_locker;
extern time_t locker_time;
extern int jobsort_flag;
extern int client_uid;

/* main.c */
struct Msg default_msg(void);
struct Result default_result(void);

#endif /* MAIN_H */

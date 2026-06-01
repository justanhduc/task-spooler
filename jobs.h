/*
    Task Spooler - a task queue system for the unix user
    Copyright (C) 2007-2013  Lluís Batlle i Rossell

    Please find the license in the provided COPYING file.
*/
#ifndef JOB_H
#define JOB_H

#include <stdint.h>
#include <stdio.h>
#include <sys/types.h>
#include <time.h>
#include "vec.h"

struct Msg;

enum Jobstate {
  QUEUED,
  RUNNING,
  PAUSE,
  FINISHED,
  SKIPPED,
  HOLDING_CLIENT,
  WAIT,
  DELINK,
  LOCKED,
};

struct Result {
  int errorlevel;
  int died_by_signal;
  int signal;
  time_t user_sec;
  time_t system_sec;
  time_t real_sec;
  int skipped;
};

struct Procinfo {
  char *ptr;
  int nchars;
  int allocchars;
  time_t enqueue_time;
  time_t start_time;
  time_t end_time;
  time_t pause_time;
  time_t pause_duration;
  time_t boot_time;
};

struct Job {
  int jobid;
  char *command;
  char *work_dir;
  int command_strip;
  enum Jobstate state;
  struct Result result;
  char *output_filename;
  int store_output;
  pid_t pid;
  struct User *user;
  int64_t wall_time;
  int64_t schedule_time;
  int should_keep_finished;
  int *depend_on;
  int depend_on_size;
  int *notify_errorlevel_to;
  int notify_errorlevel_to_size;
  int dependency_errorlevel;
  char *label;
  char *email;
  struct Procinfo info;
  int num_slots;
  int num_allocated;
  int no_cpu_binding;
  int client_socket;
#ifdef TS_CPU_BIND
  void *cpu_alloc;              /* struct CpuAlloc * 由 cpu_bind 管理 */
#endif
};

enum ExitCodes {
  EXITCODE_OK = 0,
  EXITCODE_UNKNOWN_ERROR = -1,
  EXITCODE_QUEUE_FULL = 2,
};

enum { DEFAULT_MAXFINISHED = 1000 };

/* Exported globals from jobs.c */
extern vec_t active_jobs;
extern vec_t finished_jobs;
extern int busy_slots;
extern int max_slots;
extern char buff[256];
extern int max_jobs;
extern struct User *user_locker;
extern time_t locker_time;
extern char *email_sender;
extern time_t sstmp_skip_sec;

/* Job queue operations */
void init_jobs(void);
void destroy_jobs(void);
struct Job *findjob(int jobid);
int findjob_idx(int jobid);
struct Job *get_job(int jobid);
struct Job *find_finished_job(int jobid);
const char *jstate2string(enum Jobstate s);
int job_is_running(int jobid);
int job_is_holding_client(int jobid);
int wake_hold_client(void);
int next_run_job(void);
void s_set_jobids(int i);
int s_newjob(int s, struct Msg *m, struct User *user);
void s_delete_job(int jobid);
void job_finished(const struct Result *result, int jobid);
void s_mark_job_running(int jobid);
void s_clear_finished(struct User *user);
void s_process_runjob_ok(int jobid, char *oname, int pid);
void s_send_runjob(int s, int jobid);
void s_send_newjob_ok(int socket, int jobid);
void s_set_max_slots(int s, int new_max_slots);
void s_get_max_slots(int s);
void s_kill_all_jobs(int s, struct User *user);
void s_count_running_jobs(int s, struct User *user);
void s_check_holdon(void);

int s_check_running_pid(pid_t pid);
void s_read_sqlite(void);
int s_find_pid(pid_t target_pid, int deep_search);
int s_update_slots_usage(void);
void send_list_line(int s, const char *str);
void setup_ssmtp(void);
void notify_errorlevel(struct Job *p);
void dump_jobs_struct(FILE *out);
void dump_notifies_struct(FILE *out);
void joblist_dump(int fd);
int safe_pause_job(struct Job *p);
void destroy_job(struct Job *p);
void destroy_finished_job(struct Job *j);
void pause_job_config(struct Job *p);
void rerun_job_config(struct Job *p);
void free_cores(struct Job *p);
int config_running(struct Job *p);

#endif /* JOB_H */

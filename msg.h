/*
    Task Spooler - a task queue system for the unix user
    Copyright (C) 2007-2013  Lluís Batlle i Rossell

    Please find the license in the provided COPYING file.
*/
#ifndef MSG_H
#define MSG_H

#include "jobs.h"

enum { CMD_LEN = 500, PROTOCOL_VERSION = 731 };

enum MsgTypes {
  KILL_SERVER,
  NEWJOB,
  NEWJOB_OK,
  RUNJOB,
  RUNJOB_OK,
  ENDJOB,
  LIST,
  LIST_ALL,
  LIST_LINE,
  REFRESH_USERS,
  HOLD_JOB,
  CONT_JOB,
  LOCK_SERVER,
  UNLOCK_SERVER,
  SUSPEND_USER,
  RESUME_USER,
  CLEAR_FINISHED,
  ASK_OUTPUT,
  ANSWER_OUTPUT,
  REMOVEJOB,
  REMOVEJOB_OK,
  REMOVEJOB_NOK,
  WAITJOB,
  WAIT_RUNNING_JOB,
  WAITJOB_OK,
  URGENT,
  URGENT_OK,
  GET_STATE,
  ANSWER_STATE,
  SWAP_JOBS,
  SWAP_JOBS_OK,
  INFO,
  INFO_DATA,
  SET_MAX_SLOTS,
  GET_MAX_SLOTS,
  GET_MAX_SLOTS_OK,
  GET_VERSION,
  VERSION,
  NEWJOB_NOK,
  NEWJOB_PID_NOK,
  COUNT_RUNNING,
  GET_LABEL,
  ADD_WTIME,
  LAST_ID,
  KILL_ALL,
  GET_CMD,
  GET_LOGDIR,
  SET_LOGDIR,
  GET_ENV,
  SET_ENV,
  UNSET_ENV,
  ERROR_INFO,
  FIND_PID,
  FIND_PID_RESULT,
  RECONNECT,
  RECONNECT_OK,
  ENDJOB_OK,
};

enum ListFormat { DEFAULT, JSON, TAB };

struct Msg {
  enum MsgTypes type;
  int jobid;
  union {
    struct {
      int command_size;
      int command_size_strip;
      int path_size;
      int store_output;
      int should_keep_finished;
      int label_size;
      int email_size;
      int env_size;
      int depend_on_size;
      int wait_enqueuing;
      int num_slots;
      int taskpid;
      time_t start_time;
      int64_t wall_time;
      int64_t schedule_time;
    } newjob;
    struct {
      int ofilename_size;
      int store_output;
      pid_t pid;
    } output;
    struct Result result;
    int size;
    enum Jobstate state;
    struct {
      int jobid1;
      int jobid2;
    } swap;
    int last_errorlevel;
    int max_slots;
    int version;
    int count_running;
    char *label;
    struct {
      int term_width;
      enum ListFormat list_format;
    } list;
    struct {
      pid_t pid;
      int deep;
    } find_pid;
    struct {
      time_t real_sec;
      time_t user_sec;
      time_t system_sec;
      time_t pause_duration;
      time_t start_time;
      time_t enqueue_time;
      time_t end_time;
      int num_slots;
    } finish_info;
    struct {
      pid_t pid;
    } reconnect;
  } u;
};

/* msg.c */
void send_bytes(int fd, const char *data, int bytes);
int recv_bytes(int fd, char *data, int bytes);
void send_msg(int fd, const struct Msg *m);
int recv_msg(int fd, struct Msg *m);
void send_ints(int fd, const int *data, int num);
int *recv_ints(int fd, int *num);

#endif /* MSG_H */

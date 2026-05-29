/*
    Task Spooler - a task queue system for the unix user
    Copyright (C) 2007-2013  Lluís Batlle i Rossell

    Please find the license in the provided COPYING file.
*/
#ifndef USER_H
#define USER_H

#include <stdint.h>
#include <sys/types.h>
#include "vec.h"

#define USER_NAME_WIDTH 256
#define USER_MAX 100
#define root_UID 0

struct Job;

struct User {
    char name[USER_NAME_WIDTH];
    uid_t uid;
    int max_slots;
    int busy;
    int jobs;
    int queue;
    int locked;
};

extern vec_t users_vec;
extern int server_uid;
extern char *logfile_path;

#define USER(ts) ((struct User *)vec_get(&users_vec, (size_t)(ts)))

const char *get_kill_sh_path(void);
void read_user_file(const char *path);
int get_tsUID(int uid);
void c_refresh_user(void);
const char *get_user_path(void);
const char *set_server_logfile(void);
void write_logfile(const struct Job *p);
int get_env(const char *env, int v0);
const char *uid2user_name(int uid);
int read_first_jobid_from_logfile(const char *path);
void kill_pids(int ppid, int signal, const char *cmd);

#endif /* USER_H */

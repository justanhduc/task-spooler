/*
    Task Spooler - a task queue system for the unix user
    Copyright (C) 2007-2013  Lluís Batlle i Rossell

    Please find the license in the provided COPYING file.
*/
#ifndef SERVER_USER_H
#define SERVER_USER_H

#include "user.h"

void s_user_status_all(int s);
void s_user_status(int s, struct User *user);
void s_refresh_users(int s);
struct User *s_get_job_user(int jobid);
void s_suspend_user_all(int s);
void s_suspend_user(int s, struct User *user);
void s_resume_user(int s, struct User *user);
void s_resume_user_all(int s);
void s_hold_job(int s, int jobid, struct User *user);
void s_cont_job(int s, int jobid, struct User *user);
void s_lock_server(int s, struct User *user);
void s_unlock_server(int s, struct User *user);
int s_check_locker(struct User *user);

#endif /* SERVER_USER_H */

/*
    Task Spooler - a task queue system for the unix user
    Copyright (C) 2007-2013  Lluís Batlle i Rossell

    Please find the license in the provided COPYING file.

    Build-time defaults. Override at runtime via environment variables.
    TS_SOCKET, TS_USER_PATH, TS_LOGFILE_PATH, TS_SQLITE_PATH, TS_MAXFINISHED,
    TS_MAX_WALL_TIME, TS_SORTJOBS, TS_SAVELIST, TS_ONFINISH.
*/
#ifndef DEFAULTS_H
#define DEFAULTS_H

#define DEFAULT_USER_PATH "/home/kylin/Public/task-spooler/user.txt"
#define DEFAULT_LOG_PATH "/home/kylin/Public/task-spooler/log.txt"
#define DEFAULT_SQLITE_PATH "/home/kylin/Public/task-spooler/task-spooler.db"
#define DEFAULT_EMAIL_SENDER "XXX@foxmail.com"
#define DEFAULT_EMAIL_TIME 45.0
#define DEFAULT_USER_LOCK_TIME 30
#define DEFAULT_ROOT_LOCK_TIME 86400
#define DEFAULT_MAX_WALL_TIME 604800 /* 7 days in seconds */
#define DEFAULT_HPC_NAME "intel_laptop"

#endif /* DEFAULTS_H */

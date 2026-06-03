/*
    Task Spooler PLUS - a multi-user job scheduler like slurm.
    Copyright (C) 2007-2026  Kylin JIANG - Lluís Batlle i Rossell

    Please find the license in the provided COPYING file.

    Build-time defaults. Override at runtime via environment variables.
    TS_SOCKET, TS_USER_PATH, TS_LOGFILE_PATH, TS_SQLITE_PATH, TS_MAXFINISHED,
    TS_MAX_WALL_TIME, TS_SORTJOBS, TS_SAVELIST, TS_ONFINISH.
*/
#ifndef DEFAULTS_H
#define DEFAULTS_H

#define DEFAULT_BASE_DIR "/home/kylin/Public/task-spooler"
#define DEFAULT_USER_PATH DEFAULT_BASE_DIR "/user.txt"
#define DEFAULT_LOG_PATH DEFAULT_BASE_DIR "/log.txt"
#define DEFAULT_SQLITE_PATH DEFAULT_BASE_DIR "/task-spooler.db"
#define DEFAULT_USER_LOCK_TIME 30
#define DEFAULT_ROOT_LOCK_TIME 86400
#define DEFAULT_MAX_WALL_TIME 604800 /* 7 days in seconds */
#define DEFAULT_HPC_NAME "intel_laptop"

#endif /* DEFAULTS_H */

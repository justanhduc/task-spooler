/*
    Task Spooler PLUS - a multi-user job scheduler like slurm.
    Copyright (C) 2007-2026  Kylin JIANG - Lluís Batlle i Rossell

    Please find the license in the provided COPYING file.
*/
#ifndef SERVER_ENV_H
#define SERVER_ENV_H

void s_get_logdir(int s);
void s_set_logdir(const char *path);
void s_get_env(int s, int size);
void s_set_env(int s, int size);
void s_unset_env(int s, int size);

#endif /* SERVER_ENV_H */

/*
    Task Spooler PLUS - a multi-user job scheduler like slurm.
    Copyright (C) 2007-2026  Kylin JIANG - Lluís Batlle i Rossell

    Please find the license in the provided COPYING file.
*/
#ifndef SIGNALS_H
#define SIGNALS_H

extern int signals_child_pid;

void ignore_sigpipe(void);
void restore_sigmask(void);
void block_sigint(void);
void unblock_sigint_and_install_handler(void);

#endif /* SIGNALS_H */

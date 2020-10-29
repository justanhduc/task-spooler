/*
    Task Spooler - a task queue system for the unix user
    Copyright (C) 2007-2009  Lluís Batlle i Rossell

    Please find the license in the provided COPYING file.
*/
#include <signal.h>
#include <stdio.h>
#include <stdlib.h> /* for NULL */
#include <sys/time.h> /* for NULL */
#include "main.h"

/* Some externs refer to this variable */
static sigset_t normal_sigmask;

/* as extern in execute.c */
int signals_child_pid; /* 0, not set. otherwise, set. */

void ignore_sigpipe()
{
    sigset_t set;

    sigemptyset(&set);
    sigaddset(&set, SIGPIPE);
    sigprocmask(SIG_BLOCK, &set, &normal_sigmask);
}

void restore_sigmask()
{
    sigprocmask(SIG_SETMASK, &normal_sigmask, NULL);
}

void sigint_handler(int s)
{
    if (signals_child_pid)
    {
        kill(signals_child_pid, SIGINT);
    } else
    {
        /* ts client killed by SIGINT */
        exit(1);
    }
}

void block_sigint()
{
    sigset_t set;

    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    /* ignore_sigpipe() will always be called first, and
     * only that sets the normal_sigmask. */
    sigprocmask(SIG_BLOCK, &set, 0);
}

void unblock_sigint_and_install_handler()
{
    sigset_t set;
    struct sigaction act;

    /* Install the handler */
    act.sa_handler = sigint_handler;
    sigemptyset(&act.sa_mask);
    act.sa_flags = 0;
    sigaction(SIGINT, &act, 0);

    /* Unblock the signal */
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    /* ignore_sigpipe() will always be called first, and
     * only that sets the normal_sigmask. */
    sigprocmask(SIG_UNBLOCK, &set, 0);
}

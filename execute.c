/*
    Task Spooler - a task queue system for the unix user
    Copyright (C) 2007-2009  Lluís Batlle i Rossell

    Please find the license in the provided COPYING file.
*/
#include <unistd.h>
#include <stdio.h>
#include <signal.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <stdlib.h>
#include <signal.h>
#include <time.h>
#include <sys/times.h>
#include <sys/time.h>
#include <sys/types.h>
#include <fcntl.h>
#include <assert.h>

#include "main.h"

/* from signals.c */
extern int signals_child_pid; /* 0, not set. otherwise, set. */

/* Returns errorlevel */
static void run_parent(int fd_read_filename, int pid, struct Result *result)
{
    int status;
    char *ofname = 0;
    int namesize;
    int res;
    char *command;
    struct timeval starttv;
    struct timeval endtv;
    struct tms cpu_times;

    /* Read the filename */
    /* This is linked with the write() in this same file, in run_child() */
    if (command_line.store_output) {
        res = read(fd_read_filename, &namesize, sizeof(namesize));
        if (res == -1)
            error("read the filename from %i", fd_read_filename);
        if (res != sizeof(namesize))
            error("Reading the size of the name");
        ofname = (char *) malloc(namesize);
        res = read(fd_read_filename, ofname, namesize);
        if (res != namesize)
            error("Reading the out file name");
    }
    res = read(fd_read_filename, &starttv, sizeof(starttv));
    if (res != sizeof(starttv))
        error("Reading the the struct timeval");
    close(fd_read_filename);

    /* All went fine - prepare the SIGINT and send runjob_ok */
    signals_child_pid = pid;
    unblock_sigint_and_install_handler();

    c_send_runjob_ok(ofname, pid);

    wait(&status);

    /* Set the errorlevel */
    if (WIFEXITED(status))
    {
        /* We force the proper cast */
        signed char tmp;
        tmp = WEXITSTATUS(status);
        result->errorlevel = tmp;
        result->died_by_signal = 0;
    }
    else if (WIFSIGNALED(status))
    {
        signed char tmp;
        tmp = WTERMSIG(status);
        result->signal = tmp;
        result->errorlevel = -1;
        result->died_by_signal = 1;
    }
    else
    {
        result->died_by_signal = 0;
        result->errorlevel = -1;
    }

    command = build_command_string();
    if (command_line.send_output_by_mail)
    {
        send_mail(command_line.jobid, result->errorlevel, ofname, command);
    }
    hook_on_finish(command_line.jobid, result->errorlevel, ofname, command);
    free(command);

    free(ofname);

    /* Calculate times */
    gettimeofday(&endtv, NULL);
    result->real_ms = endtv.tv_sec - starttv.tv_sec +
        ((float) (endtv.tv_usec - starttv.tv_usec) / 1000000.);
    times(&cpu_times);
    /* The times are given in clock ticks. The number of clock ticks per second
     * is obtained in POSIX using sysconf(). */
    result->user_ms = (float) cpu_times.tms_cutime /
        (float) sysconf(_SC_CLK_TCK);
    result->system_ms = (float) cpu_times.tms_cstime /
        (float) sysconf(_SC_CLK_TCK);
}

void create_closed_read_on(int dest)
{
    int p[2];
    /* Closing input */
    pipe(p);
    close(p[1]); /* closing the write handle */
    dup2(p[0], dest); /* the pipe reading goes to dest */
    if(p[0] != dest)
        close(p[0]);
}

/* This will close fd_out and fd_in in the parent */
static void run_gzip(int fd_out, int fd_in)
{
    int pid;
    pid = fork();

    switch(pid)
    {
        case 0: /* child */
            restore_sigmask();
            dup2(fd_in,0); /* stdout */
            dup2(fd_out,1); /* stdout */
            close(fd_in);
            close(fd_out);
            /* Without stderr */
            close(2);
            execlp("gzip", "gzip", NULL);
            exit(-1);
            /* Won't return */
       case -1:
            exit(-1); /* Fork error */
       default:
            close(fd_in);
            close(fd_out);
    }
}

static void run_child(int fd_send_filename)
{
    char outfname[] = "/ts-out.XXXXXX";
    char errfname[sizeof outfname + 2]; /* .e */
    int namesize;
    int outfd;
    int err;
    struct timeval starttv;

    if (command_line.store_output)
    {
        /* Prepare path */
        const char *tmpdir = getenv("TMPDIR");
        int lname;
        char *outfname_full;

        if (tmpdir == NULL)
            tmpdir = "/tmp";
        lname = strlen(tmpdir) + strlen(outfname) + 1 /* \0 */;

        outfname_full = (char *)malloc(lname);
        strcpy(outfname_full, tmpdir);
        strcat(outfname_full, outfname);

        if (command_line.gzip)
        {
            int p[2];
            /* We assume that all handles are closed*/
            err = pipe(p);
            assert(err == 0);

            /* gzip output goes to the filename */
            /* This will be the handle other than 0,1,2 */
            /* mkstemp doesn't admit adding ".gz" to the pattern */
            outfd = mkstemp(outfname_full); /* stdout */
            assert(outfd != -1);

            /* Program stdout and stderr */
            /* which go to pipe write handle */
            err = dup2(p[1], 1);
            assert(err != -1);
            if (command_line.stderr_apart)
            {
                int errfd;
                strncpy(errfname, outfname_full, sizeof errfname);
                strncat(errfname, ".e", 2);
                errfd = open(errfname, O_CREAT | O_WRONLY | O_TRUNC, 0600);
                assert(err == 0);
                err = dup2(errfd, 2);
                assert(err == 0);
                err = close(errfd);
                assert(err == 0);
            }
            else
            {
                err = dup2(p[1], 2);
                assert(err != -1);
            }
            err = close(p[1]);
            assert(err == 0);

            /* run gzip.
             * This wants p[0] in 0, so gzip will read
             * from it */
            run_gzip(outfd, p[0]);
        }
        else
        {
            /* Prepare the filename */
            outfd = mkstemp(outfname_full); /* stdout */
            dup2(outfd, 1); /* stdout */
            if (command_line.stderr_apart)
            {
                int errfd;
                strncpy(errfname, outfname_full, sizeof errfname);
                strncat(errfname, ".e", 2);
                errfd = open(errfname, O_CREAT | O_WRONLY | O_TRUNC, 0600);
                dup2(errfd, 2);
                close(errfd);
            }
            else
                dup2(outfd, 2);
            close(outfd);
        }

        /* Send the filename */
        namesize = strlen(outfname_full)+1;
        write(fd_send_filename, (char *)&namesize, sizeof(namesize));
        write(fd_send_filename, outfname_full, namesize);
    }
    /* Times */
    gettimeofday(&starttv, NULL);
    write(fd_send_filename, &starttv, sizeof(starttv));
    close(fd_send_filename);

    /* Closing input */
    if (command_line.should_go_background)
        create_closed_read_on(0);

    /* We create a new session, so we can kill process groups as:
         kill -- -`ts -p` */
    setsid();
    execvp(command_line.command.array[0], command_line.command.array);
}

int run_job(struct Result *res)
{
    int pid;
    int errorlevel;
    int p[2];


    /* For the parent */
    /*program_signal(); Still not needed*/

    block_sigint();

    /* Prepare the output filename sending */
    pipe(p);

    pid = fork();

    switch(pid)
    {
        case 0:
            restore_sigmask();
            close(server_socket);
            close(p[0]);
            run_child(p[1]);
            /* Not reachable, if the 'exec' of the command
             * works. Thus, command exists, etc. */
            fprintf(stderr, "ts could not run the command\n");
            exit(-1);
            /* To avoid a compiler warning */
            errorlevel = 0;
            break;
        case -1:
            /* To avoid a compiler warning */
            errorlevel = 0;
            error("forking");
        default:
            close(p[1]);
            run_parent(p[0], pid, res);
            break;
    }

    return errorlevel;
}

#if 0
Not needed
static void sigchld_handler(int val)
{
}

static void program_signal()
{
  struct sigaction act;

  act.sa_handler = sigchld_handler;
  /* Reset the mask */
  memset(&act.sa_mask,0,sizeof(act.sa_mask));
  act.sa_flags = SA_NOCLDSTOP;

  sigaction(SIGCHLD, &act, NULL);
}
#endif

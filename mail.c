/*
    Task Spooler - a task queue system for the unix user
    Copyright (C) 2007-2009  Lluís Batlle i Rossell

    Please find the license in the provided COPYING file.
*/
#include <signal.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/time.h> /* Needed for any main.h inclusion */

#include "main.h"

/* Returns the write pipe */
static int run_sendmail(const char *dest)
{
    int pid;
    int p[2];

    pipe(p);

    pid = fork();

    switch(pid)
    {
        case 0: /* Child */
            restore_sigmask();
            close(0);
            close(1);
            close(2);
	    close(p[1]);
            dup2(p[0], 0);
            execl("/usr/sbin/sendmail", "sendmail", "-oi", dest, NULL);
            error("run sendmail");
        case -1:
            error("fork sendmail");
        default: /* Parent */
	    close(p[0]);
    }
    return p[1];
}

static void write_header(int fd, const char *dest, const char * command,
        int jobid, int errorlevel)
{
    fd_nprintf(fd, 100, "From: Task Spooler <taskspooler>\n");
    fd_nprintf(fd, 500, "To: %s\n", dest);
    fd_nprintf(fd, 500, "Subject: the task %i finished with error %i. \n", jobid,
            errorlevel);
    fd_nprintf(fd, 500, "\nCommand: %s\n", command);
    fd_nprintf(fd, 500, "Output:\n");
}

static void copy_output(int write_fd, const char *ofname)
{
    int file_fd;
    char buffer[1000];
    int read_bytes;
    int res;

    file_fd = open(ofname, O_RDONLY);
    if (file_fd == -1)
        error("mail: Cannot open the output file %s", ofname);

    do {
        read_bytes = read(file_fd, buffer, 1000);
        if (read_bytes > 0)
        {
            res = write(write_fd, buffer, read_bytes);
            if (res == -1)
                warning("Cannot write to the mail pipe %i", write_fd);
        }
    } while (read_bytes > 0);
    if (read_bytes == -1)
        warning("Cannot read the output file %s from %i", ofname, file_fd);
}

void hook_on_finish(int jobid, int errorlevel, const char *ofname,
    const char *command)
{
    char *onfinish;
    int pid;
    char sjobid[20];
    char serrorlevel[20];
    int status;

    onfinish = getenv("TS_ONFINISH");
    if (onfinish == NULL)
        return;

    pid = fork();

    switch(pid)
    {
        case 0: /* Child */
            restore_sigmask();
            sprintf(sjobid, "%i", jobid);
            sprintf(serrorlevel, "%i", errorlevel);
            execlp(onfinish, onfinish, sjobid, serrorlevel, ofname, command,
                    NULL);
        case -1:
            error("fork on finish");
        default: /* Parent */
            wait(&status);
    }
}

void send_mail(int jobid, int errorlevel, const char *ofname,
    const char *command)
{
    char to[101];
    char *user;
    char *env_to;
    int write_fd;
    int status;

    env_to = getenv("TS_MAILTO");

    if (env_to == NULL || strlen(env_to) > 100)
    {
        user = getenv("USER");
        if (user == NULL)
            user = "nobody";

        strcpy(to, user);
        /*strcat(to, "@localhost");*/
    } else
        strcpy(to, env_to);

    write_fd = run_sendmail(to);
    write_header(write_fd, to, command, jobid, errorlevel);
    copy_output(write_fd, ofname);
    close(write_fd);
    wait(&status);
}

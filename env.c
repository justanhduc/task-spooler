/*
    Task Spooler - a task queue system for the unix user
    Copyright (C) 2007-2009  Lluís Batlle i Rossell

    Please find the license in the provided COPYING file.
*/
#include <stdio.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <stdlib.h>
#include "main.h"

static int fork_command(const char *command)
{
    int pid;
    int p[2];
    int fdnull;

    fdnull = open("/dev/null", O_RDONLY);
    if (fdnull == -1) error("Cannot open /dev/null");

    pipe(p);

    pid = fork();

    switch(pid)
    {
        case 0:
            restore_sigmask();
            close(server_socket);
            dup2(fdnull, 0);
            if (fdnull != 0)
                close(fdnull);
            dup2(p[1], 1);
            dup2(p[1], 2);
            if (p[1] != 1 && p[1] != 2)
                close(p[1]);
            close(p[0]);
            execlp("/bin/sh", "/bin/sh", "-c", command, (char*)NULL);
            error("/bin/sh exec error");
        case -1:
            error("Fork error");
        default:
            close(p[1]);
    }

    return p[0];
}

char * get_environment()
{
    char *ptr;
    char *command;
    int readfd;
    int bytes = 0;
    int alloc = 0;
    
    command = getenv("TS_ENV");
    if (command == 0)
        return 0;

    readfd = fork_command(command);

    ptr = 0;
    do
    {
        int next;
        int res;
        next = bytes + 1000;
        if (next > alloc)
        {
            ptr = realloc(ptr, next);
            alloc = next;
        }
        res = read(readfd, ptr + bytes, 1000);
        if (res < 0)
            error("Cannot read from the TS_ENV command (%s)", command);
        else if (res == 0)
            break;
        else
            bytes += res;
    } while(1);

    /* We will always have 1000 bytes more to be written, on end.
     * We add a null for it to be an ASCIIZ string. */
    ptr[bytes] = '\0';

    close(readfd);
    wait(0);

    return ptr;
}

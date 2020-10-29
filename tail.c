/*
    Task Spooler - a task queue system for the unix user
    Copyright (C) 2007-2009  Lluís Batlle i Rossell

    Please find the license in the provided COPYING file.
*/
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <sys/select.h>
#include <stdlib.h>

#include <sys/time.h> /* Dep de main.h */

#include "main.h"

enum { BSIZE=1024 };

static int min(int a, int b)
{
    if (a < b)
        return a;
    return b;
}

static int max(int a, int b)
{
    if (a > b)
        return a;
    return b;
}

static void tail_error(const char *str)
{
    fprintf(stderr, "%s", str);
    fprintf(stderr, ". errno: %i (%s)\n",
                    errno, strerror(errno));
    exit(-1);
}

static void seek_at_last_lines(int fd, int lines)
{
    char buf[BSIZE];
    int lines_found = 0;
    int last_lseek = BSIZE;
    int last_read = 0; /* Only to catch not doing any loop */
    int move_offset;
    int i = -1; /* Only to catch not doing any loop */

    last_lseek = lseek(fd, 0, SEEK_END);

    do
    {
        int next_read;
        next_read = min(last_lseek, BSIZE);

        /* we should end looping if last_lseek == 0
         * This means we already read all the file. */
        if (next_read <= 0)
            break;

        /* last_lseek will be -1 at the beginning of the file,
         * if we wanted to go farer than it. */
        last_lseek = lseek(fd, -next_read, SEEK_CUR);

        if (last_lseek == -1)
            last_lseek = lseek(fd, 0, SEEK_SET);

        last_read = read(fd, buf, next_read);
        if (last_read == -1)
        {
            if (errno == EINTR)
                continue;
            tail_error("Error reading");
        }


        for(i = last_read-1; i >= 0; --i)
        {
            if (buf[i] == '\n')
            {
                ++lines_found;
                if (lines_found > lines)
                    /* We will use 'i' to calculate where to
                     * put the file cursor, before return. */
                    break;
            }
        }
        
        /* Go back the read bytes */
        last_lseek = lseek(fd, -last_read, SEEK_CUR);
    } while(lines_found < lines);

    /* Calculate the position */
    move_offset = i+1;
    lseek(fd, move_offset, SEEK_CUR);
}

static void set_non_blocking(int fd)
{
    long arg;

    arg = O_RDONLY | O_NONBLOCK;
    fcntl(fd, F_SETFL, arg);
}

/* if last_lines == -1, go on from the start of the file */
int tail_file(const char *fname, int last_lines)
{
    int fd;
    int res;
    int waiting_end = 1;
    int end_res = 0;
    int endfile_reached = 0;
    int could_write = 1;

    fd_set readset, errorset;

    fd = open(fname, O_RDONLY);

    if (fd == -1)
        tail_error("Error: cannot open the output file");

    if (last_lines >= 0)
        seek_at_last_lines(fd, last_lines);

    /* we don't want the next read calls to block. */
    set_non_blocking(fd);

    do
    {
        char buf[BSIZE];
        int maxfd;

        FD_ZERO(&readset);
        maxfd = -1;
        if (!endfile_reached)
        {
            FD_SET(fd, &readset);
            maxfd = fd;
        }
        if (waiting_end)
        {
            FD_SET(server_socket, &readset);
            maxfd = max(maxfd, server_socket);
        }

        FD_ZERO(&errorset);
        FD_SET(1, &errorset);
        maxfd = max(maxfd, server_socket);

        /* If we don't have fd's to wait for, let's sleep */
        if (maxfd == -1)
        {
            usleep(1 /* sec */* 1000000);
        } else
        {
            /* Otherwise, do a normal select */
            struct timeval tv = {1 /*sec*/, 0 };
            res = select(maxfd + 1, &readset, 0, &errorset, &tv);
        }

        if (FD_ISSET(server_socket, &readset))
        {
            end_res = c_wait_job_recv();
            waiting_end = 0;
        }

        /* We always read when select awakes */
        res = read(fd, buf, BSIZE);
        if (res == -1)
        {
            if (errno == EINTR || errno == EAGAIN)
            {
                res = 1; /* Hack for the while condition */
                continue;
            }
            tail_error("Error reading");
        }

        if (res == 0)
            endfile_reached = 1;
        else
            endfile_reached = 0;

        if (!FD_ISSET(1, &errorset))
        {
            while(res > 0)
            {
                int wres;
                wres = write(1, buf, res);
                /* Maybe the listener doesn't want to receive more */
                if (wres < 0)
                {
                    could_write = 0;
                    break;
                }
                res -= wres;
            }
        }
        else
        {
            could_write = 0;
            break;
        }
    } while((!endfile_reached || waiting_end) && could_write);

    close(fd);

    return end_res;
}

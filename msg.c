/*
    Task Spooler - a task queue system for the unix user
    Copyright (C) 2007-2009  Lluís Batlle i Rossell

    Please find the license in the provided COPYING file.
*/
#include <sys/types.h>
#include <sys/socket.h>
#include <stdio.h>
#include <sys/time.h>
#include <stdlib.h>
#include "main.h"

void send_bytes(const int fd, const char *data, int bytes)
{
    int res;
    int offset = 0;

    while(1)
    {
        res = send(fd, data + offset, bytes, 0);
        if(res == -1)
        {
            warning("Sending %i bytes to %i.", bytes, fd);
            break;
        }
        if(res == bytes)
            break;
        offset += res;
        bytes -= res;
    }
}

int recv_bytes(const int fd, char *data, int bytes)
{
    int res;
    int offset = 0;

    while(1)
    {
        res = recv(fd, data + offset, bytes, 0);
        if(res == -1)
        {
            warning("Receiving %i bytes from %i.", bytes, fd);
            break;
        }
        if(res == bytes)
            break;
        offset += res;
        bytes -= res;
    }

    return res;
}

void send_msg(const int fd, const struct msg *m)
{
    int res;

    if (0)
        msgdump(stderr, m);
    res = send(fd, m, sizeof(*m), 0);
    if(res == -1 || res != sizeof(*m))
        warning_msg(m, "Sending a message to %i, sent %i bytes, should "
                "send %i.", fd,
                res, sizeof(*m));
}

int recv_msg(const int fd, struct msg *m)
{
    int res;

    res = recv(fd, m, sizeof(*m), 0);
    if(res == -1)
        warning_msg(m, "Receiving a message from %i.", fd);
    if (res == sizeof(*m) && 0)
        msgdump(stderr, m);
    if (res != sizeof(*m) && res > 0)
        warning_msg(m, "Receiving a message from %i, received %i bytes, "
                "should have received %i.", fd,
                res, sizeof(*m));

    return res;
}

/*
    Task Spooler - a task queue system for the unix user
    Copyright (C) 2007-2013  Lluís Batlle i Rossell

    Please find the license in the provided COPYING file.
*/
#ifndef SERVER_H
#define SERVER_H

#include <stdint.h>
#include "msg.h"

struct ucred {
  uint32_t pid;
  uint32_t uid;
  uint32_t gid;
};

struct User;

struct Client_conn {
  int socket;
  int hasjob;
  int jobid;
  struct User *user;
};

enum { MAXCONN = 1000 };

extern char *logdir;

/* server.c */
void server_main(int notify_fd, char *_path);
void dump_conns_struct(FILE *out);

#endif /* SERVER_H */

/*
    Task Spooler - a task queue system for the unix user
    Copyright (C) 2007-2009  Lluís Batlle i Rossell

    Please find the license in the provided COPYING file.
*/
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#include "main.h"

int server_socket;

static char *socket_path;
static int should_check_owner = 0;

static int fork_server();

void create_socket_path(char **path) {
  char *tmpdir;
  char userid[20] = "root";
  int size;

  /* As a priority, TS_SOCKET mandates over the path creation */
  *path = getenv("TS_SOCKET");
  if (*path != 0) {
    /* We need this in our memory, for forks and future 'free'. */
    size = strlen(*path) + 1;
    *path = (char *)malloc(size);
    strcpy(*path, getenv("TS_SOCKET"));

    /* We don't want to check ownership of the socket here,
     * as the user may have thought of some shared queue */
    should_check_owner = 0;
    return;
  }

  /* ... if the $TS_SOCKET doesn't exist ... */
  /* Create the path */
  tmpdir = getenv("TMPDIR");
  if (tmpdir == NULL)
    tmpdir = "/tmp";

  /* Calculate the size */
  size = strlen(tmpdir) + strlen("/socket-ts.") + strlen(userid) + 1;

  /* Freed after preparing the socket address */
  *path = (char *)malloc(size);

  snprintf(*path, size, "%s/socket-ts.%s", tmpdir, userid);

  should_check_owner = 1;
}

int try_connect(int s) {
  struct sockaddr_un addr;
  int res;

  addr.sun_family = AF_UNIX;
  strcpy(addr.sun_path, socket_path);

  res = connect(s, (struct sockaddr *)&addr, sizeof(addr));

  return res;
}

static void try_check_ownership() {
  int res;
  struct stat socketstat;

  if (!should_check_owner)
    return;

  res = stat(socket_path, &socketstat);

  if (res != 0)
    error("Cannot state the socket %s.", socket_path);

  // if (socketstat.st_uid != getuid())
  //  error("The uid %i does not own the socket %s.", getuid(), socket_path);
}

void wait_server_up(int fd) {
  char a;

  read(fd, &a, 1);
  close(fd);
}

static void server_info() {
  printf("Start tast-spooler server from root[%d]\n", client_uid);
  printf("  Socket path: %s         [TS_SOCKET]\n", socket_path);
  printf("  Read user file from %s  [TS_USER_PATH]\n", get_user_path());
  printf("  Write log file to %s    [TS_LOGFILE_PATH]\n\n",
         set_server_logfile());
}

static void server_daemon() {
  server_info();
  server_main(0, socket_path);
  exit(0);
}

/* Returns the fd where to wait for the parent notification */
static int fork_server() {
  int pid;
  int p[2];

  /* !!! stdin/stdout */
  pipe(p);

  pid = fork();
  switch (pid) {
  case 0: /* Child */
    close(p[0]);
    close(server_socket);
    /* Close all std handles for the server */
    close(0);
    close(1);
    close(2);
    setsid();
    server_main(p[1], socket_path);
    exit(0);
    break;
  case -1: /* Error */
    return -1;
  default: /* Parent */
    server_info();
    close(p[1]);
  }
  /* Return the read fd */
  return p[0];
}

void notify_parent(int fd) {
  char a = 'a';
  write(fd, &a, 1);
  close(fd);
}

int ensure_server_up(int daemonFlag) {
  int res;
  int notify_fd;
  server_socket = socket(AF_UNIX, SOCK_STREAM, 0);
  if (server_socket == -1)
    error("getting the server socket");

  create_socket_path(&socket_path);

  res = try_connect(server_socket);

  /* Good connection */
  if (res == 0) {
    try_check_ownership();
    free(socket_path);
    return 1;
  }

  /* If error other than "No one listens on the other end"... */
  if (!(errno == ENOENT || errno == ECONNREFUSED))
    error("c: cannot connect to the server");

  if (errno == ECONNREFUSED)
    unlink(socket_path);

  /* Try starting the server */
  if (client_uid == root_UID) {
    if (daemonFlag) {
      printf("Start task-spooler server as daemon\n");
      server_daemon();
    } else {
      printf("start task-spooler server\n");
      notify_fd = fork_server();
    }
  } else {
    printf("only task-spooler server could be run as Root!\n");
  }

  wait_server_up(notify_fd);
  res = try_connect(server_socket);

  /* The second time didn't work. Abort. */
  if (res == -1) {
    fprintf(stderr, "The server didn't come up.\n");
    exit(-1);
  }
  free(socket_path);

  /* Good connection on the second time */
  return 1;
}

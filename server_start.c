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
#include <dirent.h>

#include "main.h"
#include "server_start.h"
#include "server.h"
#include "error.h"
#include "user.h"
#include "runtime_limit.h"
#include "sqlite.h"
#include "version.h"
#ifdef TS_CPU_BIND
#include "cpu_bind.h"
#endif

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

void c_check_daemon() {
  int res;
  server_socket = socket(AF_UNIX, SOCK_STREAM, 0);
  if (server_socket == -1)
    error("getting the server socket");

  create_socket_path(&socket_path);

  res = try_connect(server_socket);

  /* Good connection */
  if (res == 0) {
    printf("Connected to the task-spooler server.\n");
    exit(EXIT_SUCCESS);
  } else {
    error("Error: Failed to connect to the task-spooler server.\n");  }
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
  printf("Task Spooler v" TS_MAKE_STR(TS_VERSION) " — starting from root[%d]\n", root_UID);
#ifdef TS_CPU_BIND
  printf("  CPU binding: %s\n", cpu_bind_enabled() ? "ON" : "OFF");
#endif
  printf("  Socket path: %s         [TS_SOCKET]\n", socket_path);
  printf("  Read user file from %s  [TS_USER_PATH]\n", get_user_path());
  printf("  Write log file to %s    [TS_LOGFILE_PATH]\n", set_server_logfile());
  printf("  Sqlite Database @ %s    [TS_SQLITE_PATH]\n", get_sqlite_path());
  time_repr_t r = format_time(get_max_wall_time());
  printf("  Max_Wall_Time: %.2f %c  [TS_MAX_WALL_TIME]\n", r.value, r.unit);
}

static void server_daemon() {
  server_info();
  server_main(0, socket_path);
  exit(EXIT_SUCCESS);
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
    server_info();
    server_main(p[1], socket_path);
    exit(EXIT_SUCCESS);
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

/* Check no other instance of this binary is running as root.
   Returns 0 if single, -1 if another instance found. */
static int ensure_single_instance(void) {
  char my_exe[512];
  ssize_t len;

  len = readlink("/proc/self/exe", my_exe, sizeof(my_exe) - 1);
  if (len <= 0) return 0; /* can't check, allow */
  my_exe[len] = '\0';

  DIR *dir = opendir("/proc");
  if (dir == NULL) return 0; /* can't check, allow */

  struct dirent *ent;
  while ((ent = readdir(dir)) != NULL) {
    if (ent->d_name[0] < '0' || ent->d_name[0] > '9') continue;
    pid_t pid = (pid_t)atoi(ent->d_name);
    if (pid == getpid()) continue;

    char path_buf[512], link_buf[512];
    snprintf(path_buf, sizeof(path_buf), "/proc/%d/exe", pid);
    len = readlink(path_buf, link_buf, sizeof(link_buf) - 1);
    if (len <= 0) continue;
    link_buf[len] = '\0';

    if (strcmp(my_exe, link_buf) == 0) {
      /* Check it's actually running as root */
      char stat_path[256];
      snprintf(stat_path, sizeof(stat_path), "/proc/%d/status", pid);
      FILE *fp = fopen(stat_path, "r");
      if (fp) {
        char line[256];
        int is_root = 0;
        while (fgets(line, sizeof(line), fp)) {
          if (strncmp(line, "Uid:", 4) == 0) {
            int real_uid;
            sscanf(line, "Uid:\t%d", &real_uid);
            if (real_uid == 0) is_root = 1;
            break;
          }
        }
        fclose(fp);
        if (is_root) {
          printf("Error: another instance of task-spooler server is already running (PID %d)\n", pid);
          closedir(dir);
          return -1;
        }
      }
    }
  }
  closedir(dir);
  return 0;
}

int ensure_server_up(int daemonFlag) {
  int res;
  int notify_fd = -1;
  server_socket = socket(AF_UNIX, SOCK_STREAM, 0);
  if (server_socket == -1)
    error("getting the server socket");

  create_socket_path(&socket_path);
  if (daemonFlag == 1)
    remove(socket_path); // try to delete it
  res = try_connect(server_socket);

  /* Good connection */
  if (res == 0) {
    try_check_ownership();
    free(socket_path);
    return 1;
  }

  /* If error other than "No one listens on the other end"... */
  if (!(errno == ENOENT || errno == ECONNREFUSED))
    error("Error: cannot connect to the server");

  if (errno == ECONNREFUSED)
    unlink(socket_path);

  int optval = 1;
  if (setsockopt(server_socket, SOL_SOCKET, SO_PASSCRED, &optval,
                 sizeof(optval)) == -1)
    error("Error: cannot setup SO_PASSCRED");

  /* Try starting the server */
  if (getuid() == root_UID) {
    if (daemonFlag) {
      if (ensure_single_instance() != 0) {
        error("Error: another task-spooler server instance is already running.");
      }
      printf("Start task-spooler server as daemon\n");
      server_daemon();
    } else {
      printf("start task-spooler server\n");
      notify_fd = fork_server();
    }
  } else {
    printf("Running the Task-Spooler server as the ROOT user is the only "
           "allowed option.\n");
  }

  if (notify_fd != -1) {
    wait_server_up(notify_fd);
  }
  res = try_connect(server_socket);

  
  free(socket_path);
  /* The second time didn't work. Abort. */
  if (res == -1) {
    error("Error: Failed to start the server.\n");
  }
  /* Good connection on the second time */
  return 1;
}

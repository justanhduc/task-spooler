/*
    Task Spooler PLUS - a multi-user job scheduler like slurm.
    Copyright (C) 2007-2026  Kylin JIANG - Lluís Batlle i Rossell

    Please find the license in the provided COPYING file.
*/
#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <sys/times.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/inotify.h>

#include <time.h>
#include <unistd.h>

#include <fcntl.h>

#include "main.h"
#include "execute.h"
#include "signals.h"
#include "error.h"
#include "client.h"
#include "runtime_limit.h"



int cgroups_freeze_ok(int jobid, pid_t pid);

/*
static int wait_for_pid(int pid)
{
    char path[32];
    int in_fd = inotify_init();
    sprintf(path, "/proc/%i/exe", pid);
    if (inotify_add_watch(in_fd, path, IN_CLOSE_NOWRITE) < 0) {
        close(in_fd);
        return -1;
    }

    sprintf(path, "/proc/%i", pid);
    int dir_fd = open(path, 0);
    if (dir_fd < 0) {
        close(in_fd);
        return -1;
    }

    int res = 0;
    while (1) {
        struct inotify_event event;
        if (read(in_fd, &event, sizeof(event)) < 0) {
            res = -1;
            break;
        }
        int f = openat(dir_fd, "fd", 0);
        if (f < 0) break;
        close(f);
    }

    close(dir_fd);
    close(in_fd);
    return res;
}
*/
/* Shell-quote a string: wrap in single quotes, escape internal ' as '\'' */
static const char *quote_str(const char *s, char **buf, size_t *len) {
    if (!s) return "";
    size_t need = strlen(s) * 2 + 3;
    if (need > *len) {
        free(*buf);
        *buf = malloc(need);
        *len = need;
    }
    if (!*buf) return "";
    char *q = *buf;
    *q++ = '\'';
    for (const char *p = s; *p; p++) {
        if (*p == '\'') {
            memcpy(q, "'\\''", 4);
            q += 4;
        } else {
            *q++ = *p;
        }
    }
    *q++ = '\'';
    *q = '\0';
    return *buf;
}

char *expand_template(const char *tmpl, int jobid, const char *output,
                      int exitid, pid_t pid, const char *label,
                      const char *command,
                      time_t real_sec, time_t user_sec, time_t sys_sec,
                      time_t pause_duration,
                      time_t start_time, time_t enqueue_time, time_t end_time,
                      int num_slots) {
    if (!tmpl) return NULL;
    size_t len = strlen(tmpl) * 2 + 256;
    char *out = malloc(len);
    if (!out) return NULL;

    const char *p = tmpl;
    char *q = out;
    while (*p) {
        if (*p == '{') {
            const char *end = strchr(p + 1, '}');
            if (end) {
                size_t klen = end - (p + 1);
                char key[32];
                if (klen < sizeof(key)) {
                    strncpy(key, p + 1, klen);
                    key[klen] = '\0';

                    char buf[64];
                    const char *val = "";
                    if (strcmp(key, "jobid") == 0)
                        snprintf(buf, sizeof(buf), "%d", jobid), val = buf;
                    else if (strcmp(key, "output") == 0) {
                        static char *qout = NULL;
                        static size_t qout_len = 0;
                        val = quote_str(output, &qout, &qout_len);
                    }
                    else if (strcmp(key, "exitcode") == 0)
                        snprintf(buf, sizeof(buf), "%d", exitid), val = buf;
                    else if (strcmp(key, "pid") == 0)
                        snprintf(buf, sizeof(buf), "%d", pid), val = buf;
                    else if (strcmp(key, "label") == 0) {
                        static char *qlbl = NULL;
                        static size_t qlbl_len = 0;
                        val = quote_str(label, &qlbl, &qlbl_len);
                    }
                    else if (strcmp(key, "command") == 0) {
                        static char *qcmd = NULL;
                        static size_t qcmd_len = 0;
                        val = quote_str(command, &qcmd, &qcmd_len);
                    } else if (strcmp(key, "realtime") == 0)
                        snprintf(buf, sizeof(buf), "%ld", (long)real_sec), val = buf;
                    else if (strcmp(key, "usertime") == 0)
                        snprintf(buf, sizeof(buf), "%ld", (long)user_sec), val = buf;
                    else if (strcmp(key, "systime") == 0)
                        snprintf(buf, sizeof(buf), "%ld", (long)sys_sec), val = buf;
                    else if (strcmp(key, "pausetime") == 0)
                        snprintf(buf, sizeof(buf), "%ld", (long)pause_duration), val = buf;
                    else if (strcmp(key, "start_time") == 0)
                        snprintf(buf, sizeof(buf), "%ld", (long)start_time), val = buf;
                    else if (strcmp(key, "enque_time") == 0)
                        snprintf(buf, sizeof(buf), "%ld", (long)enqueue_time), val = buf;
                    else if (strcmp(key, "end_time") == 0)
                        snprintf(buf, sizeof(buf), "%ld", (long)end_time), val = buf;
                    else if (strcmp(key, "slots") == 0)
                        snprintf(buf, sizeof(buf), "%d", num_slots), val = buf;

                    size_t vlen = strlen(val);
                    memcpy(q, val, vlen);
                    q += vlen;
                    p = end + 1;
                    continue;
                }
            }
        }
        *q++ = *p++;
    }
    *q = '\0';
    return out;
}

void run_on_finish(const char *tmpl, int jobid, const char *output,
                   int exitid, pid_t pid, const char *label,
                   const char *command,
                   time_t real_sec, time_t user_sec, time_t sys_sec,
                   time_t pause_duration,
                   time_t start_time, time_t enqueue_time, time_t end_time,
                   int num_slots) {
    char *expanded = expand_template(tmpl, jobid, output, exitid, pid, label, command,
                                     real_sec, user_sec, sys_sec, pause_duration,
                                     start_time, enqueue_time, end_time, num_slots);
    if (!expanded) return;

    int ret = system(expanded);
    if (ret == -1)
        error("system on finish");
    free(expanded);
}

/* run_relink and c_run_job_fork removed in auto-reconnect branch */

/* Returns errorlevel */
static void run_parent(int fd_read_filename, int pid, struct Result *result) {
  int status = 0;
  char *ofname = 0;
  int namesize;
  int res;
  time_t starttv; //, endtv;
  struct tms cpu_times;
  /* Read the filename */
  /* This is linked with the write() in this same file, in run_child() */
  if (command_line.store_output) {
    res = read(fd_read_filename, &namesize, sizeof(namesize));
    if (res == -1)
      error("read the filename from %i", fd_read_filename);
    if (res != sizeof(namesize))
      error("Reading the size of the name");
    ofname = (char *)malloc(namesize);
    res = read(fd_read_filename, ofname, namesize);
    if (res != namesize)
      error("Reading the out file name");
  }
  res = read(fd_read_filename, &starttv, sizeof(starttv));
  if (res != sizeof(starttv))
    error("Reading the the struct time_t");
  close(fd_read_filename);
  
  /* All went fine - prepare the SIGINT and send runjob_ok */
  signals_child_pid = pid;
  unblock_sigint_and_install_handler();
  // printf("runjob_ok %s\n", ofname);
  c_send_runjob_ok(ofname, pid);

  waitpid(pid, &status, 0);

  /* Set the errorlevel */
  if (WIFEXITED(status)) {
    /* We force the proper cast */
    result->errorlevel = WEXITSTATUS(status);
    result->died_by_signal = 0;
  } else if (WIFSIGNALED(status)) {
    result->signal = WTERMSIG(status);
    result->errorlevel = -1;
    result->died_by_signal = 1;
  } else {
    result->died_by_signal = 0;
    result->errorlevel = -1;
  }

  command_line.rt_pid = pid;
  if (command_line.on_finish_cmd && ofname)
      command_line.rt_output = strdup(ofname);

  /* Calculate times */
  times(&cpu_times);
  result->real_sec   = get_monotonic_sec() - starttv;
  result->user_sec   = (time_t)cpu_times.tms_cutime / sysconf(_SC_CLK_TCK);
  result->system_sec = (time_t)cpu_times.tms_cstime / sysconf(_SC_CLK_TCK);

  free(ofname);
}

void create_closed_read_on(int dest) {
  int p[2];
  /* Closing input */
  pipe(p);
  close(p[1]);      /* closing the write handle */
  dup2(p[0], dest); /* the pipe reading goes to dest */
  if (p[0] != dest)
    close(p[0]);
}

/* This will close fd_out and fd_in in the parent */
static void run_gzip(int fd_out, int fd_in) {
  int pid;
  pid = fork();

  switch (pid) {
  case 0: /* child */
    restore_sigmask();
    dup2(fd_in, 0);  /* stdout */
    dup2(fd_out, 1); /* stdout */
    close(fd_in);
    close(fd_out);
    /* Without stderr */
    close(2);
    execlp("gzip", "gzip", NULL);
    exit(EXIT_FAILURE);
    /* Won't return */
  case -1:
    exit(EXIT_FAILURE); /* Fork error */
  default:
    close(fd_in);
    close(fd_out);
  }
}

static void run_child(int fd_send_filename, const char *tmpdir, int jobid) {
  char *outfname;
  char errfname[4100]; /* .e */
  char jobid_str[100];

  int namesize;
  int outfd;
  int err;
  char *label = "ts_out";
  if (command_line.logfile) {
    label = command_line.logfile;
  } else if (command_line.label) {
    label = command_line.label;
  }
  snprintf(jobid_str, 100, "%d", jobid);

  // int len_outfname = 1 + strlen(label) + strlen(".XXXXXX") + 1;
  int len_outfname = 3 + strlen(label) + strlen(jobid_str);
  if (command_line.outfile == NULL) {
    outfname = malloc(len_outfname);
    snprintf(outfname, len_outfname, "/%s.%d", label, jobid);
  } else {
    outfname = command_line.outfile;
    tmpdir = "";
  }

  if (command_line.store_output) {
    /* Prepare path */
    int lname;
    char *outfname_full;

    // if (tmpdir == NULL)
    //  tmpdir = "/tmp";
    lname = strlen(outfname) + strlen(tmpdir) + 5 /* \0 */;

    outfname_full = (char *)malloc(lname);
    strcpy(outfname_full, tmpdir);
    strcat(outfname_full, outfname);

    if (command_line.gzip) {
      int p[2];
      /* We assume that all handles are closed*/
      err = pipe(p);
      assert(err == 0);

      /* gzip output goes to the filename */
      /* This will be the handle other than 0,1,2 */
      /* mkstemp doesn't admit adding ".gz" to the pattern */
      // outfd = mkstemp(outfname_full); /* stdout */
      outfd = open(outfname_full, O_CREAT | O_WRONLY | O_TRUNC, 0644);
      assert(outfd != -1);

      /* Program stdout and stderr */
      /* which go to pipe write handle */
      err = dup2(p[1], 1);
      assert(err != -1);
      if (command_line.stderr_apart) {
        int errfd;
        strncpy(errfname, outfname_full, sizeof errfname);
        strncat(errfname, ".e", 2 + 1);
        errfd = open(errfname, O_CREAT | O_WRONLY | O_TRUNC, 0644);
        assert(errfd != -1);
        err = dup2(errfd, 2);
        assert(err != -1);
        err = close(errfd);
        assert(err == 0);
      } else {
        err = dup2(p[1], 2);
        assert(err != -1);
      }
      err = close(p[1]);
      assert(err == 0);

      /* run gzip.
       * This wants p[0] in 0, so gzip will read
       * from it */
      run_gzip(outfd, p[0]);
    } else {
      /* Prepare the filename */
      // outfd = mkstemp(outfname_full); /* stdout */
      outfd = open(outfname_full, O_CREAT | O_WRONLY | O_TRUNC, 0644);
      dup2(outfd, 1); /* stdout */
      if (command_line.stderr_apart) {
        int errfd;
        strncpy(errfname, outfname_full, sizeof errfname);
        strncat(errfname, ".e", 2 + 1);
        errfd = open(errfname, O_CREAT | O_WRONLY | O_TRUNC, 0644);
        dup2(errfd, 2);
        close(errfd);
      } else
        dup2(outfd, 2);
      close(outfd);
    }

    /* Send the filename */
    namesize = strlen(outfname_full) + 1;
    write(fd_send_filename, (char *)&namesize, sizeof(namesize));
    write(fd_send_filename, outfname_full, namesize);
    free(outfname_full);
  }
  /* Times */
  time_t starttv = get_monotonic_sec();
  write(fd_send_filename, &starttv, sizeof(starttv));
  close(fd_send_filename);

  /* Closing input */
  if (command_line.should_go_background)
    create_closed_read_on(0);

  /* We create a new session, so we can kill process groups as:
       kill -- -`ts -p` */
  setsid();

  pid_t my_pid = getpid();
  while(cgroups_freeze_ok(jobid, my_pid) != 1) {
    usleep(30000);
  }

  int max_retries = command_line.n_retry;
  for (int attempt = 0; attempt <= max_retries; attempt++) {
    time_t t_start = get_monotonic_sec();
    pid_t child = fork();
    if (child == 0) {
      execvp(command_line.command.array[0], command_line.command.array);
      _exit(127);
    }
    if (child == -1) {
      _exit(EXIT_FAILURE);
    }

    int status;
    waitpid(child, &status, 0);
    time_t elapsed = get_monotonic_sec() - t_start;

    if (WIFEXITED(status)) {
      int code = WEXITSTATUS(status);
      if (code != 0 && code != 126 && code != 127
          && elapsed <= 15 && attempt < max_retries)
        continue;
      _exit(code);
    }
    if (WIFSIGNALED(status)) {
      int sig = WTERMSIG(status);
      signal(sig, SIG_DFL);
      raise(sig);
      _exit(128 + sig);
    }
    _exit(EXIT_FAILURE);
  }
  _exit(EXIT_FAILURE); /* unreachable */
}

int run_job(int jobid, struct Result *res) {
  pid_t pid;
  int errorlevel = 0;
  int p[2];
  char path[2048];
  getcwd(path, 2048);
  // const char *tmpdir = get_logdir();

  /* For the parent */
  /*program_signal(); Still not needed*/

  block_sigint();
  /* Prepare the output filename sending */
  pipe(p);
  
  
  pid = fork();

  switch (pid) {
  case 0:
    restore_sigmask();
    close(server_socket);
    close(p[0]);
    run_child(p[1], path, jobid);
    /* Not reachable, if the 'exec' of the command
     * works. Thus, command exists, etc. */
    fprintf(stderr, "ts could not run the command\n");
    // free((char *)tmpdir);
    exit(EXIT_FAILURE);
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
  // free((char *)tmpdir);
  return errorlevel;
}



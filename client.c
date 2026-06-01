/*
    Task Spooler - a task queue system for the unix user
    Copyright (C) 2007-2009  Lluís Batlle i Rossell

    Please find the license in the provided COPYING file.
*/
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "main.h"
#include "client.h"
#include "server_start.h"
#include "error.h"
#include "utils.h"
#include "env.h"
#include "execute.h"
#include "tail.h"

static void c_end_of_job(const struct Result *res);

static void c_wait_job_send();

static void c_wait_running_job_send();

char *build_command_string() {
  return charArray_string(command_line.command.num, command_line.command.array);
}

void c_new_job() {
  // printf("new _job \n");
  struct Msg m = default_msg();
  char *new_command, path[2048];
  char *myenv;

  m.type = NEWJOB;

  getcwd(path, 2048);
  new_command = command_line.linux_cmd; // build_command_string();
  char* old_command = build_command_string();

  myenv = get_environment();

  /* global */
  m.jobid = command_line.jobid;
  m.u.newjob.command_size = strlen(new_command) + 1; /* add null */
  m.u.newjob.command_size_strip = strlen(new_command) - strlen(old_command);
  m.u.newjob.path_size = strlen(path) + 1; /* add null */
  if (myenv)
    m.u.newjob.env_size = strlen(myenv) + 1; /* add null */
  else
    m.u.newjob.env_size = 0;
  
  if (command_line.label)
    m.u.newjob.label_size = strlen(command_line.label) + 1; /* add null */
  else
    m.u.newjob.label_size = 0;

  if (command_line.email) {
    // printf("send email to %s\n", command_line.email);
    m.u.newjob.email_size = strlen(command_line.email) + 1; /* add null */
  } else
    m.u.newjob.email_size = 0;


  m.u.newjob.store_output = command_line.store_output;
  m.u.newjob.depend_on_size = command_line.depend_on_size;
  m.u.newjob.should_keep_finished = command_line.should_keep_finished;
  m.u.newjob.command_size = strlen(new_command) + 1; /* add null */
  m.u.newjob.wait_enqueuing = command_line.wait_enqueuing;
  m.u.newjob.num_slots = command_line.num_slots;
  m.u.newjob.taskpid = command_line.taskpid;
  m.u.newjob.start_time = command_line.start_time;
  m.u.newjob.wall_time = command_line.wall_time;
  m.u.newjob.schedule_time = command_line.schedule_time;
  
  /* Send the message */
  send_msg(server_socket, &m);

  /* send dependencies */
  if (command_line.depend_on_size)
    send_ints(server_socket, command_line.depend_on,
              command_line.depend_on_size);

  /* Send the command */
  send_bytes(server_socket, new_command, m.u.newjob.command_size);

  /* Send the work dir */
  send_bytes(server_socket, path, m.u.newjob.path_size);

  /* Send the label */
  send_bytes(server_socket, command_line.label, m.u.newjob.label_size);

  /* Send the label */
  send_bytes(server_socket, command_line.email, m.u.newjob.email_size);

  /* Send the environment */
  send_bytes(server_socket, myenv, m.u.newjob.env_size);

  // free(new_command);
  free(myenv);
  free(old_command);
}

static void c_print_line(struct Msg* m) {
  char *buffer;
  buffer = (char *)malloc(m->u.size);
  recv_bytes(server_socket, buffer, m->u.size);
  printf("%s", buffer);
  free(buffer);
}

int c_wait_newjob_ok() {
  struct Msg m = default_msg();
  int res;

  res = recv_msg(server_socket, &m);
  if (res == -1)
    error("Error in wait_newjob_ok");
  if (m.type == NEWJOB_NOK) {
    fprintf(stderr, "Error: maybe conflict jobid\n");
    exit(EXITCODE_QUEUE_FULL);
  }

  if (m.type == LIST_LINE) {
    c_print_line(&m);
    return c_wait_newjob_ok();
  }

  if (m.type == NEWJOB_PID_NOK) {
    fprintf(stderr, "Error: cannot create job\n");
    exit(1);
  }
  if (m.type != NEWJOB_OK)
    error("Error: the return type is not NEWJOB_OK");
  
  return m.jobid;
}

/* Try to reconnect to server. Returns 1 on success, 0 on failure. */
static int reconnect_to_server(void) {
    if (server_socket > 0) {
        close(server_socket);
        server_socket = -1;
    }
    for (int attempt = 0;; attempt++) {
        if (attempt > 0) sleep(60);

        int new_fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (new_fd < 0) continue;

        char *sp = NULL;
        create_socket_path(&sp);
        struct sockaddr_un addr;
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, sp, sizeof(addr.sun_path) - 1);
        free(sp);

        if (connect(new_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
            close(new_fd);
            continue;
        }
        server_socket = new_fd;
        return 1;
    }
}

int c_wait_server_commands() {
  struct Msg m = default_msg();
  int res;

  while (1) {
    res = recv_msg(server_socket, &m);
    if (res == -1) {
      /* Socket error — reconnect and keep waiting */
      reconnect_to_server();
      continue;
    }

    if (res == 0) {
      /* EOF — server went down, reconnect and keep waiting */
      reconnect_to_server();
      continue;
    }
    if (res != sizeof(m))
      error("Error in wait_server_commands");
    if (m.type == RUNJOB) {
      struct Result result = default_result();
      result.skipped = 0;
      /* These will send RUNJOB_OK */
      if (command_line.depend_on_size && command_line.require_elevel &&
          m.u.last_errorlevel != 0) {
        result.errorlevel = -1;
        result.user_sec = 0;
        result.system_sec = 0;
        result.real_sec = 0;
        result.skipped = 1;
        c_send_runjob_ok(0, -1);
      } else {
        run_job(m.jobid, &result);
      }
      c_end_of_job(&result);

      /* Try to receive ENDJOB_OK — if server crashed, reconnect */
      res = recv_msg(server_socket, &m);
      if (res != sizeof(m) || m.type != ENDJOB_OK) {
          while (1) {
              reconnect_to_server();

              struct Msg r = default_msg();
              r.type = RECONNECT;
              r.jobid = command_line.jobid;
              r.u.reconnect.pid = command_line.rt_pid;
              send_msg(server_socket, &r);

              res = recv_msg(server_socket, &m);
              if (res != sizeof(m) || m.type != RECONNECT_OK) continue;

              c_end_of_job(&result);
              res = recv_msg(server_socket, &m);
              if (res == sizeof(m) && m.type == ENDJOB_OK) break;
          }
      }

      /* Store timing info from ENDJOB_OK */
      if (res == sizeof(m)) {
          command_line.rt_real_sec = m.u.finish_info.real_sec;
          command_line.rt_user_sec = m.u.finish_info.user_sec;
          command_line.rt_system_sec = m.u.finish_info.system_sec;
          command_line.rt_pause_duration = m.u.finish_info.pause_duration;
          command_line.rt_start_time = m.u.finish_info.start_time;
          command_line.rt_enqueue_time = m.u.finish_info.enqueue_time;
          command_line.rt_end_time = m.u.finish_info.end_time;
          command_line.rt_num_slots = m.u.finish_info.num_slots;
      }

      /* Run per-job on-finish callback (user context) */
      /* Run per-job on-finish callback (as user, not root) */
      if (command_line.on_finish_cmd) {
          run_on_finish(command_line.on_finish_cmd, command_line.jobid,
                        command_line.rt_output, result.errorlevel,
                        command_line.rt_pid,
                        command_line.label, command_line.linux_cmd,
                        command_line.rt_real_sec, command_line.rt_user_sec,
                        command_line.rt_system_sec,
                        command_line.rt_pause_duration,
                        command_line.rt_start_time,
                        command_line.rt_enqueue_time,
                        command_line.rt_end_time,
                        command_line.rt_num_slots);
      }
      if (command_line.rt_output) {
          free(command_line.rt_output);
          command_line.rt_output = NULL;
      }
      return result.errorlevel;
    }
  }
  return -1;
}

// check weather the return is line message with a specific header [pre_str].
static int wait_server_lines_and_check(const char *pre_str) {
  struct Msg m = default_msg();
  int res;
  int error_sig = 0;
  while (1) {
    res = recv_msg(server_socket, &m);
    if (res == -1)
      error("Error in wait_server_lines");

    if (res == 0)
      break;
    if (res != sizeof(m))
      // printf("Error in wait_server_lines 2");
      // break;
      error("Error in wait_server_lines 2");
    if (m.type == LIST_LINE) {
      char *buffer;
      buffer = (char *)malloc(m.u.size);
      recv_bytes(server_socket, buffer, m.u.size);
      if (strncmp(buffer, pre_str, strlen(pre_str)) == 0) {
        error_sig++;
      }
      printf("%s", buffer);
      free(buffer);
    }
  }
  return error_sig;
}

void c_wait_server_lines() { wait_server_lines_and_check(""); }

void c_list_jobs() {
  struct Msg m = default_msg();

  m.type = LIST;
  m.u.list.list_format = command_line.list_format;
  m.u.list.term_width = term_width;
  // m.u.term_width = term_width;
  send_msg(server_socket, &m);
}

void c_list_jobs_all() {
  struct Msg m = default_msg();

  m.type = LIST_ALL;
  m.u.list.list_format = command_line.list_format;
  m.u.list.term_width = term_width;
  // m.u.list.plain_list = command_line.plain_list;
  send_msg(server_socket, &m);
}

/* Exits if wrong */
void c_check_version() {
  struct Msg m = default_msg();
  int res;

  m.type = GET_VERSION;
  /* Double send, so an old ts will answer for sure at least once */
  send_msg(server_socket, &m);
  send_msg(server_socket, &m);

  /* Set up a 2 second timeout to receive the
  version msg. */

  res = recv_msg(server_socket, &m);
  if (res == -1)
    error("Error calling recv_msg in c_check_version");
  if (m.type != VERSION || m.u.version != PROTOCOL_VERSION) {
    error("Wrong server version. Received %i, expecting %i", m.u.version,
          PROTOCOL_VERSION);
  }

  /* Receive also the 2nd send_msg if we got the right version */
  res = recv_msg(server_socket, &m);
  if (res == -1)
    error("Error calling the 2nd recv_msg in c_check_version");
}

static void print_job_resource_usage(pid_t pid) {
    if (pid <= 0) return;
    if (kill(pid, 0) != 0) return;

    char path[64];
    FILE *fp;
    char buf[1024];
    long ticks = sysconf(_SC_CLK_TCK);
    if (ticks <= 0) return;

    /* --- CPU usage: two samples ~50ms apart --- */
    snprintf(path, sizeof(path), "/proc/%d/stat", pid);
    fp = fopen(path, "r");
    if (fp) {
        long utime1 = 0, stime1 = 0, utime2 = 0, stime2 = 0;
        if (fgets(buf, sizeof(buf), fp)) {
            char *ptr = buf;
            for (int i = 0; i < 13; i++) {
                ptr = strchr(ptr, ' ');
                if (!ptr) break;
                ptr++;
            }
            if (ptr && sscanf(ptr, "%ld %ld", &utime1, &stime1) == 2) {
                struct timespec ts1, ts2;
                clock_gettime(CLOCK_MONOTONIC, &ts1);

                struct timespec sleep_ts = { .tv_sec = 0, .tv_nsec = 50000000L };
                nanosleep(&sleep_ts, NULL);

                fclose(fp);
                fp = fopen(path, "r");
                if (fp && fgets(buf, sizeof(buf), fp)) {
                    ptr = buf;
                    for (int i = 0; i < 13; i++) {
                        ptr = strchr(ptr, ' ');
                        if (!ptr) break;
                        ptr++;
                    }
                    if (ptr && sscanf(ptr, "%ld %ld", &utime2, &stime2) == 2) {
                        clock_gettime(CLOCK_MONOTONIC, &ts2);
                        double wall_sec = (ts2.tv_sec - ts1.tv_sec)
                                        + (ts2.tv_nsec - ts1.tv_nsec) / 1e9;
                        if (wall_sec > 0) {
                            long delta = (utime2 + stime2) - (utime1 + stime1);
                            double cpu_pct = ((double)delta / ticks) / wall_sec * 100.0;
                            printf("CPU usage: %.1f%%\n", cpu_pct);
                        }
                    }
                }
            }
        }
        if (fp) fclose(fp);
    }

    /* --- Memory: RSS & VIRT from /proc/<pid>/status --- */
    snprintf(path, sizeof(path), "/proc/%d/status", pid);
    fp = fopen(path, "r");
    if (fp) {
        long rss = -1, vsz = -1;
        while (fgets(buf, sizeof(buf), fp)) {
            if (rss < 0 && strncmp(buf, "VmRSS:", 6) == 0)
                sscanf(buf + 6, "%ld", &rss);
            if (vsz < 0 && strncmp(buf, "VmSize:", 7) == 0)
                sscanf(buf + 7, "%ld", &vsz);
        }
        fclose(fp);

        if (rss >= 0 && vsz >= 0)
            printf("Memory: %ld KB RSS / %ld KB VIRT\n", rss, vsz);
        else if (rss >= 0)
            printf("Memory: %ld KB RSS\n", rss);
    }
}

void c_show_info() {
  struct Msg m = default_msg();
  int res;

  m.type = INFO;
  m.jobid = command_line.jobid;

  send_msg(server_socket, &m);

  while (1) {
    res = recv_msg(server_socket, &m);
    if (res == -1)
      error("Error in wait_server_lines");

    if (res == 0)
      break;

    if (res != sizeof(m))
      error("Error in wait_server_lines 2");
    if (m.type == LIST_LINE) {
      char *buffer;
      buffer = (char *)malloc(m.u.size);
      recv_bytes(server_socket, buffer, m.u.size);
      printf("%s", buffer);
      free(buffer);
    }
    if (m.type == INFO_DATA) {
      char *buffer;
      enum { DSIZE = 1000 };
      pid_t job_pid = 0;

      /* We're going to output data using the stdout fd */
      fflush(stdout);
      buffer = (char *)malloc(DSIZE);
      do {
        res = recv(server_socket, buffer, DSIZE, 0);
        if (res > 0) {
          write(1, buffer, res);
          /* Extract PID from "PID: NNN" in the server output */
          char *pid_pos = strstr(buffer, "PID: ");
          if (pid_pos) {
            pid_pos += 5;
            char *end = pid_pos;
            while (*end >= '0' && *end <= '9') end++;
            char saved = *end;
            *end = '\0';
            job_pid = (pid_t)atoi(pid_pos);
            *end = saved;
          }
        }
      } while (res > 0);
      free(buffer);

      /* Read CPU & memory locally from /proc */
      if (job_pid > 0)
          print_job_resource_usage(job_pid);
    }
  }
}

void c_refresh_user() {
  struct Msg m = default_msg();
  m.type = REFRESH_USERS;
  send_msg(server_socket, &m);
}

int c_lock_server() {
  struct Msg m = default_msg();
  m.type = LOCK_SERVER;
  send_msg(server_socket, &m);
  int error_sig = wait_server_lines_and_check("Error:");
  return error_sig;
}

int c_unlock_server() {
  struct Msg m = default_msg();
  m.type = UNLOCK_SERVER;
  send_msg(server_socket, &m);
  int error_sig = wait_server_lines_and_check("Error:");
  return error_sig;
}

void c_suspend_user(int uid) {
  struct Msg m = default_msg();
  // int res;
  m.type = SUSPEND_USER;
  m.jobid = uid;
  send_msg(server_socket, &m);
}

void c_resume_user(int uid) {
  struct Msg m = default_msg();
  // int res;
  m.type = RESUME_USER;
  m.jobid = uid;
  send_msg(server_socket, &m);
}

void c_show_last_id() {
  struct Msg m = default_msg();
  int res;

  m.type = LAST_ID;
  send_msg(server_socket, &m);

  /* Receive the answer */
  res = recv_msg(server_socket, &m);
  if (res != sizeof(m))
    error("Error in get_output_file");

  switch (m.type) {
  case LAST_ID:
    printf("%d\n", m.jobid);
  default:
    warning("Wrong internal message in get_output_file line size");
  }
}

void c_send_runjob_ok(const char *ofname, pid_t pid) {
  struct Msg m = default_msg();

  /* Prepare the message */
  m.type = RUNJOB_OK;
  if (ofname) /* ofname == 0, skipped execution */
    m.u.output.store_output = command_line.store_output;
  else
    m.u.output.store_output = 0;
  m.u.output.pid = pid;
  if (m.u.output.store_output)
    m.u.output.ofilename_size = strlen(ofname) + 1;
  else
    m.u.output.ofilename_size = 0;

  send_msg(server_socket, &m);

  /* Send the filename */
  if (m.u.output.store_output)
    send_bytes(server_socket, ofname, m.u.output.ofilename_size);
}

static void c_end_of_job(const struct Result *res) {
  struct Msg m = default_msg();

  m.type = ENDJOB;
  m.u.result = *res; /* struct copy */

  send_msg(server_socket, &m);
}

void c_shutdown_server() {

  struct Msg m = default_msg();
  if (client_uid != 0) {
    printf("Only the root can shutdown the task-spooler server\n");
    return;
  }
  char buf[10];
  printf("Do you want to kill the task-spooler server? (Yes/no) ");
  scanf("%3s", buf);
  if (strcmp(buf, "Yes") != 0) {
    return;
  } else {
    printf("Kill the server!\n");
  }

  m.type = KILL_SERVER;
  send_msg(server_socket, &m);
}

void c_clear_finished() {
  struct Msg m = default_msg();
  if (client_uid == 0) {
    char buf[10];
    printf("Do you want to clear all the finished jobs on the task-spooler "
           "server? (Yes/no) ");
    scanf("%3s", buf);
    if (strcmp(buf, "Yes") != 0) {
      return;
    } else {
      printf("Clear the finished!\n");
    }
  }
  m.type = CLEAR_FINISHED;

  send_msg(server_socket, &m);
}

static char *get_output_file(pid_t *pid) {
  struct Msg m = default_msg();
  int res;
  char *string = 0;

  /* Send the request */
  m.type = ASK_OUTPUT;
  m.jobid = command_line.jobid;
  send_msg(server_socket, &m);

  /* Receive the answer */
  res = recv_msg(server_socket, &m);
  if (res != sizeof(m))
    error("Error in get_output_file");
  switch (m.type) {
  case ANSWER_OUTPUT:
    if (m.u.output.store_output) {
      /* Receive the output file name */
      string = 0;
      if (m.u.output.ofilename_size > 0) {
        string = (char *)malloc(m.u.output.ofilename_size);
        recv_bytes(server_socket, string, m.u.output.ofilename_size);
      }
      *pid = m.u.output.pid;
      return string;
    }
    *pid = m.u.output.pid;
    return 0;
    /* WILL NOT GO FURTHER */
  case LIST_LINE: /* Only ONE line accepted */
    string = (char *)malloc(m.u.size);
    res = recv_bytes(server_socket, string, m.u.size);
    if (res != m.u.size) {
      error("Error in get_output_file line size");
    }
    error("Error: in the request: %s", string);
    /* WILL NOT GO FURTHER */
  default:
    warning("Wrong internal message in get_output_file line size");
  }
  /* This will never be reached */
  return 0;
}

void c_hold_job(int jobid) {
  /* This will exit if there is any error */

  // printf("kill the pid: %d\n", pid);
  /* Send SIGTERM to the process group, as pid is for process group */
  // kill(-pid, SIGSTOP);

  struct Msg m = default_msg();
  m.type = HOLD_JOB;
  m.jobid = jobid;
  send_msg(server_socket, &m);
  c_wait_server_lines();
}

void c_cont_job(int jobid) {
  // printf("kill the pid: %d\n", pid);
  /* Send SIGTERM to the process group, as pid is for process group */
  // kill(-pid, SIGCONT);

  struct Msg m = default_msg();
  m.type = CONT_JOB;
  m.jobid = jobid;
  send_msg(server_socket, &m);
  // not error, restart job
  if (wait_server_lines_and_check("Error") == 0) {
    c_wait_server_lines();
  }

}


int c_tail() {
  char *str;
  pid_t pid;
  str = get_output_file(&pid);
  if (str == 0) {
    error("Error: the output is not stored. Cannot tail.\n");
  }

  c_wait_running_job_send();

  return tail_file(str, 10 /* Last lines to show */);
}

int c_cat() {
  char *str;
  pid_t pid;
  str = get_output_file(&pid);
  if (str == 0) {
    error("Error: the output is not stored. Cannot cat.\n");
  }
  c_wait_running_job_send();

  return tail_file(str, -1 /* All the lines */);
}

void c_show_output_file() {
  char *str;
  pid_t pid;
  /* This will exit if there is any error */
  str = get_output_file(&pid);
  if (str == 0) {
    error("Error: the output is not stored.\n");
  }
  printf("%s\n", str);
  free(str);
}

void c_show_pid() {
  pid_t pid;
  /* This will exit if there is any error */
  get_output_file(&pid);
  printf("%i\n", pid);
}

int c_find_pid(pid_t pid, int deep_search) {
  struct Msg m = default_msg();
  int res;

  m.type = FIND_PID;
  m.u.find_pid.pid = pid;
  m.u.find_pid.deep = deep_search;
  send_msg(server_socket, &m);

  res = recv_msg(server_socket, &m);
  if (res != sizeof(m)) {
    error("Error in c_find_pid");
  }

  if (m.type == FIND_PID_RESULT) {
    return m.jobid;
  }

  error("Wrong message type in c_find_pid: %i", m.type);
  return -1;
}

void c_kill_job() {
  pid_t pid = 0;
  /* This will exit if there is any error */
  get_output_file(&pid);

  if (pid == -1 || pid == 0) {
    error("Error: strange PID received: %i\n", pid);
  }

  printf("kill the pid: %d\n", pid);
  /* Send SIGTERM to the process group, as pid is for process group */
  kill(-pid, SIGTERM);
}

void c_kill_all_jobs() {
  struct Msg m = default_msg();
  int res;
  char buf[10];
  if (client_uid == 0) {
    printf("Do you want to kill all jobs in [Root]? (Yes/n) ");
  } else {
    printf("Do you want to kill all jobs by this user? (Yes/n) ");
  }

  scanf("%3s", buf);
  if (strcmp(buf, "Yes") != 0) {
    return;
  } else {
    printf("Kill all jobs!\n");
  }
  /* Send the request */
  m.type = KILL_ALL;
  send_msg(server_socket, &m);

  /* Receive the answer */
  res = recv_msg(server_socket, &m);
  if (res != sizeof(m))
    error("Error in kill_all");
  switch (m.type) {
  case COUNT_RUNNING:
    for (int i = 0; i < m.u.count_running; ++i) {
      int pid;
      res = recv(server_socket, &pid, sizeof(int), 0);
      if (res != sizeof(int))
        error("Error in receiving PID kill_all");
      kill(-pid, SIGTERM);
    }
    return;
  default:
    warning("Wrong internal message in kill_all");
  }
}

void c_remove_job() {
  struct Msg m = default_msg();
  int res;
  char *string = 0;

  /* Send the request */
  m.type = REMOVEJOB;
  m.jobid = command_line.jobid;

  send_msg(server_socket, &m);

  /* Receive the answer */
  res = recv_msg(server_socket, &m);
  if (res != sizeof(m))
    error("Error in remove_job");
  switch (m.type) {
  case REMOVEJOB_OK:
    return;
    /* WILL NOT GO FURTHER */
  case REMOVEJOB_NOK: {
    string = (char *)malloc(m.u.size);
    res = recv_bytes(server_socket, string, m.u.size);
    if (strncmp(string, "Running job", 11) == 0) {
      printf("%s", string);
      free(string);
      c_kill_job();
      return;
    }
    fprintf(stderr, "Error: %s", string);
    free(string);
    exit(EXIT_FAILURE);
  }
  default:
    warning("Wrong internal message in remove_job");
  }
  /* This will never be reached */
}

int c_wait_job_recv() {
  struct Msg m = default_msg();
  int res;
  char *string = 0;

  /* Receive the answer */
  res = recv_msg(server_socket, &m);
  if (res != sizeof(m))
    error("Error in wait_job");
  switch (m.type) {
  case WAITJOB_OK:
    return m.u.result.errorlevel;
    /* WILL NOT GO FURTHER */
  case LIST_LINE: /* Only ONE line accepted */
    string = (char *)malloc(m.u.size);
    res = recv_bytes(server_socket, string, m.u.size);
    if (res != m.u.size)
      error("Error in wait_job - line size");
    error("Error: in the request: %s", string);
    /* WILL NOT GO FURTHER */
  default:
    warning("Wrong internal message in c_wait_job");
  }
  /* This will never be reached */
  return -1;
}

static void c_wait_job_send() {
  struct Msg m = default_msg();

  /* Send the request */
  m.type = WAITJOB;
  m.jobid = command_line.jobid;
  send_msg(server_socket, &m);
}

static void c_wait_running_job_send() {
  struct Msg m = default_msg();

  /* Send the request */
  m.type = WAIT_RUNNING_JOB;
  m.jobid = command_line.jobid;
  send_msg(server_socket, &m);
}

/* Returns the errorlevel */
int c_wait_job() {
  c_wait_job_send();
  return c_wait_job_recv();
}

/* Returns the errorlevel */
int c_wait_running_job() {
  c_wait_running_job_send();
  return c_wait_job_recv();
}

void c_send_max_slots(int max_slots) {
  struct Msg m = default_msg();
  /* Send the request */
  m.type = SET_MAX_SLOTS;
  m.u.max_slots = command_line.max_slots;
  send_msg(server_socket, &m);
}

void c_get_max_slots() {
  struct Msg m = default_msg();
  int res;

  /* Send the request */
  m.type = GET_MAX_SLOTS;
  m.u.max_slots = command_line.max_slots;
  send_msg(server_socket, &m);

  /* Receive the answer */
  res = recv_msg(server_socket, &m);
  if (res != sizeof(m))
    error("Error in move_urgent");
  switch (m.type) {
  case GET_MAX_SLOTS_OK:
    printf("Max slots: %i\n", m.u.max_slots);
    return;
  default:
    warning("Wrong internal message in get_max_slots");
  }
}

void c_move_urgent() {
  struct Msg m = default_msg();
  int res;
  char *string = 0;

  /* Send the request */
  m.type = URGENT;
  m.jobid = command_line.jobid;
  send_msg(server_socket, &m);

  /* Receive the answer */
  res = recv_msg(server_socket, &m);
  if (res != sizeof(m))
    error("Error in move_urgent");
  switch (m.type) {
  case URGENT_OK:
    return;
    /* WILL NOT GO FURTHER */
  case LIST_LINE: /* Only ONE line accepted */
    string = (char *)malloc(m.u.size);
    res = recv_bytes(server_socket, string, m.u.size);
    if (res != m.u.size)
      error("Error in move_urgent - line size");
    error("Error: in the request: %s", string);
    /* WILL NOT GO FURTHER */
  default:
    warning("Wrong internal message in move_urgent");
  }
  /* This will never be reached */
  return;
}

void c_get_state() {
  struct Msg m = default_msg();
  int res;
  char *string = 0;

  /* Send the request */
  m.type = GET_STATE;
  m.jobid = command_line.jobid;
  send_msg(server_socket, &m);

  /* Receive the answer */
  res = recv_msg(server_socket, &m);
  if (res != sizeof(m))
    error("Error in get_state - line size");
  switch (m.type) {
  case ANSWER_STATE:
    printf("%s\n", jstate2string(m.u.state));
    return;
    /* WILL NOT GO FURTHER */
  case LIST_LINE: /* Only ONE line accepted */
    string = (char *)malloc(m.u.size);
    res = recv_bytes(server_socket, string, m.u.size);
    if (res != m.u.size)
      error("Error in get_state - line size");
    error("Error: in the request: %s", string);
    /* WILL NOT GO FURTHER */
  default:
    warning("Wrong internal message in get_state");
  }
  /* This will never be reached */
  return;
}

void c_swap_jobs() {
  struct Msg m = default_msg();
  int res;
  char *string = 0;

  /* Send the request */
  m.type = SWAP_JOBS;
  m.u.swap.jobid1 = command_line.jobid;
  m.u.swap.jobid2 = command_line.jobid2;
  send_msg(server_socket, &m);

  /* Receive the answer */
  res = recv_msg(server_socket, &m);
  if (res != sizeof(m))
    error("Error in swap_jobs");
  switch (m.type) {
  case SWAP_JOBS_OK:
    return;
    /* WILL NOT GO FURTHER */
  case LIST_LINE: /* Only ONE line accepted */
    string = (char *)malloc(m.u.size);
    res = recv_bytes(server_socket, string, m.u.size);
    if (res != m.u.size)
      error("Error in swap_jobs - line size");
    error("Error: in the request: %s", string);
    /* WILL NOT GO FURTHER */
  default:
    warning("Wrong internal message in swap_jobs");
  }
  /* This will never be reached */
  return;
}

void c_get_count_running() {
  struct Msg m = default_msg();
  int res;

  /* Send the request */
  m.type = COUNT_RUNNING;
  send_msg(server_socket, &m);

  /* Receive the answer */
  res = recv_msg(server_socket, &m);
  if (res != sizeof(m))
    error("Error in count_running - line size");

  switch (m.type) {
  case COUNT_RUNNING:
    printf("%i\n", m.u.count_running);
    return;
  default:
    warning("Wrong internal message in count_running");
  }

  /* This will never be reached */
  return;
}

void c_show_label() {
  struct Msg m = default_msg();
  int res;
  char *string = 0;

  /* Send the request */
  m.type = GET_LABEL;
  m.jobid = command_line.jobid;
  send_msg(server_socket, &m);

  /* Receive the answer */
  res = recv_msg(server_socket, &m);
  if (res != sizeof(m))
    error("Error in get_label");

  switch (m.type) {
  case LIST_LINE:
    string = (char *)malloc(m.u.size);
    res = recv_bytes(server_socket, string, m.u.size);
    if (res != m.u.size)
      error("Error in get_label - line size");

    printf("%s", string);
    free(string);
    return;
  default:
    warning("Wrong internal message in get_label");
  }

  /* This will never be reached */
  return;
}

void c_add_wtime() {
  struct Msg m = default_msg();
  if (client_uid != 0) {
    error("Error: Only the root can change the job's wall time!\n");
  }
  if (command_line.jobid == 0) {
    error("Error: job ID is not specified. "
          "Use --job [jobid] or -J [jobid].\n");
  }
  int res;
  char *string = 0;

  /* Send the request */
  m.type = ADD_WTIME;
  m.jobid = command_line.jobid;
  m.u.newjob.wall_time = command_line.wall_time;
  send_msg(server_socket, &m);

  /* Receive the answer */
  res = recv_msg(server_socket, &m);
  if (res != sizeof(m))
    error("Error in get_label");

  switch (m.type) {
  case LIST_LINE:
    string = (char *)malloc(m.u.size);
    res = recv_bytes(server_socket, string, m.u.size);
    if (res != m.u.size)
      error("Error in c_extend_walltime() - line size");

    printf("%s", string);
    free(string);
    return;
  default:
    warning("Wrong internal message in c_extend_walltime()");
  }

  /* This will never be reached */
  return;


}

void c_show_cmd() {
  struct Msg m = default_msg();
  int res;
  char *string = 0;

  /* Send the request */
  m.type = GET_CMD;
  m.jobid = command_line.jobid;
  send_msg(server_socket, &m);

  /* Receive the answer */
  res = recv_msg(server_socket, &m);
  if (res != sizeof(m))
    error("Error in show_cmd");

  switch (m.type) {
  case LIST_LINE:
    string = (char *)malloc(m.u.size);
    res = recv_bytes(server_socket, string, m.u.size);
    if (res != m.u.size)
      error("Error in show_cmd - line size");

    printf("%s", string);
    free(string);
    return;
  default:
    warning("Wrong internal message in show_cmd");
  }
}

char *get_logdir() {
  struct Msg m = default_msg();
  int res;
  char *string = 0;

  /* Send the request */
  m.type = GET_LOGDIR;
  send_msg(server_socket, &m);

  /* Receive the answer */
  res = recv_msg(server_socket, &m);
  if (res != sizeof(m))
    error("Error in get_logdir");

  switch (m.type) {
  case LIST_LINE:
    string = (char *)malloc(m.u.size);
    res = recv_bytes(server_socket, string, m.u.size);
    if (res != m.u.size) {
      free(string);
      error("Error in get_logdir - line size");
    }

    return string;
  default:
    warning("Wrong internal message in get_logdir");
  }
  return string;
}

void c_get_logdir() {
  char *path;
  path = get_logdir();
  printf("%s\n", path);
  free(path);
}

void c_set_logdir() {
  struct Msg m = default_msg();

  /* Send the request */
  m.type = SET_LOGDIR;
  m.u.size = strlen(command_line.label) + 1;
  send_msg(server_socket, &m);
  send_bytes(server_socket, command_line.label, m.u.size);
}

void c_get_env() {
  struct Msg m = default_msg();
  int res;
  char *string = 0;

  /* Send the request */
  m.type = GET_ENV;
  m.u.size = strlen(command_line.label) + 1;
  send_msg(server_socket, &m);
  send_bytes(server_socket, command_line.label, m.u.size);

  /* Receive the answer */
  res = recv_msg(server_socket, &m);
  if (res != sizeof(m))
    error("Error in get_env");

  switch (m.type) {
  case LIST_LINE:
    if (m.u.size) {
      string = (char *)malloc(m.u.size);
      res = recv_bytes(server_socket, string, m.u.size);
      if (res != m.u.size)
        error("Error in get_env - line size");

      printf("%s\n", string);
      free(string);
    } else
      printf("\n");

    return;
  default:
    warning("Wrong internal message in get_env");
  }
}

void c_set_env() {
  struct Msg m = default_msg();

  /* Send the request */
  m.type = SET_ENV;
  m.u.size = strlen(command_line.label) + 1;
  send_msg(server_socket, &m);
  send_bytes(server_socket, command_line.label, m.u.size);
}

void c_unset_env() {
  struct Msg m = default_msg();

  /* Send the request */
  m.type = UNSET_ENV;
  m.u.size = strlen(command_line.label) + 1;
  send_msg(server_socket, &m);
  send_bytes(server_socket, command_line.label, m.u.size);
}

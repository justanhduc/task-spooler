/*
    Task Spooler - a task queue system for the unix user
    Copyright (C) 2007-2013  Lluís Batlle i Rossell

    Please find the license in the provided COPYING file.
*/
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>

#ifdef linux
#include <sys/time.h>
#endif

#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/un.h>

#ifdef HAVE_UCRED_H
#include <ucred.h>
#endif
#ifdef HAVE_SYS_UCRED_H
#include <sys/ucred.h>
#endif

#include <unistd.h>

#include "main.h"
#include "user.h"

enum { MAXCONN = 1000 };

enum Break { BREAK, NOBREAK, CLOSE };

char *logdir;

/* Prototypes */
static void server_loop(int ls);

static enum Break client_read(int index);

static void end_server(int ls);

static void s_newjob_ok(int index);

static void s_newjob_nok(int index);

static void s_runjob(int jobid, int index);

static void clean_after_client_disappeared(int socket, int index);

struct Client_conn {
  int socket;
  int hasjob;
  int jobid;
  int ts_UID;
};

/* Globals */
static struct Client_conn client_cs[MAXCONN];
static int nconnections;
static char *path;
static int max_descriptors;

/* in jobs.c */
extern int max_jobs;

static void s_send_version(int s) {
  struct Msg m = default_msg();

  m.type = VERSION;
  m.u.version = PROTOCOL_VERSION;

  send_msg(s, &m);
}

static void sigterm_handler(int n) {
  const char *dumpfilename;
  int fd;

  /* Dump the job list if we should to */
  dumpfilename = getenv("TS_SAVELIST");
  if (dumpfilename != NULL) {
    fd = open(dumpfilename, O_WRONLY | O_APPEND | O_CREAT, 0600);
    if (fd != -1) {
      joblist_dump(fd);
      close(fd);
    } else
      warning("The TS_SAVELIST file \"%s\" cannot be opened", dumpfilename);
  }

  /* path will be initialized for sure, before installing the handler */
  unlink(path);
  exit(1);
}

static void set_default_maxslots() {
  char *str;

  str = getenv("TS_SLOTS");
  if (str != NULL) {
    int slots;
    slots = abs(atoi(str));
    s_set_max_slots(0, slots);
  }
}

static void set_socket_model(const char *path) { chmod(path, 0777); }

static void initialize_log_dir() {
  char *tmpdir = getenv("TMPDIR") == NULL ? "/tmp" : getenv("TMPDIR");
  logdir = malloc(strlen(tmpdir) + 1);
  strcpy(logdir, tmpdir);
}

static void install_sigterm_handler() {
  struct sigaction act;

  act.sa_handler = sigterm_handler;
  /* Reset the mask */
  memset(&act.sa_mask, 0, sizeof(act.sa_mask));
  act.sa_flags = 0;

  sigaction(SIGTERM, &act, NULL);
}

static int get_max_descriptors() {
  const int MARGIN = 5; /* stdin, stderr, listen socket, and whatever */
  int max;
  struct rlimit rlim;
  int res;
  const char *str;

  max = MAXCONN;

  str = getenv("TS_MAXCONN");
  if (str != NULL) {
    int user_maxconn;
    user_maxconn = abs(atoi(str));
    if (max > user_maxconn)
      max = user_maxconn;
  }

  if (max > FD_SETSIZE)
    max = FD_SETSIZE - MARGIN;

  /* I'd like to use OPEN_MAX or NR_OPEN, but I don't know if any
   * of them is POSIX compliant */

  res = getrlimit(RLIMIT_NOFILE, &rlim);
  if (res != 0)
    warning("getrlimit for open files");
  else {
    if (max > rlim.rlim_cur)
      max = rlim.rlim_cur - MARGIN;
  }

  if (max < 1)
    error("Too few opened descriptors available");

  return max;
}


void server_main(int notify_fd, char *_path) {
  dbf = fopen("/home/kylin/task-spooler/debug.txt", "w");
  fprintf(dbf, "start server_main\n");
  fflush(dbf);

  int ls;
  struct sockaddr_un addr;
  int res;
  char *dirpath;

  process_type = SERVER;
  max_descriptors = get_max_descriptors();
  /* Arbitrary limit, that will block the enqueuing, but should allow space
   * for usual ts queries */
  max_jobs = max_descriptors - 5;

  path = _path;

  /* Move the server to the socket directory */
  dirpath = malloc(strlen(path) + 1);
  strcpy(dirpath, path);
  chdir(dirname(dirpath));
  free(dirpath);

  nconnections = 0;

  ls = socket(AF_UNIX, SOCK_STREAM, 0);
  if (ls == -1)
    error("cannot create the listen socket in the server");

  addr.sun_family = AF_UNIX;
  strcpy(addr.sun_path, path);

  res = bind(ls, (struct sockaddr *)&addr, sizeof(addr));
  if (res == -1)
    error("Error binding.");

  res = listen(ls, 0);
  if (res == -1)
    error("Error listening.");

  // setup root user
  user_number = 1;
  user_UID[0] = 0;
  user_max_slots[0] = 0;
  strcpy(user_name[0], "Root");
  for (int i = 0; i < USER_MAX; i++) {
    user_busy[i] = 0;
    user_jobs[i] = 0;
    user_queue[i] = 0;
  }

  set_server_logfile();
  int jobid = read_first_jobid_from_logfile(logfile_path);
  s_set_jobids(get_env("TS_FIRST_JOBID", jobid));
  jobsort_flag = get_env("TS_SORTJOBS", 0);

  read_user_file(get_user_path());
  set_socket_model(_path);

  install_sigterm_handler();

  set_default_maxslots();

  initialize_log_dir();

  if (notify_fd != 0)
    notify_parent(notify_fd);
  printf("Start main server loops...\n");
  server_loop(ls);
}

static int get_conn_of_jobid(int jobid) {
  int i;
  for (i = 0; i < nconnections; ++i)
    if (client_cs[i].hasjob && client_cs[i].jobid == jobid)
      return i;
  return -1;
}

static void server_loop(int ls) {
  fd_set readset;
  int i;
  int maxfd;
  int keep_loop = 1;
  int newjob;

  while (keep_loop) {
    FD_ZERO(&readset);
    maxfd = 0;
    /* If we can accept more connections, go on.
     * Otherwise, the system block them (no accept will be done). */
    if (nconnections < max_descriptors) {
      FD_SET(ls, &readset);
      maxfd = ls;
    }

    for (i = 0; i < nconnections; ++i) {
      FD_SET(client_cs[i].socket, &readset);
      if (client_cs[i].socket > maxfd)
        maxfd = client_cs[i].socket;
    }

    select(maxfd + 1, &readset, NULL, NULL, NULL);
    if (FD_ISSET(ls, &readset)) {
      int cs;
      // wait the connection
      cs = accept(ls, NULL, NULL);
      if (cs == -1)
        error("Accepting from %i", ls);

      
      struct ucred scred;
      unsigned int len = sizeof(struct ucred);
      if (getsockopt(cs, SOL_SOCKET, SO_PEERCRED, &scred, &len) == -1)
        error("cannot read peer credentials from %i", cs);
      
      client_cs[nconnections].hasjob = 0;
      client_cs[nconnections].socket = cs;
      
      client_cs[nconnections].ts_UID = get_tsUID(scred.uid);

      if (client_cs[nconnections].ts_UID == -1) {
        close(cs);
      } else {
        ++nconnections;
      }
    }

    for (i = 0; i < nconnections; ++i)
      if (FD_ISSET(client_cs[i].socket, &readset)) {
        enum Break b;
        b = client_read(i);
        /* Check if we should break */
        if (b == CLOSE) {
          warning("Closing");
          /* On unknown message, we close the client,
             or it may hang waiting for an answer */
          clean_after_client_disappeared(client_cs[i].socket, i);
        } else if (b == BREAK) {
          keep_loop = 0;
        }
      } else {
        /*
        if (client_cs[i].hasjob && check_running_dead(client_cs[i].jobid) == 1) {
          clean_after_client_disappeared(client_cs[i].socket, i);
        }
        */
      }
    /* This will return firstjob->jobid or -1 */
    newjob = next_run_job();
    fprintf(dbf, "start new job: %d\n", newjob);
    fflush(dbf);
    if (newjob != -1) {
      int conn, awaken_job;
      conn = get_conn_of_jobid(newjob);
      /* This next marks the firstjob state to RUNNING */
      s_mark_job_running(newjob);
      s_runjob(newjob, conn);

      while ((awaken_job = wake_hold_client()) != -1) {
        int wake_conn = get_conn_of_jobid(awaken_job);
        if (wake_conn == -1)
          error("The job awaken does not have a connection open");
        s_newjob_ok(wake_conn);
      }
    }
  }

  end_server(ls);
}

static void end_server(int ls) {
  close(ls);
  unlink(path);
  /* This comes from the parent, in the fork after server_main.
   * This is the last use of path in this process.*/
  free(path);
}

static void remove_connection(int index) {
  int i;

  if (client_cs[index].hasjob) {
    s_removejob(client_cs[index].jobid);
  }

  for (i = index; i < (nconnections - 1); ++i) {
    memcpy(&client_cs[i], &client_cs[i + 1], sizeof(client_cs[0]));
  }
  nconnections--;
}

static void clean_after_client_disappeared(int socket, int index) {
  /* Act as if the job ended. */
  int jobid = client_cs[index].jobid;
  // fprintf(dbf, "clean %d from client_cs[%d]\n", jobid, index);
  // fflush(dbf);
  if (client_cs[index].hasjob) {
    struct Result r = default_result();

    r.errorlevel = -1;
    r.died_by_signal = 1;
    r.signal = SIGKILL;
    r.user_ms = 0;
    r.system_ms = 0;
    r.real_ms = 0;
    r.skipped = 0;

    warning("JobID %i quit while running.", jobid);
    job_finished(&r, jobid);
    /* For the dependencies */
    check_notify_list(jobid);
    /* We don't want this connection to do anything
     * more related to the jobid, secially on remove_connection
     * when we receive the EOC. */
    client_cs[index].hasjob = 0;
  } else
    /* If it doesn't have a running job,
     * it may well be a notification */
    s_remove_notification(socket);

  close(socket);
  remove_connection(index);
}

static enum Break client_read(int index) {
  struct Msg m = default_msg();
  int s;
  int res;

  s = client_cs[index].socket;

  /* Read the message */
  res = recv_msg(s, &m);
  if (res == -1) {
    warning("client recv failed");    
    fprintf(dbf, "start clean_after_clieeared\n");
    fflush(dbf);
    clean_after_client_disappeared(s, index);
    return NOBREAK;
  } else if (res == 0) {
    fprintf(dbf, "start clean_after_clie2\n");
    fflush(dbf);
    clean_after_client_disappeared(s, index);
    return NOBREAK;
  }
  int ts_UID = client_cs[index].ts_UID;

  /* Process message */
  switch (m.type) {
  case REFRESH_USERS:
    if (ts_UID == 0) {
      s_refresh_users(s);
    }
    close(s);
    remove_connection(index);
    break;
  case LOCK_SERVER:
    s_lock_server(s, ts_UID);
    close(s);
    remove_connection(index);
    break;
  case UNLOCK_SERVER:
    s_unlock_server(s, ts_UID);
    close(s);
    remove_connection(index);
    break;
  case HOLD_JOB:
    s_hold_job(s, m.jobid, ts_UID);
    close(s);
    remove_connection(index);
    break;
  case RESTART_JOB:
    s_restart_job(s, m.jobid, ts_UID);
    close(s);
    remove_connection(index);
    break;
  case STOP_USER:
    // Root, uid in m.jobid
    if (ts_UID == 0) {
      if (m.jobid != 0) {
        s_stop_user(s, get_tsUID(m.jobid));
        s_user_status(s, get_tsUID(m.jobid));
      } else {
        s_stop_all_users(s);
        s_user_status_all(s);
      }
    } else {
      s_stop_user(s, ts_UID);
      s_user_status(s, ts_UID);
    }
    close(s);
    remove_connection(index);
    break;
  case CONT_USER:
    if (ts_UID == 0) {
      if (m.jobid != 0) {
        s_cont_user(s, get_tsUID(m.jobid));
        s_user_status(s, get_tsUID(m.jobid));
      } else {
        s_cont_all_users(s);
        s_user_status_all(s);
      }
    } else {
      s_cont_user(s, ts_UID);
      s_user_status(s, ts_UID);
    }
    close(s);
    remove_connection(index);
    break;
  case KILL_SERVER:
    if (ts_UID == 0)
      return BREAK; /* break in the parent*/
    break;
  case NEWJOB:
    if (m.u.newjob.taskpid != 0) {
      // check if taskpid isnot in queue and from a valid user.
      fprintf(dbf, "ts_UID = %d, taskpid = %d\n", ts_UID, m.u.newjob.taskpid);
      ts_UID = ts(ts_UID, m.u.newjob.taskpid);
      // fprintf(dbf, "ts_UID = %d, taskpid = %d\n", ts_UID, m.u.newjob.taskpid);
      // fflush(dbf);
    } else {
      if (s_check_locker(s, ts_UID) == 1) { break; }
    }

    if (ts_UID < 0 || ts_UID > USER_MAX) { break; }

    client_cs[index].jobid = s_newjob(s, &m, ts_UID);
    client_cs[index].hasjob = 1;
    if (!job_is_holding_client(client_cs[index].jobid))
      s_newjob_ok(index);
    else if (!m.u.newjob.wait_enqueuing) {
      s_newjob_nok(index);
      clean_after_client_disappeared(s, index);
    }
    break;
  case RUNJOB_OK: {
    char *buffer = 0;
    if (m.u.output.store_output) {
      /* Receive the output filename */
      buffer = (char *)malloc(m.u.output.ofilename_size);
      res = recv_bytes(s, buffer, m.u.output.ofilename_size);
      if (res != m.u.output.ofilename_size)
        error("Reading the ofilename");
    }
    s_process_runjob_ok(client_cs[index].jobid, buffer, m.u.output.pid);
  } break;
  case KILL_ALL:
    if (ts_UID == 0)
      s_kill_all_jobs(s);
    break;
  case LIST:
    term_width = m.u.list.term_width;
    if (m.u.list.plain_list)
      s_list_plain(s);
    else
      s_list(s, ts_UID);
    s_user_status(s, ts_UID);
    /* We must actively close, meaning End of Lines */
    close(s);
    remove_connection(index);
    break;
  case LIST_ALL:
    term_width = m.u.list.term_width;
    if (m.u.list.plain_list)
      s_list_plain(s);
    else
      s_list_all(s);
    s_user_status_all(s);
    /* We must actively close, meaning End of Lines */
    close(s);
    remove_connection(index);
    break;
  case INFO:
    s_job_info(s, m.jobid);
    close(s);
    remove_connection(index);
    break;
  case LAST_ID:
    s_send_last_id(s);
    break;
  case GET_LABEL:
    s_get_label(s, m.jobid);
    break;
  case GET_CMD:
    s_send_cmd(s, m.jobid);
    break;
  case ENDJOB:
    job_finished(&m.u.result, client_cs[index].jobid);
    /* For the dependencies */
    check_notify_list(client_cs[index].jobid);
    /* We don't want this connection to do anything
     * more related to the jobid, secially on remove_connection
     * when we receive the EOC. */
    client_cs[index].hasjob = 0;
    break;
  case CLEAR_FINISHED:
    s_clear_finished(ts_UID);
    break;
  case ASK_OUTPUT:
    s_send_output(s, m.jobid);
    break;
  case REMOVEJOB: {
    int went_ok;
    /* Will update the jobid. If it's -1, will set the jobid found */
    went_ok = s_remove_job(s, &m.jobid, ts_UID);
    if (went_ok) {
      int i;
      for (i = 0; i < nconnections; ++i) {
        if (client_cs[i].hasjob && client_cs[i].jobid == m.jobid) {
          close(client_cs[i].socket);

          /* So remove_connection doesn't call s_removejob again */
          client_cs[i].hasjob = 0;

          /* We don't try to remove any notification related to
           * 'i', because it will be for sure a ts client for a job */
          remove_connection(i);
        }
      }
    }
  } break;
  case WAITJOB:
    s_wait_job(s, m.jobid);
    break;
  case WAIT_RUNNING_JOB:
    s_wait_running_job(s, m.jobid);
    break;
  case COUNT_RUNNING:
    s_count_running_jobs(s);
    break;
  case URGENT:
    if (ts_UID == 0 || ts_UID == s_get_job_tsUID(m.jobid)) {
      s_move_urgent(s, m.jobid);
    }
    if (jobsort_flag)
      s_sort_jobs();
    close(s);
    remove_connection(index);
    break;
  case SET_MAX_SLOTS:
    if (ts_UID == 0)
      s_set_max_slots(s, m.u.max_slots);
    close(s);
    remove_connection(index);
    break;
  case GET_MAX_SLOTS:
    s_get_max_slots(s);
    break;
  case SWAP_JOBS:
    if (ts_UID == 0) {
      s_swap_jobs(s, m.u.swap.jobid1, m.u.swap.jobid2);
    } else {
      int job1_uid = s_get_job_tsUID(m.u.swap.jobid1);
      int job2_uid = s_get_job_tsUID(m.u.swap.jobid2);
      if (ts_UID == job1_uid && ts_UID == job2_uid) {
        s_swap_jobs(s, m.u.swap.jobid1, m.u.swap.jobid2);
      }
    }
    if (jobsort_flag)
      s_sort_jobs();
    close(s);
    remove_connection(index);
    break;
  case GET_STATE:
    s_send_state(s, m.jobid);
    break;
  case GET_ENV:
    s_get_env(s, m.u.size);
    break;
  case SET_ENV:
    s_set_env(s, m.u.size);
    break;
  case UNSET_ENV:
    s_unset_env(s, m.u.size);
    break;
  case GET_VERSION:
    s_send_version(s);
    break;
  case GET_LOGDIR:
    s_get_logdir(s);
    break;
  case SET_LOGDIR: {
    char *path = malloc(m.u.size);
    recv_bytes(s, path, m.u.size);
    s_set_logdir(path);
  }
    close(s);
    remove_connection(index);
    break;
  default:
    /* Command not supported */
    /* On unknown message, we close the client,
       or it may hang waiting for an answer */
    warning("Unknown message: %i", m.type);
    return CLOSE;
  }

  return NOBREAK; /* normal */
}

static void s_runjob(int jobid, int index) {
  int s;

  if (!client_cs[index].hasjob)
    error("Run job of the client %i which doesn't have any job", index);

  s = client_cs[index].socket;
  // fprintf(dbf, "socket = %d, jobid = %d\n", s, jobid);
  // fflush(dbf);
  s_send_runjob(s, jobid);
}

static void s_newjob_ok(int index) {
  int s;
  struct Msg m = default_msg();

  if (!client_cs[index].hasjob)
    error("Run job of the client %i which doesn't have any job", index);

  s = client_cs[index].socket;

  m.type = NEWJOB_OK;
  m.jobid = client_cs[index].jobid;

  send_msg(s, &m);
}

static void s_newjob_nok(int index) {
  int s;
  struct Msg m = default_msg();

  if (!client_cs[index].hasjob)
    error("Run job of the client %i which doesn't have any job", index);

  s = client_cs[index].socket;

  m.type = NEWJOB_NOK;

  send_msg(s, &m);
}

static void dump_conn_struct(FILE *out, const struct Client_conn *p) {
  fprintf(out, "  new_conn\n");
  fprintf(out, "    socket %i\n", p->socket);
  fprintf(out, "    hasjob \"%i\"\n", p->hasjob);
  fprintf(out, "    jobid %i\n", p->jobid);
}

void dump_conns_struct(FILE *out) {
  int i;

  fprintf(out, "New_conns");

  for (i = 0; i < nconnections; ++i) {
    dump_conn_struct(out, &client_cs[i]);
  }
}

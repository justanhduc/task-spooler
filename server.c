/*
    Task Spooler PLUS - a multi-user job scheduler like slurm.
    Copyright (C) 2007-2026  Kylin JIANG - Lluís Batlle i Rossell

    Please find the license in the provided COPYING file.
*/
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>

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
#include <time.h>

#ifdef HAVE_UCRED_H
#include <ucred.h>
#endif
#ifdef HAVE_SYS_UCRED_H
#include <sys/ucred.h>
#endif

#include <unistd.h>

#include "main.h"
#include "user.h"
#include "vec.h"

#include "defaults.h"
#include "server.h"
#include "jobs.h"
#include "error.h"
#include "notify.h"
#include "job_ops.h"
#include "server_user.h"
#include "server_env.h"
#include "server_start.h"
#include "sqlite.h"
#include "cgroups.h"
#ifdef TS_CPU_BIND
#include "cpu_bind.h"
#endif
#include "signals.h"
#include "runtime_limit.h"
#include "info.h"
#include "print.h"

enum Break { BREAK, NOBREAK, CLOSE };

char *logdir;
/* Prototypes */
static void server_loop(int ls);

static enum Break client_read(int index);

static void end_server(int ls);

static void s_newjob_ok(int index);

static void s_newjob_nok(int index);

static void clean_after_client_disappeared(int socket, int index);

/* Globals */
static struct Client_conn client_cs[MAXCONN];
static int nconnections;
static char *path;
static int max_descriptors;

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
  exit(EXIT_FAILURE); // sig = 1
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

/*
static void check_jobDB() {
  struct Job* p;
  for (int i = 0; i < jobDB_num; i++) {
    p = jobDB_Jobs[i];
    if (p->pid != 0 && s_check_running_pid(p->pid) != 1) {
      struct Result r = default_result();

      r.errorlevel = -1;
      r.died_by_signal = 1;
      r.signal = SIGKILL;
      r.user_sec = 0;
      r.system_sec = 0;
      r.real_sec = 0;
      r.skipped = 0;

      // warning("JobID %i quit while running.", jobid);
      job_finished(&r, p->jobid);
      check_notify_list(p->jobid);

      jobDB_num--;
      jobDB_Jobs[i] = jobDB_Jobs[jobDB_num];
      return;
    }
  }
}
*/

/*
int s_add_connection(int jobid, int socket, int hasjob, int ts_UID) {
  if (nconnections < MAXCONN) {
    client_cs[nconnections].jobid = jobid;
    client_cs[nconnections].socket = socket;
    client_cs[nconnections].hasjob = hasjob;
    client_cs[nconnections].ts_UID = ts_UID;
    nconnections++;
    return 0;
  } else {
    return 1;
  }
}
*/

void server_main(int notify_fd, char *_path) {
  int ls;
  struct sockaddr_un addr;
  int res;
  char *dirpath;
  signal(SIGCLD, SIG_IGN);
  signal(SIGCHLD, SIG_IGN);

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
  vec_init(&users_vec);
  struct User *root = (struct User *)calloc(1, sizeof(struct User));
  root->uid = 0;
  root->max_slots = 0;
  strcpy(root->name, "Root");
  vec_push(&users_vec, root);
  // jobDB_num = jobDB_wait_num = 0;
  // jobDB_Jobs = NULL;
  set_server_logfile();
  // int jobid = read_first_jobid_from_logfile(logfile_path);
  read_user_file(get_user_path());
  set_socket_model(_path);
  
  install_sigterm_handler();

  set_default_maxslots();

  initialize_log_dir();
  cgroups_clean_all_finished();
#ifdef CGROUP_V2
  cgroups_v2_init();
#endif
#ifdef TS_CPU_BIND
  cpu_bind_init();
  /* --no-bind 服务器级关闭 */
  if (command_line.no_cpu_binding)
      cpu_bind_set_disabled(1);
  if (command_line.no_bind_defrag)
      cpu_bind_set_defrag_disabled(1);
  /* 若 max_slots 超过可绑核总数，降低到可用值 */
  if (cpu_bind_enabled()) {
      int total_cpus = 0;
      for (int i = 0; i < sys_topology.num_nodes; i++)
          total_cpus += sys_topology.nodes[i].num_cores;
      if (max_slots > total_cpus) {
          printf("max_slots %d > total_cpus %d, capping to %d\n",
                 max_slots, total_cpus, total_cpus);
          s_set_max_slots(0, total_cpus);
      }
  }
#endif

  if (notify_fd != 0)
    notify_parent(notify_fd);

  if (open_sqlite() != 0) {
    error("Cannot open sqlite database");
  }
  init_jobs();
  jobsort_flag = get_env("TS_SORTJOBS", 0);

  /* Try to get stored counter from Global table */
  int stored = get_jobids_DB();
  if (stored > 0) {
      jobids = stored;
  } else {
      s_init_jobids(get_env("TS_FIRST_JOBID", 1000));
  }

  s_read_sqlite();
#ifdef TS_CPU_BIND
  cgroups_restore_all_cpu_bind();
#endif
  printf("Start main server loops...\n");
  server_loop(ls);
}

static void server_loop(int ls) {
  fd_set readset;
  int i;
  int maxfd;
  int keep_loop = 1;

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

    {
        struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
        select(maxfd + 1, &readset, NULL, NULL, &tv);
    }
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

      client_cs[nconnections].user = find_user_by_uid(scred.uid);

      if (client_cs[nconnections].user == NULL) {
        printf("Unauthorized connection from UID: %d, please check the user configuration file\n", scred.uid);
        struct Msg m = default_msg();
        m.type = ERROR_INFO;
        char buf[128];
        sprintf(buf, "Unauthorized connection from UID: %d\n", scred.uid);
        m.u.size = strlen(buf) + 1;
        send_msg(cs, &m);
        send_bytes(cs, buf, m.u.size);
        close(cs);
      } else {
        nconnections++;
      }
    }

    for (i = 0; i < nconnections; ++i) {
      if (FD_ISSET(client_cs[i].socket, &readset)) {
        enum Break b;
        b = client_read(i);
        /* Check if we should break */
        if (b == CLOSE) {
          warning("Closing");
          /* On unknown message, we close the client,
             or it may hang waiting for an answer */
          // printf("close to jobid %d nconnections = %d/%d\n", client_cs[i].jobid, i, nconnections);
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
    } // nconnections

    next_run_job();
    s_check_holdon();
#ifdef TS_CPU_BIND
    cpu_bind_defrag_poll();
#endif
  } // end of while (keep_loop)

  end_server(ls);
}

static void end_server(int ls) {
  close(ls);
  unlink(path);
  destroy_jobs();
  close_sqlite();
  /* This comes from the parent, in the fork after server_main.
   * This is the last use of path in this process.*/
  free(path);
}

static void remove_connection(int index) {
  int i;

  if (client_cs[index].hasjob) {
    s_delete_job(client_cs[index].jobid);
  }

  for (i = index; i < (nconnections - 1); ++i) {
    memcpy(&client_cs[i], &client_cs[i + 1], sizeof(client_cs[0]));
  }
  nconnections--;
}


static void clean_after_client_disappeared(int socket, int index) {
  /* Act as if the job ended. */
  int jobid = client_cs[index].jobid;
  if (client_cs[index].hasjob) {
    struct Job *p = findjob(jobid);

    if (p && (p->state == RUNNING || p->state == PAUSE)) {
        /* Running job — kill orphaned child, mark finished */
        if (p->pid > 0) {
            kill(-(p->pid), SIGTERM);
            struct timespec ts = { .tv_sec = 0, .tv_nsec = 50000000L };
            for (int i = 0; i < 10; i++) {
                if (kill(p->pid, 0) != 0) break;
                nanosleep(&ts, NULL);
            }
            if (kill(p->pid, 0) == 0)
                kill(-(p->pid), SIGKILL);
        }

        struct Result r = default_result();
        r.errorlevel = -1;
        r.died_by_signal = 1;
        r.signal = SIGKILL;
        r.user_sec = 0;
        r.system_sec = 0;
        r.real_sec = 0;
        r.skipped = 0;

        warning("JobID %i quit while running.", jobid);
        job_finished(&r, jobid);
        check_notify_list(jobid);
    } else if (p) {
        /* Queued/delink job — just remove from queue */
        warning("JobID %i removed (client disconnected before running).", jobid);
        s_delete_job(jobid);
    }

    client_cs[index].hasjob = 0;
  } else
    /* If it doesn't have a running job,
     * it may well be a notification */
    s_remove_notification(socket);

  close(socket);
  remove_connection(index);
}


static void s_remove_all_queues(struct User *user) {
  int i = 0;
  while (i < nconnections) {
    if (user == USER(0) || client_cs[i].user == user) {
      if (job_is_running(client_cs[i].jobid) != 1) {
        clean_after_client_disappeared(client_cs[i].socket, i);
      } else {
        i++;
      }
    } else {
      i++;
    }
  }
}

static enum Break client_read(int index) {
  // printf("client_read(%d)\n", index);

  struct Msg m = default_msg();
  int s;
  int res;

  s = client_cs[index].socket;
  /* Read the message */
  res = recv_msg(s, &m);
  if (res == -1) {
    warning("client recv failed");
    clean_after_client_disappeared(s, index);
    return NOBREAK;
  } else if (res == 0) {
    clean_after_client_disappeared(s, index);
    return NOBREAK;
  }
  // printf("client_read(%d), m.type = %d\n", index, m.type);
  struct User *user = client_cs[index].user;

  /* Time-out unlock */
  if (user_locker != NULL) {
    time_t dt = get_monotonic_sec() - locker_time;
    if (user_locker->uid == 0) {
      if (dt > DEFAULT_ROOT_LOCK_TIME) user_locker = NULL;
    } else {
      if (dt > DEFAULT_USER_LOCK_TIME) user_locker = NULL;
    }
  }


  /* Process message */
  switch (m.type) {
  case REFRESH_USERS:
    if (user == USER(0)) {
      s_refresh_users(s);
    }
    close(s);
    remove_connection(index);
    break;
  case LOCK_SERVER:
    s_lock_server(s, user);
    close(s);
    remove_connection(index);
    break;
  case UNLOCK_SERVER:
    s_unlock_server(s, user);
    close(s);
    remove_connection(index);
    break;
  case HOLD_JOB:
    s_hold_job(s, m.jobid, user);
    close(s);
    remove_connection(index);
    break;
  case CONT_JOB:
    s_cont_job(s, m.jobid, user);
    close(s);
    remove_connection(index);
    break;
  case SUSPEND_USER:
    // Root, uid in m.jobid
    if (user == USER(0)) {
      if (m.jobid != 0) {
        struct User *target = find_user_by_uid((uid_t)m.jobid);
        s_suspend_user(s, target);
        s_user_status(s, target);
      } else {
        s_suspend_user_all(s);
        s_user_status_all(s);
      }
    } else {
      s_suspend_user(s, user);
      s_user_status(s, user);
    }
    close(s);
    remove_connection(index);
    break;
  case RESUME_USER:
    if (user == USER(0)) {
      if (m.jobid != 0) {
        struct User *target = find_user_by_uid((uid_t)m.jobid);
        s_resume_user(s, target);
        s_user_status(s, target);
      } else {
        s_resume_user_all(s);
        s_user_status_all(s);
      }
    } else {
      s_resume_user(s, user);
      s_user_status(s, user);
    }
    close(s);
    remove_connection(index);
    break;
  case KILL_SERVER:
    if (user == USER(0))
      return BREAK; /* break in the parent*/
    break;
  case NEWJOB:
    if (s_check_locker(user) == 1) { break; }

    if (user == NULL) {
      struct Msg m = default_msg();
      m.type = NEWJOB_PID_NOK;
      send_msg(s, &m);
      break;
    }

    // start a new job in servers
    client_cs[index].jobid = s_newjob(s, &m, user);
    client_cs[index].hasjob = 1;

    if (client_cs[index].jobid == -1) {
      s_newjob_nok(index);
      client_cs[index].hasjob = 0;
      clean_after_client_disappeared(s, index);
      break;
    }
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
    if (m.u.output.pid > 0)
      s_process_runjob_ok(client_cs[index].jobid, buffer, m.u.output.pid);
  } break;
  case KILL_ALL:
      s_kill_all_jobs(s, user);
      s_remove_all_queues(user);
      /* TODO to remove the queued jobs
      for (int i = 0; i < nconnections; i++) {
        client_cs[].hasjob = 0;
        clean_after_client_disappeared(s, index);
      }
      */
  case LIST:
    term_width = m.u.list.term_width;
    s_list(s, user, m.u.list.list_format, m.jobid);

    /* We must actively close, meaning End of Lines */
    close(s);
    remove_connection(index);
    break;
  case LIST_ALL:
    term_width = m.u.list.term_width;
    s_list(s, 0, m.u.list.list_format, m.jobid); // list all

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
  case ADD_WTIME:
    if (user == USER(0)) {
      s_add_wtime(s, m.jobid, m.u.newjob.wall_time);
    }
    close(s);
    remove_connection(index);
    break;
  case GET_CMD:
    s_send_cmd(s, m.jobid);
    break;
  case RECONNECT:
    {
        struct Job *jp = get_job(m.jobid);
        struct Msg resp = default_msg();
        resp.type = RECONNECT_OK;
        resp.jobid = m.jobid;

        if (jp && (jp->state == QUEUED || jp->state == DELINK
                   || jp->state == RUNNING || jp->state == FINISHED)
            && (jp->pid == m.u.reconnect.pid || m.u.reconnect.pid == 0)
            && jp->user == user) {
            /* Only transition to RUNNING if the job was already running */
            if (jp->state == DELINK) jp->state = RUNNING;
            jp->client_socket = s;
            client_cs[index].hasjob = 1;
            client_cs[index].jobid = m.jobid;
            send_msg(s, &resp);
        } else {
            resp.type = ERROR_INFO;
            send_msg(s, &resp);
        }
    }
    break;
  case ENDJOB:
    // printf("job_finished = %x, jobid = %d\n", &m.u.result, client_cs[index].jobid);
    job_finished(&m.u.result, client_cs[index].jobid);

    /* For the dependencies */
    // printf("check_notify_list\n");

    check_notify_list(client_cs[index].jobid);

    /* Send timing info back to the client for --on-finish */
    {
        struct Job *jp = get_job(client_cs[index].jobid);
        struct Msg resp = default_msg();
        resp.type = ENDJOB_OK;
        resp.jobid = client_cs[index].jobid;
        if (jp) {
            resp.u.finish_info.real_sec = jp->result.real_sec;
            resp.u.finish_info.user_sec = jp->result.user_sec;
            resp.u.finish_info.system_sec = jp->result.system_sec;
            resp.u.finish_info.pause_duration = jp->info.pause_duration;
            resp.u.finish_info.start_time = jp->info.start_time;
            resp.u.finish_info.enqueue_time = jp->info.enqueue_time;
            resp.u.finish_info.end_time = jp->info.end_time;
            resp.u.finish_info.num_slots = jp->num_slots;
        }
        send_msg(s, &resp);
    }

    // printf("check_notify_list0\n");
    /* We don't want this connection to do anything
     * more related to the jobid, secially on remove_connection
     * when we receive the EOC. */
    client_cs[index].hasjob = 0;
    break;
  case CLEAR_FINISHED:
    s_clear_finished(user);
    break;
  case ASK_OUTPUT:
    s_send_output(s, m.jobid);
    break;
  case REMOVEJOB: {
    int went_ok;
    /* Will update the jobid. If it's -1, will set the jobid found */
    went_ok = s_remove_job(s, &m.jobid, user);
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
    s_count_running_jobs(s, user);
    break;
  case URGENT: {
    struct User *job_user = s_get_job_user(m.jobid);
    if (user == USER(0) || user == job_user) {
      s_move_urgent(s, m.jobid);
    }
    if (jobsort_flag)
      s_sort_jobs();
    close(s);
    remove_connection(index);
    break;
  }
  case SET_MAX_SLOTS:
    if (user == USER(0))
      s_set_max_slots(s, m.u.max_slots);
    close(s);
    remove_connection(index);
    break;
  case GET_MAX_SLOTS:
    s_get_max_slots(s);
    break;
  case SWAP_JOBS:
    if (user == USER(0)) {
      s_swap_jobs(s, m.u.swap.jobid1, m.u.swap.jobid2);
    } else {
      struct User *job1_user = s_get_job_user(m.u.swap.jobid1);
      struct User *job2_user = s_get_job_user(m.u.swap.jobid2);
      if (user == job1_user && user == job2_user) {
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
  case FIND_PID: {
    struct Msg reply = default_msg();
    reply.type = FIND_PID_RESULT;
    reply.jobid = s_find_pid(m.u.find_pid.pid, m.u.find_pid.deep);
    send_msg(s, &reply);
    break;
  }
  case BIND_OFF:
    if (user == USER(0)) {
#ifdef TS_CPU_BIND
        cpu_bind_set_disabled(1);
        printf("[BIND] CPU binding disabled by root\n");
#endif
    }
    close(s);
    remove_connection(index);
    break;
  case BIND_ON:
    if (user == USER(0)) {
#ifdef TS_CPU_BIND
        cpu_bind_set_disabled(0);
        printf("[BIND] CPU binding enabled by root\n");
#endif
    }
    close(s);
    remove_connection(index);
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

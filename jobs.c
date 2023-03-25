/*
    Task Spooler - a task queue system for the unix user
    Copyright (C) 2007-2013  Lluís Batlle i Rossell

    Please find the license in the provided COPYING file.
*/
#define _DEFAULT_SOURCE
#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "main.h"
#include "user.h"

/* The list will access them */
int busy_slots = 0;
int max_slots = 1;

struct Notify {
  int socket;
  int jobid;
  struct Notify *next;
};

/* Globals */
static struct Job firstjob = {0};
static struct Job first_finished_job = {0};
static int jobids = 1000;
/* This is used for dependencies from jobs
 * already out of the queue */
static int last_errorlevel = 0; /* Before the first job, let's consider
                                   a good previous result */
/* We need this to handle well "-d" after a "-nf" run */
static int last_finished_jobid;

static struct Notify *first_notify = 0;
static char buff[256];
/* server will access them */
int max_jobs;

static struct Job *get_job(int jobid);

void notify_errorlevel(struct Job *p);

void s_set_jobids(int i) { 
  jobids = i; 
  set_jobids_DB(i);
}

static void destroy_job(struct Job *p) {
  if (p != NULL) {
    free(p->notify_errorlevel_to);
    free(p->command);
    free(p->work_dir);
    free(p->output_filename);
    pinfo_free(&p->info);
    free(p->depend_on);
    free(p->label);
    free(p);
  }
}

void send_list_line(int s, const char *str) {
  struct Msg m = default_msg();

  /* Message */
  m.type = LIST_LINE;
  m.u.size = strlen(str) + 1;

  send_msg(s, &m);

  /* Send the line */
  send_bytes(s, str, m.u.size);
}

static void send_urgent_ok(int s) {
  struct Msg m = default_msg();

  /* Message */
  m.type = URGENT_OK;

  send_msg(s, &m);
}

static void send_swap_jobs_ok(int s) {
  struct Msg m = default_msg();

  /* Message */
  m.type = SWAP_JOBS_OK;

  send_msg(s, &m);
}

void s_sort_jobs() {
  struct Job queue;
  struct Job *p_queue, *p_run;
  struct Job *p;

  p_run = &firstjob;
  p_queue = &queue;

  p = firstjob.next;
  while (p != NULL) {
    if (p->state == RUNNING) {
      p_run->next = p;
      p_run = p;
    } else {
      p_queue->next = p;
      p_queue = p;
    }
    p = p->next;
  }
  p_run->next = queue.next;
}

static struct Job *find_previous_job(const struct Job *final) {
  struct Job *p;

  /* Show Queued or Running jobs */
  p = &firstjob;
  while (p != NULL) {
    if (p->next == final)
      return p;
    p = p->next;
  }

  return NULL;
}


static struct Job *findjob(int jobid) {
  struct Job *p;

  /* Show Queued or Running jobs */
  p = firstjob.next;
  while (p != 0) {
    if (p->jobid == jobid)
      return p;
    p = p->next;
  }

  return NULL;
}


static int jobid_from_pid(int pid) {
  // if (pid == 0) return 0;
  struct Job *p = &firstjob;

  while (p->next != NULL) {
    p = p->next;
    if (p->pid == pid) return p->jobid;
  }
  return -1;
}

// return 1 for running, other is dead
int s_check_running_pid(int pid) {
  char cmd[256], filename[256] = "";
  snprintf(cmd, sizeof(cmd), "readlink -f /proc/%d/fd/1", pid);
  linux_cmd(cmd, filename, sizeof(filename));
  return strlen(filename) != 0;
}

// if any error return 0;
int s_check_relink(int s, struct Msg *m, int ts_UID) {
  int pid = m->u.newjob.taskpid;
  int jobid = jobid_from_pid(pid);
  if (jobid != -1) {
    sprintf(buff, "  Error: PID [%i] has already in job queue as Jobid: %i\n", pid, jobid);
    send_list_line(s, buff);
    return -1;
  }

  char filename[256];
  struct stat t_stat;

  snprintf(filename, 256, "/proc/%d/stat", pid);
  if (stat(filename, &t_stat) == -1) {
    sprintf(buff, "  Error: PID [%i] is not running\n", pid);
    send_list_line(s, buff);
    return -1;
  }

  int job_tsUID = get_tsUID(t_stat.st_uid);
  if (ts_UID == 0) { 
    return job_tsUID;
  } else if (ts_UID == job_tsUID) {
    return job_tsUID;
  } else {
    sprintf(buff, "  Error: PID [%i] is owned by [%d] `%s` not the user [%d] `%s`\n",
    pid, user_UID[job_tsUID], user_name[job_tsUID],
    user_UID[ts_UID], user_name[ts_UID]);
    send_list_line(s, buff);
    return -1;
  }
}

/*
int check_running_dead(int jobid) {
  struct Job* p = findjob(jobid);
  if (p->pid != 0 && p->state == RUNNING) {
    // a task is allocated by a pid and is running
    return check_ifsleep(p->pid) == -1;
  }
  return 0;
}
*/

static struct Job *findjob_holding_client() {
  struct Job *p;

  /* Show Queued or Running jobs */
  p = firstjob.next;
  while (p != 0) {
    if (p->state == HOLDING_CLIENT)
      return p;
    p = p->next;
  }

  return 0;
}

static struct Job *find_finished_job(int jobid) {
  struct Job *p;

  /* Show Queued or Running jobs */
  p = first_finished_job.next;
  while (p != 0) {
    if (p->jobid == jobid)
      return p;
    p = p->next;
  }

  return 0;
}

static int count_not_finished_jobs() {
  int count = 0;
  struct Job *p;

  /* Show Queued or Running jobs */
  p = firstjob.next;
  while (p != 0) {
    ++count;
    p = p->next;
  }
  return count;
}

static void add_notify_errorlevel_to(struct Job *job, int jobid) {
  int *p;
  int newsize = (job->notify_errorlevel_to_size + 1) * sizeof(int);
  p = (int *)realloc(job->notify_errorlevel_to, newsize);

  if (p == 0)
    error("Cannot allocate more memory for notify_errorlist_to for jobid %i,"
          " having already %i elements",
          job->jobid, job->notify_errorlevel_to_size);

  job->notify_errorlevel_to = p;
  job->notify_errorlevel_to_size += 1;
  job->notify_errorlevel_to[job->notify_errorlevel_to_size - 1] = jobid;
}

void s_kill_all_jobs(int s) {

  struct Job *p;
  s_count_running_jobs(s);

  /* send running job PIDs */
  p = firstjob.next;
  while (p != 0) {
    if (p->state == RUNNING)
      send(s, &p->pid, sizeof(int), 0);

    p = p->next;
  }
}

void s_count_running_jobs(int s) {
  int count = 0;
  struct Job *p;
  struct Msg m = default_msg();

  /* Count running jobs */
  p = firstjob.next;
  while (p != 0) {
    if (p->state == RUNNING)
      ++count;

    p = p->next;
  }

  /* Message */
  m.type = COUNT_RUNNING;
  m.u.count_running = count;
  send_msg(s, &m);
}

int s_get_job_tsUID(int jobid) {
  struct Job *p = get_job(jobid);
  if (p == NULL) {
    return -1;
  } else {
    return p->ts_UID;
  }
}

void s_get_label(int s, int jobid) {
  struct Job *p = 0;
  char *label;

  if (jobid == -1) {
    /* Find the last job added */
    p = firstjob.next;

    if (p != 0)
      while (p->next != 0)
        p = p->next;

    /* Look in finished jobs if needed */
    if (p == 0) {
      p = first_finished_job.next;
      if (p != 0)
        while (p->next != 0)
          p = p->next;
    }

  } else {
    p = get_job(jobid);
  }

  if (p == 0) {
    snprintf(buff, 255, "Job %i not finished or not running.\n", jobid);
    send_list_line(s, buff);
    return;
  }

  if (p->label) {
    label = (char *)malloc(strlen(p->label) + 1);
    sprintf(label, "%s\n", p->label);
  } else
    label = "";
  send_list_line(s, label);
  if (p->label)
    free(label);
}

void s_send_cmd(int s, int jobid) {
  struct Job *p = 0;
  char *cmd;

  if (jobid == -1) {
    /* Find the last job added */
    p = firstjob.next;

    if (p != 0)
      while (p->next != 0)
        p = p->next;

    /* Look in finished jobs if needed */
    if (p == 0) {
      p = first_finished_job.next;
      if (p != 0)
        while (p->next != 0)
          p = p->next;
    }

  } else {
    p = get_job(jobid);
  }

  if (p == 0) {
    snprintf(buff, 255, "Job %i not finished or not running.\n", jobid);
    send_list_line(s, buff);
    return;
  }
  cmd = (char *)malloc(strlen(p->command) + 1);
  sprintf(cmd, "%s\n", p->command);
  send_list_line(s, cmd);
  free(cmd);
}

void s_mark_job_running(int jobid) {
  struct Job *p;
  p = findjob(jobid);
  if (!p)
    error("Cannot mark the jobid %i RUNNING.", jobid);
  p->state = RUNNING;
}

/* -1 means nothing awaken, otherwise returns the jobid awaken */
int wake_hold_client() {
  struct Job *p;
  p = findjob_holding_client();
  if (p) {
    p->state = QUEUED;
    return p->jobid;
  }
  return -1;
}

const char *jstate2string(enum Jobstate s) {
  const char *jobstate;
  switch (s) {
  case QUEUED:
    jobstate = "queued";
    break;
  case RUNNING:
    jobstate = "running";
    break;
  case FINISHED:
    jobstate = "finished";
    break;
  case SKIPPED:
  case HOLDING_CLIENT:
    jobstate = "skipped";
    break;
  case RELINK:
    jobstate = "relink";
    break;
  case WAIT:
    jobstate = "wait";
    break;
  }
  return jobstate;
}

void s_list(int s, int ts_UID) {
  struct Job *p;
  char *buffer;

  /* Times:   0.00/0.00/0.00 - 4+4+4+2 = 14*/
  buffer = joblist_headers();
  send_list_line(s, buffer);
  free(buffer);

  /* Show Queued or Running jobs */
  // char buf[256];
  p = firstjob.next;
  while (p != 0) {
    // sprintf(buf, "jobid = %d\n", p->jobid);
    // send_list_line(s, buf);

    if (p->state != HOLDING_CLIENT) {
      if (p->ts_UID == ts_UID) {
        buffer = joblist_line(p);
        // sprintf(buf, "== jobid = %d\n", p->jobid);
        // send_list_line(s, buf);
        send_list_line(s, buffer);
        free(buffer);
      }
    }
    p = p->next;
  }

  
  p = first_finished_job.next;
  if (p != NULL && firstjob.next != NULL)
    send_list_line(s, "----- Finished -----\n");
  /* Show Finished jobs */
  while (p != 0) {
    if (p->ts_UID == ts_UID) {
      buffer = joblist_line(p);
      send_list_line(s, buffer);
      free(buffer);
    }
    p = p->next;
  }
  
}

void s_list_all(int s) {
  struct Job *p;
  char *buffer;

  /* Times:   0.00/0.00/0.00 - 4+4+4+2 = 14*/
  buffer = joblist_headers();
  send_list_line(s, buffer);
  free(buffer);

  /* Show Queued or Running jobs */
  p = firstjob.next;
  while (p != 0) {
    if (p->state != HOLDING_CLIENT) {
      buffer = joblist_line(p);
      send_list_line(s, buffer);
      free(buffer);
    }
    p = p->next;
  }

  p = first_finished_job.next;
  if (p != NULL && firstjob.next != NULL)
    send_list_line(s, "\n ----- Finished -----\n");

  /* Show Finished jobs */
  while (p != 0) {
    buffer = joblist_line(p);
    send_list_line(s, buffer);
    free(buffer);
    p = p->next;
  }
}

void s_list_plain(int s) {
  struct Job *p;
  char *buffer;

  /* Show Queued or Running jobs */
  p = firstjob.next;
  while (p != 0) {
    if (p->state != HOLDING_CLIENT) {
      buffer = joblist_line_plain(p);
      send_list_line(s, buffer);
      free(buffer);
    }
    p = p->next;
  }

  p = first_finished_job.next;

  /* Show Finished jobs */
  while (p != 0) {
    buffer = joblist_line_plain(p);
    send_list_line(s, buffer);
    free(buffer);
    p = p->next;
  }
}

static struct Job *newjobptr() {
  struct Job *p;

  p = &firstjob;
  while (p->next != 0)
    p = p->next;

  p->next = (struct Job *)malloc(sizeof(*p));
  p->next->next = 0;
  p->next->output_filename = 0;
  p->next->pid = 0;
  p->next->command  = NULL;
  p->next->work_dir = NULL;
  p->next->command_strip = 0;
  
  struct Procinfo* info= &(p->next->info);
  info->enqueue_time.tv_sec  = 0;
  info->start_time.tv_sec    = 0;
  info->end_time.tv_sec      = 0;
  info->enqueue_time.tv_usec = 0;
  info->start_time.tv_usec   = 0;
  info->end_time.tv_usec     = 0;
  
  return p->next;
}

/* Returns -1 if no last job id found */
static int find_last_jobid_in_queue(int neglect_jobid) {
  struct Job *p;
  int last_jobid = -1;

  p = firstjob.next;
  while (p != 0) {
    if (p->jobid != neglect_jobid && p->jobid > last_jobid)
      last_jobid = p->jobid;
    p = p->next;
  }

  return last_jobid;
}

/* Returns -1 if no last job id found */
static int find_last_stored_jobid_finished() {
  struct Job *p;
  int last_jobid = -1;

  p = first_finished_job.next;
  while (p != 0) {
    if (p->jobid > last_jobid)
      last_jobid = p->jobid;
    p = p->next;
  }

  return last_jobid;
}

/* Returns job id or -1 on error */
int s_newjob(int s, struct Msg *m, int ts_UID, int socket) {
  
  struct Job *p;
  int res;
  int waitjob_flag = 0;
  if (m->jobid != 0) {
    p = findjob(m->jobid);
    if (p != NULL && p->state == WAIT) {
      jobDB_wait_num--;
      waitjob_flag = 1;
      p->state = QUEUED;
    } else {
      return -1;
    }
  }

  if (waitjob_flag == 0) {
    p = newjobptr();
    if (m->jobid != 0) {
      p->jobid = m->jobid;
      jobids = jobids > m->jobid ? jobids : m->jobid + 1;
    } else {
      p->jobid = jobids++;
    }
    if (count_not_finished_jobs() < max_jobs) {
      p->state = QUEUED;
    } else
      p->state = HOLDING_CLIENT;
  }
  // save the ts_UID and record the number of waiting jobs
  p->ts_UID = ts_UID; // get_tsUID(m->uid);
  user_queue[p->ts_UID]++;

  p->num_slots = m->u.newjob.num_slots;
  p->store_output = m->u.newjob.store_output;
  p->should_keep_finished = m->u.newjob.should_keep_finished;
  p->notify_errorlevel_to = 0;
  p->notify_errorlevel_to_size = 0;
  p->depend_on_size = m->u.newjob.depend_on_size;
  p->depend_on = 0;

  /* this error level here is used internally to decide whether a job should be
   * run or not so it only matters whether the error level is 0 or not. thus,
   * summing the absolute error levels of all dependencies is sufficient.*/
  p->dependency_errorlevel = 0;
  if (m->u.newjob.depend_on_size) {
    int *depend_on;
    int foo;
    depend_on = recv_ints(s, &foo);
    assert(p->depend_on_size == foo);

    /* Depend on the last queued job. */
    int idx = 0;
    for (int i = 0; i < p->depend_on_size; i++) {
      /* filter out dependencies that are current jobs */
      if (depend_on[i] >= p->jobid)
        continue;

      p->depend_on = (int *)realloc(p->depend_on, (idx + 1) * sizeof(int));
      /* As we already have 'p' in the queue,
       * neglect it during the find_last_jobid_in_queue() */
      if (depend_on[i] == -1) {
        p->depend_on[idx] = find_last_jobid_in_queue(p->jobid);

        /* We don't trust the last jobid in the queue (running or queued)
         * if it's not the last added job. In that case, let
         * the next control flow handle it as if it could not
         * do_depend on any still queued job. */
        if (last_finished_jobid > p->depend_on[idx])
          p->depend_on[idx] = -1;

        /* If it's queued still without result, let it know
         * its result to p when it finishes. */
        if (p->depend_on[idx] != -1) {
          struct Job *depended_job;
          depended_job = findjob(p->depend_on[idx]);
          if (depended_job != 0)
            add_notify_errorlevel_to(depended_job, p->jobid);
          else
            warning("The jobid %i is queued to do_depend on the jobid %i"
                    " suddenly non existent in the queue",
                    p->jobid, p->depend_on[idx]);
        } else /* Otherwise take the finished job, or the last_errorlevel */
        {
          if (depend_on[i] == -1) {
            int ljobid = find_last_stored_jobid_finished();
            p->depend_on[idx] = ljobid;

            /* If we have a newer result stored, use it */
            /* NOTE:
             *   Reading this now, I don't know how ljobid can be
             *   greater than last_finished_jobid */
            if (last_finished_jobid < ljobid) {
              struct Job *parent;
              parent = find_finished_job(ljobid);
              if (!parent)
                error("jobid %i suddenly disappeared from the finished list",
                      ljobid);
              p->dependency_errorlevel += abs(parent->result.errorlevel);
            } else
              p->dependency_errorlevel += abs(last_errorlevel);
          }
        }
      } else {
        /* The user decided what's the job this new job depends on */
        struct Job *depended_job;
        p->depend_on[idx] = depend_on[i];
        depended_job = findjob(p->depend_on[idx]);

        if (depended_job != 0)
          add_notify_errorlevel_to(depended_job, p->jobid);
        else {
          struct Job *parent;
          parent = find_finished_job(p->depend_on[idx]);
          if (parent) {
            p->dependency_errorlevel += abs(parent->result.errorlevel);
          } else {
            /* We consider as if the job not found
               didn't finish well */
            p->dependency_errorlevel += 1;
          }
        }
      }
      idx++;
    }
    free(depend_on);
    p->depend_on_size = idx;
  }

  /* if dependency list is empty after removing invalid dependencies, make it
   * independent */
  if (p->depend_on_size == 0)
    p->depend_on = 0;

  pinfo_init(&p->info);
  pinfo_set_enqueue_time(&p->info);

  /* load the command */

  char* buff = malloc(m->u.newjob.command_size);
  if (buff == 0)
    error("Cannot allocate memory in s_newjob command_size (%i)",
          m->u.newjob.command_size);
  res = recv_bytes(s, buff, m->u.newjob.command_size);
  if (res == -1)
    error("wrong bytes received");

  if (1) { // waitjob_flag == 0
    p->command = buff;
    p->command_strip = m->u.newjob.command_size_strip;
  } else {
    free(buff);
  }
  
  /* load the work dir */
  p->work_dir = 0;
  if (m->u.newjob.path_size > 0) {
    char *ptr;
    ptr = (char *)malloc(m->u.newjob.path_size);
    if (ptr == 0)
      error("Cannot allocate memory in s_newjob path_size(%i)",
            m->u.newjob.path_size);
    res = recv_bytes(s, ptr, m->u.newjob.path_size);
    if (res == -1)
      error("wrong bytes received");
    p->work_dir = ptr;
  }

  /* load the label */
  p->label = 0;
  if (m->u.newjob.label_size > 0) {
    char *ptr;
    ptr = (char *)malloc(m->u.newjob.label_size);
    if (ptr == 0)
      error("Cannot allocate memory in s_newjob label_size(%i)",
            m->u.newjob.label_size);
    res = recv_bytes(s, ptr, m->u.newjob.label_size);
    if (res == -1)
      error("wrong bytes received");
    p->label = ptr;
  }

  /* load the info */
  if (m->u.newjob.env_size > 0) {
    char *ptr;
    ptr = (char *)malloc(m->u.newjob.env_size);
    if (ptr == 0)
      error("Cannot allocate memory in s_newjob env_size(%i)",
            m->u.newjob.env_size);
    res = recv_bytes(s, ptr, m->u.newjob.env_size);
    if (res == -1)
      error("wrong bytes received");
    pinfo_addinfo(&p->info, m->u.newjob.env_size + 100, "Environment:\n%s",
                  ptr);
    free(ptr);
  }

  /* for relink running task */
  if (m->u.newjob.taskpid != 0) {
    p->pid = m->u.newjob.taskpid;
    struct Procinfo* pinfo = &(p->info);
    pinfo->start_time.tv_sec = 0;
    int num_slots = p->num_slots, id = p->ts_UID;
    busy_slots += num_slots;
    user_busy[id] += num_slots;
    user_jobs[id]++;
    p->state = RELINK;
    p->info.start_time.tv_sec = m->u.newjob.start_time;
    p->info.start_time.tv_usec = 0;

  }
  if(waitjob_flag == 0) insert_DB(p, "Jobs");
  set_jobids_DB(jobids);
  return p->jobid;
}

/* This assumes the jobid exists */
void s_removejob(int jobid) {
  struct Job *p;
  struct Job *newnext;
  /*
  if (firstjob.next.jobid == jobid) {
    struct Job *newfirst;

  // First job is to be removed //
  newfirst = firstjob->next;
  destroy_job(firstjob);
  firstjob = newfirst;
  return;
  }
  */
  p = &firstjob;
  /* Not first job */
  while (p->next != 0) {
    if (p->next->jobid == jobid)
      break;
    p = p->next;
  }
  if (p->next == 0)
    error("Job to be removed not found. jobid=%i", jobid);

  newnext = p->next->next;

  destroy_job(p->next);
  p->next = newnext;
}

/* -1 if no one should be run. */
int next_run_job() {
  struct Job *p;

  /* If there are no jobs to run... */
  if (firstjob.next == 0)
    return -1;
  p = firstjob.next;
  while (p != 0) {
    if (p->state == RELINK) {
      return p->jobid;
    }
    p = p->next;
  }
  // start from a random sequence
  int uid = rand() % user_number;

  const int free_slots = max_slots - busy_slots;

  /* busy_slots may be bigger than the maximum slots,
   * if the user was running many jobs, and suddenly
   * trimmed the maximum slots down. */
  if (free_slots <= 0)
    return -1;

  /* Look for a runnable task */
  for (int i = 0; i < user_number; i++) {
    uid = (uid + 1) % user_number;
    if (user_queue[uid] == 0) {
      continue;
    }
    p = firstjob.next;
    while (p != 0) {
      if (p->state == QUEUED) {
        if (p->depend_on_size) {
          int ready = 1;
          for (int i = 0; i < p->depend_on_size; i++) {
            struct Job *do_depend_job = get_job(p->depend_on[i]);
            /* We won't try to run any job do_depending on an unfinished
             * job */
            if (do_depend_job != NULL && (do_depend_job->state == QUEUED ||
                                          do_depend_job->state == RUNNING)) {
              /* Next try */
              p = p->next;
              ready = 0;
              break;
            }
          }
          if (ready != 1)
            continue;
        }

        int num_slots = p->num_slots, id = p->ts_UID;
        if (id == uid && free_slots >= num_slots &&
            user_max_slots[id] - user_busy[id] >= num_slots) {
          busy_slots += num_slots;
          user_busy[id] += num_slots;

          user_jobs[id]++;
          user_queue[id]--;
          return p->jobid;
        }
      }
      p = p->next;
    }
  }
  return -1;
}

/* Returns 1000 if no limit, The limit otherwise. */
static int get_max_finished_jobs() {
  char *limit;

  limit = getenv("TS_MAXFINISHED");
  if (limit == NULL)
    return 1000;
  int num = abs(atoi(limit));
  if (num < 1)
    num = 1000;
  return num;
}

/* Add the job to the finished queue. */
static void new_finished_job(struct Job *j) {
  struct Job *p;
  int count = 0, max;

  max = get_max_finished_jobs();

  p = &first_finished_job;
  while (p->next != 0) {
    p = p->next;
    ++count;
  }

  /* If too many jobs, wipe out the first */
  if (count >= max) {
    struct Job *tmp;
    tmp = first_finished_job.next;
    first_finished_job.next = tmp->next;
    destroy_job(tmp);
  }
  p->next = j;
  p->next->next = 0;

  delete_DB(j->jobid, "Jobs");
  insert_DB(j, "Finished");
}

static int job_is_in_state(int jobid, enum Jobstate state) {
  struct Job *p;

  p = findjob(jobid);
  if (p == 0)
    return 0;
  if (p->state == state)
    return 1;
  return 0;
}

int job_is_running(int jobid) { return job_is_in_state(jobid, RUNNING); }

int job_is_holding_client(int jobid) {
  return job_is_in_state(jobid, HOLDING_CLIENT);
}

static int in_notify_list(int jobid) {
  struct Notify *n, *tmp;

  n = first_notify;
  while (n != 0) {
    tmp = n;
    n = n->next;
    if (tmp->jobid == jobid)
      return 1;
  }
  return 0;
}

void job_finished(const struct Result *result, int jobid) {
  struct Job *p;

  if (busy_slots <= 0)
    error(
        "Wrong state in the server. busy_slots = %i instead of greater than 0",
        busy_slots);

  p = findjob(jobid);
  if (p == 0)
    error("on jobid %i finished, it doesn't exist", jobid);

  /* The job may be not only in running state, but also in other states, as
   * we call this to clean up the jobs list in case of the client closing the
   * connection. */
  if (p->state == RUNNING) {
    busy_slots = busy_slots - p->num_slots;
    user_busy[p->ts_UID] -= p->num_slots;
    user_jobs[p->ts_UID]--;
  }

  /* Mark state */
  if (result->skipped)
    p->state = SKIPPED;
  else
    p->state = FINISHED;
  p->result = *result;
  last_finished_jobid = p->jobid;
  notify_errorlevel(p);
  pinfo_set_end_time(&p->info);

  if (p->result.died_by_signal)
    pinfo_addinfo(&p->info, 100, "Exit status: killed by signal %i\n",
                  p->result.signal);
  else
    pinfo_addinfo(&p->info, 100, "Exit status: died with exit code %i\n",
                  p->result.errorlevel);

  /* Find the pointing node, to
   * update it removing the finished job. */
  {
    struct Job *jpointer = &firstjob;
    struct Job *newfirst = p->next;

    while (jpointer->next != p) {
      jpointer = jpointer->next;
    }

    /* Add it to the finished queue (maybe temporarily) */
    if (p->should_keep_finished || in_notify_list(p->jobid))
      new_finished_job(p);

    /* Remove it from the run queue */
    if (jpointer == 0)
      error("Cannot remove a finished job from the "
            "queue list (jobid=%i)",
            p->jobid);

    jpointer->next = newfirst;
  }
}
static int fork_cmd(int UID, const char* path, const char* cmd) {
    int pid = -1; //定义一个进程ID变量
    int fd[2]; //定义一个管道数组
    // char buffer[10]; //定义一个缓冲区
    if (pipe(fd) < 0) //创建一个管道
    {
        perror("pipe error"); //打印错误信息
        return -1;
    }
    pid = fork(); //调用fork()函数创建子进程
    if (pid < 0) //如果返回值小于0，表示fork失败
    {
        perror("fork error"); //打印错误信息
        return -1;
    }
    else if (pid == 0) //如果返回值等于0，表示子进程正在运行
    {
        // printf("This is child process, pid = %d\n", getpid()); //打印子进程的ID
        close(fd[0]); //关闭管道的读端
        /*
        if (setuid(UID) < 0) {
          sprintf(buffer, "%d", -1); //将子进程的ID转换为字符串
        } else {
          sprintf(buffer, "%d", getpid()); //将子进程的ID转换为字符串
        }
        write(fd[1], buffer, sizeof(buffer)); //将字符串写入管道的写端
        */
        setuid(UID);
        close(fd[1]); //关闭管道的写端

        if (path != NULL) chdir(path);
        system(cmd);
        exit(0);
        /*
        int cmd_array_size;
        printf("cmd = %s\n", cmd);
        char** cmd_arry = split_str(cmd, &cmd_array_size);
        if (cmd_array_size > 0) {
          printf("run cmd %s\n", cmd_arry[0]);
          system(cmd);
          exit(0);
          // execvp(cmd_arry[0], cmd_arry);
        }
        // execlp("ls", "-l", NULL); //执行ls -l命令，替换当前进程
        */
        return -1;
    }
    else //如果返回值大于0，表示父进程正在运行
    {
        // printf("This is parent process, pid = %d\n", getpid()); //打印父进程的ID
        // close(fd[1]); //关闭管道的写端
        // read(fd[0], buffer, sizeof(buffer)); //从管道的读端读取字符串
        printf("[Child PID:%d] Add queued job: %s\n", pid, cmd); //打印子进程的ID
        // close(fd[0]); //关闭管道的读端
        //wait(NULL); //等待子进程结束
    }
    return pid;
}

static void s_add_job(struct Job* j, struct Job** p) {
  if (j->state == RUNNING || j->state == HOLDING_CLIENT || j->state == RELINK) {
    if (j->pid > 0 && s_check_running_pid(j->pid) == 1) {
      printf("add job %d\n", j->jobid);
      
      int ts_UID = j->ts_UID;
      user_jobs[ts_UID]++;

      if (j->state == RUNNING && check_ifsleep(j->pid) == 1) {
        int slots = j->num_slots;
        user_busy[ts_UID] += slots;
        busy_slots += slots;
      }

      jobDB_Jobs[jobDB_num] = j;
      jobDB_num++;
      (*p)->next = j;
      (*p) = j;

      jobids = jobids > j->jobid ? jobids : j->jobid + 1;
      j = NULL;
    }
  } else if (j->state == QUEUED) {
    printf("add the queue job %d\n", j->jobid);
    j->state = WAIT;
    jobDB_wait_num++;
    (*p)->next = j;
    (*p) = j;


    
    char c[32]; //创建一个存储数字的字符串
    sprintf(c, "-J %d ", j->jobid); //将数字转换为字符串
    int len = strlen(j->command) + strlen(c) + 1; //计算新字符串的长度
    char *str = (char *)calloc(0, len * sizeof(char)); //用malloc函数分配内存
    if (str == NULL) //判断是否分配成功
    {
      printf("Memory allocation failed.\n");
      return;
    }
    strncpy(str, j->command, j->command_strip); //将s数组的前t个字符复制到str中
    strcat(str, c); //将数字字符串连接到str后面
    strcat(str, (j->command + j->command_strip)); //将s剩余的字符串连接到str后面
    fork_cmd(user_UID[j->ts_UID], j->work_dir, str);
    jobids = jobids > j->jobid ? jobids : j->jobid + 1;
    j = NULL;

    /*
    printf("add job %d; CMD = %s\n", j->jobid, j->command);
    jobDB_Jobs[jobDB_num] = j;
    jobDB_num++;
    user_queue[j->ts_UID]++;
    (*p)->next = j;
    (*p) = j;
    */
  }
  
  destroy_job(j);
}

void s_read_sqlite() {
  int num_jobs, *jobs_DB = NULL;
  struct Job *job, *p;
  p = &firstjob;
  num_jobs = read_jobid_DB(&(jobs_DB), "Jobs");
  // printf("read from jobs %d\n", num_jobs);
  jobDB_Jobs = (struct Job**)malloc(sizeof(struct Job*) * num_jobs);
  printf("Jobs:\n");
  for (int i = 0; i < num_jobs; i++) {
    job = read_DB(jobs_DB[i], "Jobs");
    if (job == NULL) {
      printf("Error in reading DB %d\n", jobs_DB[i]);
    } else {
      s_add_job(job, &p);
    }
  }
  p->next = NULL;
  // clear_DB("Jobs");

  // finished jobs
  p = &first_finished_job;
  num_jobs = read_jobid_DB(&(jobs_DB), "Finished");
  printf("Finished:\n");
  for (int i = 0; i < num_jobs; i++) {
    job = read_DB(jobs_DB[i], "Finished");
    if (job == NULL) {
      printf("Error in reading DB %d\n", jobs_DB[i]);
    } else {
      printf("add job: %d from %d\n", job->jobid, jobs_DB[i]);
      p->next = job;
      p = job;
    }
  }
  p->next = NULL;
  free(jobs_DB);
  set_jobids_DB(jobids);
}

void s_clear_finished(int ts_UID) {
  struct Job *p, *other_user_job = &first_finished_job;
  if (first_finished_job.next == NULL)
    return;

  p = first_finished_job.next;
  other_user_job->next = NULL;
  while (p != NULL) {
    struct Job *tmp;
    tmp = p->next;
    if (p->ts_UID == ts_UID || ts_UID == 0) {
      delete_DB(p->jobid, "Finished");
      destroy_job(p);
    } else {
      other_user_job->next = p;
      other_user_job = p;
    }
    p = tmp;
  }
  other_user_job->next = NULL;
}

void s_process_runjob_ok(int jobid, char *oname, int pid) {
  struct Job *p;
  p = findjob(jobid);
  if (p == 0)
    error("Job %i already run not found on runjob_ok", jobid);
  if (p->state != RUNNING)
    error("Job %i not running, but %i on runjob_ok", jobid, p->state);

  p->pid = pid;
  p->output_filename = oname;
  pinfo_set_start_time(&p->info);
  insert_or_replace_DB(p, "Jobs");
  write_logfile(p);
}

void s_send_runjob(int s, int jobid) {
  struct Msg m = default_msg();
  struct Job *p;

  p = findjob(jobid);
  if (p == 0)
    error("Job %i was expected to run", jobid);

  m.type = RUNJOB;

  /* TODO
   * We should make the dependencies update the jobids they're do_depending on.
   * Then, on finish, these could set the errorlevel to send to its dependency
   * childs.
   * We cannot consider that the jobs will leave traces in the finished job list
   * (-nf?) . */

  m.u.last_errorlevel = p->dependency_errorlevel;
  m.jobid = jobid;
  send_msg(s, &m);
}

void s_job_info(int s, int jobid) {
  struct Job *p = 0;
  struct Msg m = default_msg();

  if (jobid == -1) {
    /* This means that we want the job info of the running task, or that
     * of the last job run */
    if (busy_slots > 0) {
      p = firstjob.next;
      if (p == 0)
        error("Internal state WAITING, but job not run."
              "firstjob = %x",
              firstjob.next);
    } else {
      p = first_finished_job.next;
      if (p == 0) {
        send_list_line(s, "No jobs.\n");
        return;
      }
      while (p->next != 0)
        p = p->next;
    }
  } else {
    p = firstjob.next;
    while (p != 0 && p->jobid != jobid)
      p = p->next;

    /* Look in finished jobs if needed */
    if (p == 0) {
      p = first_finished_job.next;
      while (p != 0 && p->jobid != jobid)
        p = p->next;
    }
  }

  if (p == 0) {
    snprintf(buff, 255, "Job %i not finished or not running.\n", jobid);
    send_list_line(s, buff);
    return;
  }

  m.type = INFO_DATA;
  send_msg(s, &m);
  pinfo_dump(&p->info, s);
  fd_nprintf(s, 100, "Command: ");
  if (p->depend_on) {
    fd_nprintf(s, 100, "[%i,", p->depend_on[0]);
    for (int i = 1; i < p->depend_on_size; i++)
      fd_nprintf(s, 100, ",%i", p->depend_on[i]);
    fd_nprintf(s, 100, "]&& ");
  }
  write(s, p->command, strlen(p->command));
  fd_nprintf(s, 100, "\n");
  fd_nprintf(s, 100, "User: %s [%d]\n", user_name[p->ts_UID], user_UID[p->ts_UID]);
  fd_nprintf(s, 100, "Slots required: %4i;  PID: %d\n",
        p->num_slots, p->pid);
  fd_nprintf(s, 100, "Output: %s\n", p->output_filename);
  fd_nprintf(s, 100, "Enqueue time: %s", ctime(&p->info.enqueue_time.tv_sec));
  if (p->state == RUNNING) {
    fd_nprintf(s, 100, "Start time: %s", ctime(&p->info.start_time.tv_sec));
    float t = pinfo_time_until_now(&p->info);
    char *unit = time_rep(&t);
    fd_nprintf(s, 100, "Time running: %f%s\n", t, unit);
  } else if (p->state == FINISHED) {
    fd_nprintf(s, 100, "Start time: %s", ctime(&p->info.start_time.tv_sec));
    fd_nprintf(s, 100, "End time: %s", ctime(&p->info.end_time.tv_sec));
    float t = pinfo_time_run(&p->info);
    char *unit = time_rep(&t);
    fd_nprintf(s, 100, "Time run: %:.4f %s\n", t, unit);
  }
  fd_nprintf(s, 100, "\n");
}

void s_send_last_id(int s) {
  struct Msg m = default_msg();

  m.type = LAST_ID;
  m.jobid = jobids - 1;
  send_msg(s, &m);
}

void s_refresh_users(int s) {
  read_user_file(get_user_path());
  send_list_line(s, "refresh the list success!\n");
}

void s_stop_all_users(int s) {
  for (int i = 1; i < user_number; i++) {
    s_stop_user(s, i);
  }
}

void s_cont_all_users(int s) {
  for (int i = 1; i < user_number; i++) {
    s_cont_user(s, i);
  }
}

void s_cont_user(int s, int ts_UID) {
  // get the sequence of ts_UID
  if (ts_UID < 0 || ts_UID > USER_MAX)
    return;

  user_max_slots[ts_UID] = abs(user_max_slots[ts_UID]);
  user_locked[ts_UID] = 0;

  struct Job *p = firstjob.next;
  while (p != NULL) {
    if (p->ts_UID == ts_UID && p->state == RUNNING) {
      // p->state = HOLDING_CLIENT;
      if (p->pid != 0) {
        // kill(p->pid, SIGCONT);
        kill_pid(p->pid, "CONT");
      }
    }
    p = p->next;
  }

  snprintf(buff, 255, "Resume user: [%d] %s\n", user_UID[ts_UID], user_name[ts_UID]);
  send_list_line(s, buff);
}

void s_stop_user(int s, int ts_UID) {
  // get the sequence of ts_UID
  if (ts_UID < 0 || ts_UID > USER_MAX)
    return;

  user_max_slots[ts_UID] = -abs(user_max_slots[ts_UID]);
  user_locked[ts_UID] = 1;

  struct Job *p = firstjob.next;
  while (p != NULL) {
    if (p->ts_UID == ts_UID && p->state == RUNNING) {
      // p->state = HOLDING_CLIENT;
      if (p->pid != 0) {
        // kill(p->pid, SIGSTOP);
        kill_pid(p->pid, "STOP");
      } else {
        char *label = "(...)";
        if (p->label != NULL)
          label = p->label;
        snprintf(buff, 255, "Error in stop %s [%d] %s | %s\n",
                 user_name[ts_UID], p->jobid, label, p->command);
        send_list_line(s, buff);
      }
    }
    p = p->next;
  }

  snprintf(buff, 255, "Lock user: [%d] %s\n", user_UID[ts_UID], user_name[ts_UID]);
  send_list_line(s, buff);
}

void s_send_output(int s, int jobid) {
  struct Job *p = 0;
  struct Msg m = default_msg();

  if (jobid == -1) {
    /* This means that we want the output info of the running task, or that
     * of the last job run */
    if (busy_slots > 0) {
      p = firstjob.next;
      if (p == 0)
        error("Internal state WAITING, but job not run."
              "firstjob = %x",
              firstjob.next);
    } else {
      p = first_finished_job.next;
      if (p == 0) {
        send_list_line(s, "No jobs.\n");
        return;
      }
      while (p->next != 0)
        p = p->next;
    }
  } else {
    p = get_job(jobid);
    if (p != 0 && p->state != RUNNING && p->state != FINISHED &&
        p->state != SKIPPED)
      p = 0;
  }

  if (p == 0) {
    if (jobid == -1)
      snprintf(buff, 255, "The last job has not finished or is not running.\n");
    else
      snprintf(buff, 255, "Job %i not finished or not running.\n", jobid);
    send_list_line(s, buff);
    return;
  }

  if (p->state == SKIPPED) {
    if (jobid == -1)
      snprintf(buff, 255, "The last job was skipped due to a dependency.\n");

    else
      snprintf(buff, 255, "Job %i was skipped due to a dependency.\n", jobid);
    send_list_line(s, buff);
    return;
  }

  m.type = ANSWER_OUTPUT;
  m.u.output.store_output = p->store_output;
  m.u.output.pid = p->pid;
  if (m.u.output.store_output && p->output_filename)
    m.u.output.ofilename_size = strlen(p->output_filename) + 1;
  else
    m.u.output.ofilename_size = 0;
  send_msg(s, &m);
  if (m.u.output.ofilename_size > 0)
    send_bytes(s, p->output_filename, m.u.output.ofilename_size);
}

void notify_errorlevel(struct Job *p) {
  int i;

  last_errorlevel = p->result.errorlevel;

  for (i = 0; i < p->notify_errorlevel_to_size; ++i) {
    struct Job *notified;
    notified = get_job(p->notify_errorlevel_to[i]);
    if (notified) {
      notified->dependency_errorlevel += abs(p->result.errorlevel);
    }
  }
}

/* jobid is input/output. If the input is -1, it's changed to the jobid
 * removed */
int s_remove_job(int s, int *jobid, int client_tsUID) {
  struct Job *p = 0;
  struct Msg m = default_msg();
  struct Job *before_p = &firstjob;

  if (client_tsUID < 0 || client_tsUID > USER_MAX) { 
    snprintf(buff, 255, "invalid ts_UID [%d] in job removal.\n", client_tsUID);
    send_list_line(s, buff);
    return 0;
  }

  if (*jobid == -1) {
    /* Find the last job added */
    p = firstjob.next;
    if (p != 0) {
      while (p->next != 0) {
        before_p = p;
        p = p->next;
      }
    } else {
      /* last 'finished' */
      p = first_finished_job.next;
      before_p = &first_finished_job;
      if (p) {
        while (p->next != 0) {
          before_p = p;
          p = p->next;
        }
      }
    }
  } else {
    p = findjob(*jobid);
    before_p = find_previous_job(p);
    /* If not found, look in the 'finished' list */
    if (p == 0 || p->jobid != *jobid) {
      p = first_finished_job.next;
      before_p = &first_finished_job;
      if (p != 0) {
        while (p->next != 0 && p->jobid != *jobid) {
          before_p = p;
          p = p->next;
        }
        if (p->jobid != *jobid)
          p = 0;
      }
    }
  }

  if (p != NULL && client_tsUID == 0) {
    client_tsUID = p->ts_UID;
  }

  if (p == NULL || (p->ts_UID != client_tsUID)) {

    if (*jobid == -1)
      snprintf(buff, 255, "The last job cannot be removed.\n");
    else {
      if (p == NULL) {
        snprintf(buff, 255, "The job %i is not in queue.\n", *jobid);
      } else {
        snprintf(buff, 255, "The job %i is owned by [%d] `%s` not the user [%d] `%s`.\n", 
        *jobid, user_UID[p->ts_UID], user_name[p->ts_UID],
        user_UID[client_tsUID], user_name[client_tsUID]);
      }
    }
    if (p != NULL) {
      if (p->ts_UID != client_tsUID) {
        snprintf(buff, 255, "The job %i belongs to %s not %s.\n",
                 *jobid, user_name[p->ts_UID], user_name[client_tsUID]);
      }
    }
    send_list_line(s, buff);
    return 0;
  }

  if (p->state == RUNNING) {
    if (p->pid != 0 && (p->ts_UID == client_tsUID)) {
      // kill((p->pid), SIGTERM);
      // kill_pid(p->pid, "-9");

      if (*jobid == -1)
        snprintf(buff, 255, "Running job of last job is removed.\n");
      else
        snprintf(buff, 255, "Running job [%i] PID: %d by `%s` is removed.\n", *jobid, p->pid, user_name[p->ts_UID]);
      send_list_line(s, buff);
      return 0;
    }
    send_list_line(s, "RUNNING\n");
    return 0;
  }

  /*
  if (p == firstjob) {
    p->state = FINISHED;
    send_list_line(s, "remove the first job\n");
    notify_errorlevel(p);
    destroy_job(p);
    return 0;
  }
  */
  /* Return the jobid found */
  *jobid = p->jobid;
  delete_DB(p->jobid, "Jobs");
  /* Tricks for the check_notify_list */
  p->state = FINISHED;
  p->result.errorlevel = -1;
  notify_errorlevel(p);

  /* Notify the clients in wait_job */
  check_notify_list(m.jobid);

  /* Update the list pointers */
  before_p->next = p->next;

  destroy_job(p);

  m.type = REMOVEJOB_OK;
  send_msg(s, &m);
  return 1;
}

static void add_to_notify_list(int s, int jobid) {
  struct Notify *n;
  struct Notify *new;

  new = (struct Notify *)malloc(sizeof(*new));

  new->socket = s;
  new->jobid = jobid;
  new->next = 0;

  n = first_notify;
  if (n == 0) {
    first_notify = new;
    return;
  }

  while (n->next != 0)
    n = n->next;

  n->next = new;
}

static void send_waitjob_ok(int s, int errorlevel) {
  struct Msg m = default_msg();

  m.type = WAITJOB_OK;
  m.u.result.errorlevel = errorlevel;
  send_msg(s, &m);
}

static struct Job *get_job(int jobid) {
  struct Job *j;

  j = findjob(jobid);
  if (j != NULL)
    return j;

  j = find_finished_job(jobid);

  if (j != NULL)
    return j;

  return 0;
}

int s_check_locker(int s, int ts_UID) {
  int dt = time(NULL) - locker_time;
  int res;

  if (user_locker != 0 && dt > 30) {
    user_locker = -1;
  }

  if (user_locker == -1) {
    res = 0;
  } else {
    if (user_locker == ts_UID) {
      res = 0;
    } else {
      res = 1;
    }
  }

  return res;
}

void s_lock_server(int s, int ts_UID) {
  if (ts_UID == 0) {
    user_locker = 0;
    locker_time = time(NULL);
    snprintf(buff, 255, "lock the task-spooler server by Root\n");
  } else {
    if (user_locker == -1) {
      user_locker = ts_UID;
      locker_time = time(NULL);
      snprintf(buff, 255, "lock the task-spooler server by [%d] `%s`\n",
                user_UID[user_locker], user_name[ts_UID]);
    } else {
      if (user_locker == ts_UID) {
        snprintf(buff, 255,
                 "The task-spooler server has already been locked by [%d] `%s`\n",
                 user_UID[user_locker], user_name[user_locker]);
      } else {
        snprintf(buff, 255,
                 "Error: the task-spooler server has already been locked by other user [%d] `%s`\n",
                 user_UID[user_locker], user_name[user_locker]);
      }
    }
  }
  send_list_line(s, buff);
}

void s_unlock_server(int s, int ts_UID) {
  if (user_locker == -1) {
    snprintf(buff, 255,
    "The task-spooler server has already been unlocked\n");
  } else {
    if (ts_UID == 0) {
    user_locker = -1;
    snprintf(buff, 255, "Unlock the task-spooler server by Root\n");
    } else {
      if (user_locker == ts_UID) {
        user_locker = -1;
        snprintf(buff, 255, "Unlock the task-spooler server by [%d] `%s`\n",
                 user_UID[ts_UID], user_name[ts_UID]);
      } else {
        snprintf(buff, 255,
                 "Error: the task-spooler server locked by other user cannot be unlocked by [%d] `%s`\n",
                 user_UID[ts_UID], user_name[ts_UID]);

      }
    }

  }
  send_list_line(s, buff);
}

void s_pause_job(int s, int jobid, int ts_UID) {
  struct Job *p;
  p = findjob(jobid);
  if (p == 0) {
    snprintf(buff, 255, "Error: cannot find job [%d]\n", jobid);
    send_list_line(s, buff);
    return;
  }
  if (check_ifsleep(p->pid) == 1) {
    snprintf(buff, 255, "job [%d] is aleady in PAUSE.\n", jobid);
    send_list_line(s, buff);
    return;
  }
  int job_tsUID = p->ts_UID;
  if (p->pid != 0 && (job_tsUID = ts_UID || ts_UID == 0)) {
    // kill(p->pid, SIGSTOP);
    // kill_pid(p->pid, "-stop");
    user_busy[ts_UID] -= p->num_slots;
    busy_slots -= p->num_slots;
    user_queue[ts_UID]--;
    user_jobs[ts_UID]--;
    snprintf(buff, 255, "To pause job [%d] successfully!\n", jobid);

  } else {
    snprintf(buff, 255, "Error: cannot pause job [%d]\n", jobid);
  }
  send_list_line(s, buff);

}

void s_rerun_job(int s, int jobid, int ts_UID) {
  struct Job *p;

  p = findjob(jobid);
  if (p == 0) {
    snprintf(buff, 255, "Error: cannot find job [%d]\n", jobid);
    send_list_line(s, buff);
    return;
  }
  
  if (check_ifsleep(p->pid) == 0) {
    snprintf(buff, 255, "job [%d] is aleady in RUNNING.\n", jobid);
    send_list_line(s, buff);
    return;
  }
  int job_tsUID = p->ts_UID;
  if (p->pid != 0 && (job_tsUID = ts_UID || ts_UID == 0)) {
    // kill(p->pid, SIGCONT);
    // kill_pid(p->pid, "-cont");
    int num_slots = p->num_slots;
    if (user_busy[ts_UID] + num_slots < user_max_slots[ts_UID] && busy_slots + num_slots <= max_slots) {
      user_busy[ts_UID] += num_slots;
      busy_slots += num_slots;
      user_queue[ts_UID]++;
      user_jobs[ts_UID]++;
      snprintf(buff, 255, "To rerun job [%d] successfully!\n", jobid);
    } else {
      snprintf(buff, 255, "Error: not enough slots [%d]\n", jobid);
    }
  } else {
    snprintf(buff, 255, "Error: cannot rerun job [%d]\n", jobid);
  }
  send_list_line(s, buff);
}
/* Don't complain, if the socket doesn't exist */
void s_remove_notification(int s) {
  struct Notify *n;
  struct Notify *previous;
  n = first_notify;
  while (n != 0 && n->socket != s)
    n = n->next;
  if (n == 0 || n->socket != s)
    return;

  /* Remove the notification */
  previous = first_notify;
  if (n == previous) {
    first_notify = n->next;
    free(n);
    return;
  }

  /* if not the first... */
  while (previous->next != n)
    previous = previous->next;

  previous->next = n->next;
  free(n);
}

static void destroy_finished_job(struct Job *j) {
  struct Job *p = &first_finished_job;
  while (p->next != 0) {
    if (p->next != j) {
      p = p->next;
    } else {
      p->next = j->next;
      destroy_job(j);
      return;
    }
  }
  error("Cannot destroy the expected job %i", j->jobid);
}

/* This is called when a job finishes */
void check_notify_list(int jobid) {
  struct Notify *n, *tmp;
  struct Job *j;

  n = first_notify;
  while (n != 0) {
    tmp = n;
    n = n->next;
    if (tmp->jobid == jobid) {
      j = get_job(jobid);
      /* If the job finishes, notify the waiter */
      if (j->state == FINISHED || j->state == SKIPPED) {
        send_waitjob_ok(tmp->socket, j->result.errorlevel);
        /* We want to get the next Nofity* before we remove
         * the actual 'n'. As s_remove_notification() simply
         * removes the element from the linked list, we can
         * safely follow on the list from n->next. */
        s_remove_notification(tmp->socket);

        /* Remove the jobs that were temporarily in the finished list,
         * just for their notifiers. */
        if (!in_notify_list(jobid) && !j->should_keep_finished) {
          destroy_finished_job(j);
        }
      }
    }
  }
}

void s_wait_job(int s, int jobid) {
  struct Job *p = 0;

  if (jobid == -1) {
    /* Find the last job added */
    p = firstjob.next;

    if (p != 0)
      while (p->next != 0)
        p = p->next;

    /* Look in finished jobs if needed */
    if (p == 0) {
      p = first_finished_job.next;
      if (p != 0)
        while (p->next != 0)
          p = p->next;
    }
  } else {
    p = firstjob.next;
    while (p != 0 && p->jobid != jobid)
      p = p->next;

    /* Look in finished jobs if needed */
    if (p == 0) {
      p = first_finished_job.next;
      while (p != 0 && p->jobid != jobid)
        p = p->next;
    }
  }

  if (p == 0) {
    if (jobid == -1)
      snprintf(buff, 255, "The last job cannot be waited.\n");
    else
      snprintf(buff, 255, "The job %i cannot be waited.\n", jobid);
    send_list_line(s, buff);
    return;
  }

  if (p->state == FINISHED || p->state == SKIPPED) {
    send_waitjob_ok(s, p->result.errorlevel);
  } else
    add_to_notify_list(s, p->jobid);
}

void s_wait_running_job(int s, int jobid) {
  struct Job *p = 0;

  /* The job finding algorithm should be similar to that of
   * s_send_output, because this will be used by "-t" and "-c" */
  if (jobid == -1) {
    /* This means that we want the output info of the running task, or that
     * of the last job run */
    if (busy_slots > 0) {
      p = firstjob.next;
      if (p == 0)
        error("Internal state WAITING, but job not run."
              "firstjob = %x",
              firstjob.next);
    } else {
      p = first_finished_job.next;
      if (p == 0) {
        send_list_line(s, "No jobs.\n");
        return;
      }
      while (p->next != 0)
        p = p->next;
    }
  } else {
    p = firstjob.next;
    while (p != 0 && p->jobid != jobid)
      p = p->next;

    /* Look in finished jobs if needed */
    if (p == 0) {
      p = first_finished_job.next;
      while (p != 0 && p->jobid != jobid)
        p = p->next;
    }
  }

  if (p == 0) {
    if (jobid == -1)
      snprintf(buff, 255, "The last job cannot be waited.\n");
    else
      snprintf(buff, 255, "The job %i cannot be waited.\n", jobid);
    send_list_line(s, buff);
    return;
  }

  if (p->state == FINISHED || p->state == SKIPPED) {
    send_waitjob_ok(s, p->result.errorlevel);
  } else
    add_to_notify_list(s, p->jobid);
}

void s_set_max_slots(int s, int new_max_slots) {
  if (new_max_slots > 0)
    max_slots = new_max_slots;
  else
    warning("Received new_max_slots=%i", new_max_slots);
  if (s > 0) {
    snprintf(buff, 255, "Reset the number of slots: %d\n", max_slots);
    send_list_line(s, buff);
  }
}

void s_get_max_slots(int s) {
  struct Msg m = default_msg();

  /* Message */
  m.type = GET_MAX_SLOTS_OK;
  m.u.max_slots = max_slots;

  send_msg(s, &m);
}

/* move jobid upto the top of list */
void s_move_urgent(int s, int jobid) {
  struct Job *p = 0;
  struct Job *tmp1;

  if (jobid == -1) {
    /* Find the last job added */
    p = firstjob.next;

    if (p != 0)
      while (p->next != 0)
        p = p->next;
  } else {
    p = firstjob.next;
    while (p != 0 && p->jobid != jobid)
      p = p->next;
  }

  // firstjob.next means no run job
  if (p == 0 || firstjob.next == 0) {
    if (jobid == -1)
      snprintf(buff, 255, "The last job cannot be urged.\n");
    else
      snprintf(buff, 255, "The job %i cannot be urged.\n", jobid);
    send_list_line(s, buff);
    return;
  }

  /* Interchange the pointers */
  tmp1 = find_previous_job(p);
  if (tmp1 == NULL) {
    snprintf(buff, 255, "The job %i cannot be urged.\n", jobid);
    send_list_line(s, buff);
    return;
  }
  tmp1->next = p->next;
  p->next = firstjob.next;
  firstjob.next = p;
  movetop_DB(jobid);
  send_urgent_ok(s);
}

void s_swap_jobs(int s, int jobid1, int jobid2) {
  struct Job *p1, *p2;
  struct Job *prev1, *prev2;
  struct Job *tmp;

  p1 = findjob(jobid1);
  p2 = findjob(jobid2);

  if (p1 == NULL || p2 == NULL) {
    snprintf(buff, 255, "The jobs %i and %i cannot be swapped.\n", jobid1,
             jobid2);
    send_list_line(s, buff);
    return;
  }

  /* Interchange the pointers */
  prev1 = find_previous_job(p1);
  prev2 = find_previous_job(p2);
  prev1->next = p2;
  prev2->next = p1;
  tmp = p1->next;
  p1->next = p2->next;
  p2->next = tmp;
  swap_DB(jobid1, jobid2);
  send_swap_jobs_ok(s);
}

static void send_state(int s, enum Jobstate state) {
  struct Msg m = default_msg();

  m.type = ANSWER_STATE;
  m.u.state = state;

  send_msg(s, &m);
}

void s_send_state(int s, int jobid) {
  struct Job *p = 0;

  if (jobid == -1) {
    /* Find the last job added */
    p = firstjob.next;

    if (p != 0)
      while (p->next != 0)
        p = p->next;

    /* Look in finished jobs if needed */
    if (p == 0) {
      p = first_finished_job.next;
      if (p != 0)
        while (p->next != 0)
          p = p->next;
    }

  } else {
    p = get_job(jobid);
  }

  if (p == 0) {
    if (jobid == -1)
      snprintf(buff, 255, "The last job cannot be stated.\n");
    else
      snprintf(buff, 255, "The job %i cannot be stated.\n", jobid);
    send_list_line(s, buff);
    return;
  }

  /* Interchange the pointers */
  send_state(s, p->state);
}

static void dump_job_struct(FILE *out, const struct Job *p) {
  fprintf(out, "  new_job\n");
  fprintf(out, "    jobid %i\n", p->jobid);
  fprintf(out, "    command \"%s\"\n", p->command);
  fprintf(out, "    state %s\n", jstate2string(p->state));
  fprintf(out, "    result.errorlevel %i\n", p->result.errorlevel);
  fprintf(out, "    output_filename \"%s\"\n",
          p->output_filename ? p->output_filename : "NULL");
  fprintf(out, "    store_output %i\n", p->store_output);
  fprintf(out, "    pid %i\n", p->pid);
  fprintf(out, "    should_keep_finished %i\n", p->should_keep_finished);
}

void dump_jobs_struct(FILE *out) {
  const struct Job *p;

  fprintf(out, "New_jobs\n");

  p = firstjob.next;
  while (p != 0) {
    dump_job_struct(out, p);
    p = p->next;
  }

  p = first_finished_job.next;
  while (p != 0) {
    dump_job_struct(out, p);
    p = p->next;
  }
}

static void dump_notify_struct(FILE *out, const struct Notify *n) {
  fprintf(out, "  notify\n");
  fprintf(out, "    jobid %i\n", n->jobid);
  fprintf(out, "    socket \"%i\"\n", n->socket);
}

void dump_notifies_struct(FILE *out) {
  const struct Notify *n;

  fprintf(out, "New_notifies\n");

  n = first_notify;
  while (n != 0) {
    dump_notify_struct(out, n);
    n = n->next;
  }
}

void joblist_dump(int fd) {
  struct Job *p;
  char *buffer;

  buffer = joblistdump_headers();
  write(fd, buffer, strlen(buffer));
  free(buffer);

  /* We reuse the headers from the list */
  buffer = joblist_headers();
  write(fd, "# ", 2);
  write(fd, buffer, strlen(buffer));

  /* Show Finished jobs */
  p = first_finished_job.next;
  while (p != 0) {
    buffer = joblist_line(p);
    write(fd, "# ", 2);
    write(fd, buffer, strlen(buffer));
    free(buffer);
    p = p->next;
  }

  write(fd, "\n", 1);

  /* Show Queued or Running jobs */
  p = firstjob.next;
  while (p != 0) {
    buffer = joblistdump_torun(p);
    write(fd, buffer, strlen(buffer));
    free(buffer);
    p = p->next;
  }
}

void s_get_logdir(int s) { send_list_line(s, logdir); }

void s_set_logdir(const char *path) {
  logdir = realloc(logdir, strlen(path) + 1);
  strcpy(logdir, path);
}

void s_get_env(int s, int size) {
  char *var = malloc(size);
  int res = recv_bytes(s, var, size);
  if (res != size)
    error("Receiving environment variable name");

  char *val = getenv(var);
  struct Msg m = default_msg();
  m.type = LIST_LINE;
  m.u.size = val ? strlen(val) + 1 : 0;
  send_msg(s, &m);
  if (val)
    send_bytes(s, val, m.u.size);

  free(var);
}

void s_set_env(int s, int size) {
  char *var = malloc(size);
  int res = recv_bytes(s, var, size);
  if (res != size)
    error("Receiving environment variable name");

  /* get the var name */
  char *name = strtok(var, "=");

  /* get the var value */
  char *val = strtok(NULL, "=");
  setenv(name, val, 1);
  free(var);
}

void s_unset_env(int s, int size) {
  char *var = malloc(size);
  int res = recv_bytes(s, var, size);
  if (res != size)
    error("Receiving environment variable name");

  unsetenv(var);
  free(var);
}

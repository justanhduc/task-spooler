/*
    Task Spooler PLUS - a multi-user job scheduler like slurm.
    Copyright (C) 2007-2026  Kylin JIANG - Lluís Batlle i Rossell

    Please find the license in the provided COPYING file.
*/
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "main.h"
#include "vec.h"
#include "notify.h"
#include "jobs.h"
#include "error.h"

struct Notify {
  int socket;
  int jobid;
  struct Notify *next;
};

static struct Notify *first_notify = 0;

static void send_waitjob_ok(int s, int errorlevel) {
  struct Msg m = default_msg();
  m.type = WAITJOB_OK;
  m.u.result.errorlevel = errorlevel;
  send_msg(s, &m);
}

static int in_notify_list(int jobid) {
  struct Notify *n;
  n = first_notify;
  while (n != 0) {
    if (n->jobid == jobid) return 1;
    n = n->next;
  }
  return 0;
}

static void add_to_notify_list(int s, int jobid) {
  struct Notify *new_entry;
  new_entry = (struct Notify *)malloc(sizeof(*new_entry));
  new_entry->socket = s;
  new_entry->jobid = jobid;
  new_entry->next = 0;

  if (first_notify == 0) {
    first_notify = new_entry;
    return;
  }

  struct Notify *n = first_notify;
  while (n->next != 0) n = n->next;
  n->next = new_entry;
}

void s_remove_notification(int s) {
  struct Notify *n = first_notify;
  while (n != 0 && n->socket != s) n = n->next;
  if (n == 0 || n->socket != s) return;

  if (n == first_notify) {
    first_notify = n->next;
    free(n);
    return;
  }

  struct Notify *prev = first_notify;
  while (prev->next != n) prev = prev->next;
  prev->next = n->next;
  free(n);
}

void check_notify_list(int jobid) {
  struct Notify *n, *tmp;
  struct Job *j;

  n = first_notify;
  while (n != 0) {
    tmp = n;
    n = n->next;
    if (tmp->jobid == jobid) {
      j = get_job(jobid);
      if (j->state == FINISHED || j->state == SKIPPED) {
        send_waitjob_ok(tmp->socket, j->result.errorlevel);
        s_remove_notification(tmp->socket);

        if (!in_notify_list(jobid) && !j->should_keep_finished)
          destroy_finished_job(j);
      }
    }
  }
}

void s_wait_job(int s, int jobid) {
  struct Job *p = 0;

  if (jobid == -1) {
    extern vec_t active_jobs;
    size_t an = vec_size(&active_jobs);
    if (an > 0) {
      p = (struct Job *)vec_get(&active_jobs, an - 1);
    } else {
      size_t fn = vec_size(&finished_jobs);
      if (fn > 0) p = (struct Job *)vec_get(&finished_jobs, fn - 1);
    }
  } else {
    p = get_job(jobid);
  }

  if (p == 0) {
    snprintf(buff, 255, "The job %i cannot be waited.\n", jobid);
    send_list_line(s, buff);
    return;
  }

  if (p->state == FINISHED || p->state == SKIPPED)
    send_waitjob_ok(s, p->result.errorlevel);
  else
    add_to_notify_list(s, p->jobid);
}

void s_wait_running_job(int s, int jobid) {
  struct Job *p = 0;

  if (jobid == -1) {
    extern vec_t active_jobs;
    if (busy_slots > 0) {
      if (vec_size(&active_jobs) == 0)
        error("Internal state WAITING, but no active job.");
      p = (struct Job *)vec_get(&active_jobs, 0);
    } else {
      size_t fn = vec_size(&finished_jobs);
      if (fn == 0) {
        send_list_line(s, "No jobs.\n");
        return;
      }
      p = (struct Job *)vec_get(&finished_jobs, fn - 1);
    }
  } else {
    p = get_job(jobid);
  }

  if (p == 0) {
    snprintf(buff, 255, "The job %i cannot be waited.\n", jobid);
    send_list_line(s, buff);
    return;
  }

  if (p->state == FINISHED || p->state == SKIPPED)
    send_waitjob_ok(s, p->result.errorlevel);
  else
    add_to_notify_list(s, p->jobid);
}

void dump_notifies_struct(FILE *out) {
  const struct Notify *n;
  fprintf(out, "New_notifies\n");
  n = first_notify;
  while (n != 0) {
    fprintf(out, "  notify\n");
    fprintf(out, "    jobid %i\n", n->jobid);
    fprintf(out, "    socket \"%i\"\n", n->socket);
    n = n->next;
  }
}

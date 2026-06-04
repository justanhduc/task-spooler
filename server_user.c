/*
    Task Spooler PLUS - a multi-user job scheduler like slurm.
    Copyright (C) 2007-2026  Kylin JIANG - Lluís Batlle i Rossell

    Please find the license in the provided COPYING file.

    Server-side user management: suspend, resume, lock, hold, cont.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "main.h"
#include "user.h"
#include "vec.h"
#include "server_user.h"
#include "jobs.h"
#include "runtime_limit.h"
#include "sqlite.h"
#include "cpu_bind.h"
#include "utils.h"
#include "list.h"
#include "cgroups.h"

void s_user_status_all(int s) {
  char buffer[256];
  char *extra;
  send_list_line(s, "-- Users ----------- \n");
  for (int i = 0; i < (int)vec_size(&users_vec); i++) {
    extra = USER(i)->locked != 0 ? "Locked" : "";
    if (USER(i)->max_slots == 0 && USER(i)->busy == 0)
      continue;
    snprintf(buffer, 256, "[%04d] %3d/%-4d Q:%-3d %16s Run. %2d %s\n",
             USER(i)->uid, USER(i)->busy, abs(USER(i)->max_slots), USER(i)->queue,
             USER(i)->name, USER(i)->jobs, extra);
    send_list_line(s, buffer);
  }
  snprintf(buffer, 256, "Service at UID:%d\n", server_uid);
  send_list_line(s, buffer);
}

void s_user_status(int s, struct User *u) {
  char buffer[256];
  char *extra = "";
  if (u->locked != 0)
    extra = "Locked";
  snprintf(buffer, 256, "[%04d] %3d/%-4d Q:%-3d %16s Run. %2d %s\n",
           u->uid, u->busy, abs(u->max_slots), u->queue,
           u->name, u->jobs, extra);
  send_list_line(s, buffer);
}

struct User *s_get_job_user(int jobid) {
  struct Job *p = get_job(jobid);
  if (p == NULL) return NULL;
  return p->user;
}

void s_refresh_users(int s) {
  /* Snapshot current users: refresh only allows adding, not removing or changing */
  size_t old_n = vec_size(&users_vec);
  uid_t *old_uid = (uid_t *)malloc(old_n * sizeof(uid_t));
  int   *old_slots = (int *)malloc(old_n * sizeof(int));
  if (old_uid == NULL || old_slots == NULL) {
    free(old_uid); free(old_slots);
    send_list_line(s, "Error: out of memory during refresh\n");
    return;
  }
  for (size_t i = 0; i < old_n; i++) {
    old_uid[i]   = USER(i)->uid;
    old_slots[i] = USER(i)->max_slots;
  }

  read_user_file(get_user_path());

  /* Verify: every existing user still present with same slots */
  int ok = 1;
  for (size_t i = 0; i < old_n; i++) {
    struct User *u = find_user_by_uid(old_uid[i]);
    if (u == NULL) {
      snprintf(buff, 255, "Error: refresh would remove uid=%d, rejected\n", old_uid[i]);
      ok = 0; break;
    }
    if (u->max_slots != old_slots[i]) {
      snprintf(buff, 255, "Error: slots for uid=%d changed %d->%d, rejected\n",
               old_uid[i], old_slots[i], u->max_slots);
      ok = 0; break;
    }
  }

  if (!ok) {
    /* Rollback: remove newly added users, restore original slots */
    while (vec_size(&users_vec) > old_n) {
      struct User *u = (struct User *)vec_pop(&users_vec);
      free(u);
    }
    for (size_t i = 0; i < old_n; i++)
      USER(i)->max_slots = old_slots[i];
    send_list_line(s, buff);
  } else {
    send_list_line(s, "refresh the list success!\n");
    s_update_slots_usage();
  }
  free(old_uid); free(old_slots);
}

void s_suspend_user_all(int s) {
  for (int i = 1; i < (int)vec_size(&users_vec); i++)
    s_suspend_user(s, USER(i));
}

void s_resume_user_all(int s) {
  for (int i = 1; i < (int)vec_size(&users_vec); i++)
    s_resume_user(s, USER(i));
}

void s_resume_user(int s, struct User *u) {
#ifdef TS_CPU_BIND
  if (cpu_bind_alloc_is_locked()) {
    cpu_bind_defer_resume(s, u);
    return;
  }
#endif
  u->max_slots = abs(u->max_slots);
  u->locked = 0;

  size_t n = vec_size(&active_jobs);
  for (size_t i = 0; i < n; i++) {
    struct Job *p = (struct Job *)vec_get(&active_jobs, i);
    if (p->user == u && p->state == PAUSE) {
      if (p->pid != 0) config_running(p);
    }
  }
  snprintf(buff, 255, "Resume user: [%04d] %-20s\n", u->uid, u->name);
  send_list_line(s, buff);
}

void s_suspend_user(int s, struct User *u) {
  u->max_slots = -abs(u->max_slots);
  u->locked = 1;

  size_t n = vec_size(&active_jobs);
  for (size_t i = 0; i < n; i++) {
    struct Job *p = (struct Job *)vec_get(&active_jobs, i);
    if (p->user == u && p->state == RUNNING) {
      if (p->pid != 0) {
#ifdef TS_CPU_BIND
        if (cpu_bind_alloc_is_locked()) {
          cpu_bind_defer_suspend(s, u);
          return;
        }
#endif
        safe_pause_job(p);
        p->state = PAUSE;
      } else {
        const char *label = "(...)";
        if (p->label != NULL) label = p->label;
        snprintf(buff, 255, "Error in stop %s [%d] %s | %s\n",
                 u->name, p->jobid, label, p->command);
        send_list_line(s, buff);
      }
    }
  }

  snprintf(buff, 255, "Suspend user: [%04d] %-20s\n", u->uid, u->name);
  send_list_line(s, buff);
  s_update_slots_usage();
}

int s_check_locker(struct User *u) {
  time_t dt = get_monotonic_sec() - locker_time;
  int res;
  if (user_locker != NULL && dt > 30)
    user_locker = NULL;

  if (user_locker == NULL)
    res = 0;
  else if (user_locker == u)
    res = 0;
  else
    res = 1;
  return res;
}

void s_lock_server(int s, struct User *u) {
  if (u->uid == 0) {
    s_update_slots_usage();
    user_locker = USER(0);
    locker_time = get_monotonic_sec();
    snprintf(buff, 255, "lock the task-spooler server by Root\n");
  } else {
    if (user_locker == NULL) {
      user_locker = u;
      locker_time = get_monotonic_sec();
      snprintf(buff, 255, "lock the task-spooler server by [%d] `%s`\n",
               user_locker->uid, u->name);
    } else {
      if (user_locker == u) {
        snprintf(buff, 255,
                 "The task-spooler server has already been locked by [%d] `%s`\n",
                 user_locker->uid, user_locker->name);
      } else {
        snprintf(buff, 255,
                 "Error: the task-spooler server has already been locked by other user [%d] `%s`\n",
                 user_locker->uid, user_locker->name);
      }
    }
  }
  send_list_line(s, buff);
}

void s_unlock_server(int s, struct User *u) {
  if (user_locker == NULL) {
    snprintf(buff, 255, "The task-spooler server has already been unlocked\n");
  } else {
    if (u->uid == 0) {
      user_locker = NULL;
      snprintf(buff, 255, "Unlock the task-spooler server by Root\n");
    } else {
      if (user_locker == u) {
        user_locker = NULL;
        snprintf(buff, 255, "Unlock the task-spooler server by [%d] `%s`\n",
                 u->uid, u->name);
      } else {
        snprintf(buff, 255,
                 "Error: the task-spooler server locked by other user cannot be unlocked by [%d] `%s`\n",
                 u->uid, u->name);
      }
    }
  }
  send_list_line(s, buff);
}

static void s_lock_queue(struct Job *p) {
  if (p->state == QUEUED) {
    p->user->queue--;
    p->state = LOCKED;
    set_state_DB(p->jobid, LOCKED);
  }
}

static void s_unlock_queue(struct Job *p) {
  if (p->state == LOCKED) {
    p->user->queue++;
    p->state = QUEUED;
    set_state_DB(p->jobid, QUEUED);
  }
}

void s_hold_job(int s, int jobid, struct User *u) {
  if (u->max_slots < 0) {
    snprintf(buff, 255, "Error: The owner `%s` is locked\n", u->name);
    send_list_line(s, buff);
    return;
  }
  struct Job *p = findjob(jobid);
  if (p == 0) {
    snprintf(buff, 255, "Error: cannot find job [%d]\n", jobid);
    send_list_line(s, buff);
    return;
  }

  if (p->state == QUEUED) {
    if (p->user == u || u->uid == 0) {
      snprintf(buff, 255, "The queued job [%d] is hold on.\n", jobid);
      s_lock_queue(p);
      send_list_line(s, buff);
      return;
    } else {
      snprintf(buff, 255, "Cannot hold on the queued job [%d].\n", jobid);
      send_list_line(s, buff);
      return;
    }
  }

  if (p->state == LOCKED) {
    snprintf(buff, 255, "The queued job [%d] is already in locked.\n", jobid);
    send_list_line(s, buff);
    return;
  }

  if (p->state == PAUSE) {
    snprintf(buff, 255, "The job [%d] is already in PAUSE.\n", jobid);
    send_list_line(s, buff);
    return;
  }

  if (p->pid != 0 && (p->user == u || u->uid == 0)) {
#ifdef TS_CPU_BIND
    if (cpu_bind_alloc_is_locked()) {
      cpu_bind_defer_pause(s, jobid, u);
      return;
    }
#endif
    if (safe_pause_job(p) == 0) {
      p->state = PAUSE;
      snprintf(buff, 255, "To pause job [%d] successfully!\n", jobid);
    } else {
      snprintf(buff, 255, "Error: cannot pause job [%d] using kill SIGSTOP\n", jobid);
    }
  } else {
    snprintf(buff, 255, "Error: cannot pause job [%d]\n", jobid);
  }
  send_list_line(s, buff);
}

void s_cont_job(int s, int jobid, struct User *u) {
  s_update_slots_usage();
  if (u->max_slots < 0) {
    snprintf(buff, 255, "Error: The owner `%s` is locked\n", u->name);
    send_list_line(s, buff);
    return;
  }
  struct Job *p = findjob(jobid);
  if (p == 0) {
    snprintf(buff, 255, "Error: cannot find job [%d]\n", jobid);
    send_list_line(s, buff);
    return;
  }

  if (p->state == LOCKED) {
    if (p->user == u || u->uid == 0) {
      snprintf(buff, 255, "The locked job [%d] is in queue.\n", jobid);
      s_unlock_queue(p);
      send_list_line(s, buff);
      return;
    } else {
      snprintf(buff, 255, "Cannot unlock the locked job [%d].\n", jobid);
      send_list_line(s, buff);
      return;
    }
  }

  if (p->state == QUEUED) {
    snprintf(buff, 255, "The job [%d] is already in queue.\n", jobid);
    send_list_line(s, buff);
    return;
  }

  p->wall_time = i64abs(p->wall_time);
  if (p->state == RUNNING) {
    if (is_sleep(p) == 0) {
      snprintf(buff, 255, "job [%d] is already in RUNNING.\n", jobid);
    } else {
#ifdef TS_CPU_BIND
      if (cpu_bind_alloc_is_locked()) {
        cpu_bind_defer_continue(s, jobid, u);
        return;
      }
#endif
      cgroups_thaw_job(p);
      snprintf(buff, 255, "job [%d] is continued.\n", jobid);
    }
  } else {
    if (p->pid != 0 && (p->user == u || u->uid == 0)) {
      int num_slots = p->num_slots;
      if (u->busy + num_slots <= u->max_slots &&
          busy_slots + num_slots <= max_slots) {
        if (config_running(p))
          printf("Cannot set Job %i as RUNNING", p->jobid);
        snprintf(buff, 255, "To rerun job [%d] successfully!\n", jobid);
      } else {
        snprintf(buff, 255, "Not enough slots for job [%d], set as time-out wait\n", jobid);
        p->wall_time = -i64abs(p->wall_time);
      }
    } else {
      snprintf(buff, 255, "Error: cannot rerun job [%d]\n", jobid);
    }
  }
  send_list_line(s, buff);
}

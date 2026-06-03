/*
    Task Spooler PLUS - a multi-user job scheduler like slurm.
    Copyright (C) 2007-2026  Kylin JIANG - Lluís Batlle i Rossell

    Please find the license in the provided COPYING file.

    Server-side job display and listing operations.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "cjson/cJSON.h"
#include "main.h"
#include "user.h"
#include "vec.h"
#include "job_ops.h"
#include "jobs.h"
#include "list.h"
#include "info.h"
#include "print.h"
#include "runtime_limit.h"
#include "error.h"
#include "server_user.h"
#include "cgroups.h"
#ifdef TS_CPU_BIND
#include "cpu_bind.h"
#endif
#include "utils.h"

static void send_state(int s, enum Jobstate state) {
  struct Msg m = default_msg();
  m.type = ANSWER_STATE;
  m.u.state = state;
  send_msg(s, &m);
}

static int add_job_to_json_array(struct Job *p, cJSON *jobs) {
  cJSON *job = cJSON_CreateObject();
  if (job == NULL) { error("Error initializing JSON object for job %i.", p->jobid); return 0; }
  cJSON_AddItemToArray(jobs, job);

  cJSON *field;

#define ADD_NUM(key, val) \
  do { field = cJSON_CreateNumber(val); \
       if (field == NULL) return 0; \
       cJSON_AddItemToObject(job, key, field); } while(0)

#define ADD_STR(key, val) \
  do { field = cJSON_CreateStringReference(val); \
       if (field == NULL) return 0; \
       cJSON_AddItemToObject(job, key, field); } while(0)

  ADD_NUM("jobid",     p->jobid);
  ADD_STR("state",     jstate2string(p->state));
  ADD_STR("command",   p->command + p->command_strip);
  ADD_NUM("slots",     p->num_slots);
  if (p->user) {
    ADD_STR("user",    p->user->name);
    ADD_NUM("uid",     p->user->uid);
  } else {
    cJSON_AddItemToObject(job, "user", cJSON_CreateNull());
    cJSON_AddItemToObject(job, "uid", cJSON_CreateNull());
  }

  if (p->label)
    ADD_STR("label",   p->label);
  else
    cJSON_AddItemToObject(job, "label", cJSON_CreateNull());

  if (p->pid)
    ADD_NUM("pid",      p->pid);

  ADD_STR("output",    p->output_filename);

  if (p->work_dir)
    ADD_STR("workdir",  p->work_dir);

  /* dependencies */
  if (p->depend_on && p->depend_on_size > 0) {
    cJSON *deps = cJSON_CreateIntArray(p->depend_on, p->depend_on_size);
    if (deps) cJSON_AddItemToObject(job, "depend_on", deps);
  }

  /* wall-time limit */
  ADD_NUM("wall_time", i64abs(p->wall_time));

  /* CPU binding */
#ifdef TS_CPU_BIND
  if (p->cpu_alloc) {
    struct CpuAlloc *ca = (struct CpuAlloc *)p->cpu_alloc;
    char *cpus = cpu_bind_format_cpus(ca);
    char *mems = cpu_bind_format_mems(ca);
    if (cpus) { ADD_STR("cpu_set", cpus); free(cpus); }
    if (mems) { ADD_STR("numa_mems", mems); free(mems); }
  }
#endif

  /* time fields */
  time_t g_boot = p->info.boot_time;
  if (g_boot == 0)
    g_boot = time(NULL) - get_monotonic_sec();

  ADD_NUM("enqueue_ts", (double)(p->info.enqueue_time + g_boot));
  ADD_NUM("start_ts",   (double)(p->info.start_time + g_boot));
  if (p->info.pause_time != 0)
    ADD_NUM("pause_ts", (double)(p->info.pause_time + g_boot));
  if (p->state == FINISHED)
    ADD_NUM("end_ts",   (double)(p->info.end_time + g_boot));

  /* schedule */
  if (p->schedule_time > 0) {
    time_t boot = time(NULL) - get_monotonic_sec();
    ADD_NUM("schedule_ts", (double)(p->schedule_time + boot));
    time_t left = p->schedule_time - get_monotonic_sec();
    ADD_NUM("schedule_left", left > 0 ? (double)left : 0.0);
  }

  /* work time / elapsed */
  time_t t_work = get_work_time_by_job(p);
  if (t_work > 0) ADD_NUM("work_time", t_work);
  if (p->state == FINISHED && p->result.real_sec > 0)
    ADD_NUM("elapsed_time", p->result.real_sec);
  else if (p->state == RUNNING && p->info.start_time > 0)
    ADD_NUM("elapsed_time", get_monotonic_sec() - p->info.start_time);

  /* pause duration */
  time_t t_pause = p->info.pause_duration;
  if (p->info.pause_time != 0)
    t_pause += get_monotonic_sec() - p->info.pause_time;
  if (t_pause > 0) ADD_NUM("pause_time", t_pause);

  /* finished job info */
  if (p->state == FINISHED) {
    ADD_NUM("exitcode",  p->result.errorlevel);
    ADD_NUM("signal",    p->result.signal);
    ADD_NUM("realtime",  p->result.real_sec);
    ADD_NUM("cputime_us",  p->result.user_sec);
    ADD_NUM("cputime_sys", p->result.system_sec);
  }

  /* SLEEP check */
  if (p->state != FINISHED && p->state != PAUSE && is_sleep(p))
    ADD_NUM("in_sleep", 1);

#undef ADD_NUM
#undef ADD_STR

  return 1;
}

void s_list(int s, struct User *user, enum ListFormat listFormat, int jobid) {
  s_update_slots_usage();

  size_t an = vec_size(&active_jobs);
  size_t fn = vec_size(&finished_jobs);
  char *buffer;

  if (listFormat == DEFAULT) {
    buffer = joblist_headers();
    send_list_line(s, buffer);
    free(buffer);

    for (size_t i = 0; i < an; i++) {
      struct Job *p = (struct Job *)vec_get(&active_jobs, i);
      if (p->state != HOLDING_CLIENT) {
        if (!user || p->user == user || user == USER(0)) {
          buffer = joblist_line(p);
          send_list_line(s, buffer);
          free(buffer);
        }
      }
    }

    if (fn > 0 && an > 0)
      send_list_line(s, "----- Finished -----\n");

    for (size_t i = 0; i < fn; i++) {
      struct Job *p = (struct Job *)vec_get(&finished_jobs, i);
      if (!user || p->user == user || user == USER(0)) {
        buffer = joblist_line(p);
        send_list_line(s, buffer);
        free(buffer);
      }
    }
    if (!user || user == USER(0))
      s_user_status_all(s);
    else
      s_user_status(s, user);
  } else if (listFormat == JSON) {
    cJSON *jobs = cJSON_CreateArray();
    if (jobs == NULL) { error("Error initializing JSON array."); goto end; }

    if (jobid > 0) {
      /* single job lookup */
      struct Job *p = findjob(jobid);
      if (p && p->state != HOLDING_CLIENT) {
        if (add_job_to_json_array(p, jobs) == 0) goto end;
      }
      if (!p) {
        p = find_finished_job(jobid);
        if (p) {
          if (add_job_to_json_array(p, jobs) == 0) goto end;
        }
      }
    } else {
      for (size_t i = 0; i < an; i++) {
        struct Job *p = (struct Job *)vec_get(&active_jobs, i);
        if (p->state != HOLDING_CLIENT) {
          if (add_job_to_json_array(p, jobs) == 0) goto end;
        }
      }
      for (size_t i = 0; i < fn; i++) {
        struct Job *p = (struct Job *)vec_get(&finished_jobs, i);
        if (add_job_to_json_array(p, jobs) == 0) goto end;
      }
    }

    buffer = cJSON_PrintUnformatted(jobs);
    if (buffer == NULL) { error("Error converting jobs to JSON."); goto end; }

    size_t buffer_strlen = strlen(buffer);
    char *newbuf = realloc(buffer, buffer_strlen + 2);
    if (newbuf == NULL) { free(buffer); buffer = NULL; goto end; }
    buffer = newbuf;
    strcat(buffer, "\n");
    send_list_line(s, buffer);

  end:
    cJSON_Delete(jobs);
    free(buffer);
  } else if (listFormat == TAB) {
    for (size_t i = 0; i < an; i++) {
      struct Job *p = (struct Job *)vec_get(&active_jobs, i);
      if (p->state != HOLDING_CLIENT) {
        buffer = joblist_line_plain(p);
        send_list_line(s, buffer);
        free(buffer);
      }
    }
    for (size_t i = 0; i < fn; i++) {
      struct Job *p = (struct Job *)vec_get(&finished_jobs, i);
      buffer = joblist_line_plain(p);
      send_list_line(s, buffer);
      free(buffer);
    }
  }
}

void s_list_all(int s, enum ListFormat listFormat) {
  char *buffer;
  buffer = joblist_headers();
  send_list_line(s, buffer);
  free(buffer);

  size_t an = vec_size(&active_jobs);
  size_t fn = vec_size(&finished_jobs);

  for (size_t i = 0; i < an; i++) {
    struct Job *p = (struct Job *)vec_get(&active_jobs, i);
    if (p->state != HOLDING_CLIENT) {
      buffer = joblist_line(p);
      send_list_line(s, buffer);
      free(buffer);
    }
  }

  if (fn > 0 && an > 0)
    send_list_line(s, "\n ----- Finished -----\n");

  for (size_t i = 0; i < fn; i++) {
    struct Job *p = (struct Job *)vec_get(&finished_jobs, i);
    buffer = joblist_line(p);
    send_list_line(s, buffer);
    free(buffer);
  }
}

void s_job_info(int s, int jobid) {
  struct Job *p = 0;
  struct Msg m = default_msg();

  if (jobid == -1) {
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
    snprintf(buff, 255, "[s_job_info] Job %i not finished or not running.\n", jobid);
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
  const char *status = "";
  if (p->state != FINISHED) {
    if (p->state != PAUSE && is_sleep(p) == 1)
      status = " in SLEEP!";
  }
  write(s, p->command + p->command_strip, strlen(p->command + p->command_strip));
  fd_nprintf(s, 100, "\n");
  fd_nprintf(s, 100, "User: %s [%d]\n", p->user->name, p->user->uid);
  fd_nprintf(s, 100, "State: %9s PID: %-6d%s\n", jstate2string(p->state), p->pid, status);
#ifdef TS_CPU_BIND
  if (p->cpu_alloc) {
      struct CpuAlloc *ca = (struct CpuAlloc *)p->cpu_alloc;
      char *cpus = cpu_bind_format_cpus(ca);
      char *mems = cpu_bind_format_mems(ca);
      fd_nprintf(s, 200, "Slots: %-3d  CPU: %s  NUMA: %s\n",
                 p->num_slots,
                 cpus ? cpus : "(none)",
                 mems ? mems : "(none)");
      free(cpus);
      free(mems);
  } else {
      fd_nprintf(s, 100, "Slots: %-3d  CPU: free\n", p->num_slots);
  }
#else
  fd_nprintf(s, 100, "Slots: %-3d\n", p->num_slots);
#endif
  if (p->output_filename != NULL) {
    fd_nprintf(s, strlen(p->output_filename) + 30, "Ouput: %s\n", p->output_filename);
  } else {
    fd_nprintf(s, strlen(p->work_dir) + 30, "Workdir: %s\n", p->work_dir);
  }

  time_t g_boot_wallclock = p->info.boot_time;
  if (g_boot_wallclock == 0) {
    g_boot_wallclock = time(NULL) - get_monotonic_sec();
  }
  time_t ct = p->info.enqueue_time + g_boot_wallclock;
  fd_nprintf(s, 100, "Enqueue time: %s", ctime(&ct));
  ct = p->info.start_time + g_boot_wallclock;
  fd_nprintf(s, 100, "Start time: %s", ctime(&ct));
  if (p->info.pause_time != 0) {
    ct = p->info.pause_time + g_boot_wallclock;
    fd_nprintf(s, 100, "Pause time: %s", ctime(&ct));
  }
  if (p->state == FINISHED) {
    ct = p->info.end_time + g_boot_wallclock;
    fd_nprintf(s, 100, "End time: %s", ctime(&ct));
  }

  if (p->schedule_time > 0) {
      time_t boot = time(NULL) - get_monotonic_sec();
      time_t wall = p->schedule_time + boot;
      struct tm *tm = localtime(&wall);
      char datebuf[64];
      strftime(datebuf, sizeof(datebuf), "%c", tm);
      time_t left = p->schedule_time - get_monotonic_sec();
      if (left > 0) {
          time_repr_t dr = format_time(left);
          fd_nprintf(s, 100, "Schedule: %s (in %.2f%c)\n", datebuf, dr.value, dr.unit);
      } else {
          fd_nprintf(s, 100, "Schedule: %s (now)\n", datebuf);
      }
  }

  time_t t_wall = i64abs(p->wall_time);
  time_repr_t r = format_time(t_wall);
  fd_nprintf(s, 100, "Wall-time: %.4f %c\n----\n", r.value, r.unit);

  time_t t_work = get_work_time_by_job(p);
  if (t_work > 0) {
      r = format_time(t_work);
      fd_nprintf(s, 100, "Work time: %.4f %c\n", r.value, r.unit);
  }

  time_t t_pause = get_pause_time_by_job(p);
  time_t t_real = t_pause + t_work;
  if (t_real > 0) {
      r = format_time(t_pause);
      fd_nprintf(s, 100, "Pause time: %.4f %c\n", r.value, r.unit);
      r = format_time(t_real);
      fd_nprintf(s, 100, "Elapsed time: %.4f %c\n", r.value, r.unit);
  }

  if (p->state == FINISHED) {
    struct Result *res = &(p->result);
    fd_nprintf(s, 100, "Error: %d Signal: %d Die: %d\n", res->errorlevel, res->signal, res->died_by_signal);
  }
}

void s_get_label(int s, int jobid) {
  struct Job *p = 0;
  char *label;

  if (jobid == -1) {
    size_t n = vec_size(&active_jobs);
    if (n > 0) p = (struct Job *)vec_get(&active_jobs, n - 1);
    if (p == 0) {
      n = vec_size(&finished_jobs);
      if (n > 0) p = (struct Job *)vec_get(&finished_jobs, n - 1);
    }
  } else {
    p = get_job(jobid);
  }

  if (p == 0) {
    snprintf(buff, 255, "[get_label] Job %i not finished or not running.\n", jobid);
    send_list_line(s, buff);
    return;
  }

  if (p->label) {
    label = (char *)malloc(strlen(p->label) + 1);
    sprintf(label, "%s\n", p->label);
  } else {
    label = "";
  }
  send_list_line(s, label);
  if (p->label) free(label);
}

void s_send_cmd(int s, int jobid) {
  struct Job *p = 0;
  char *cmd;

  if (jobid == -1) {
    size_t n = vec_size(&active_jobs);
    if (n > 0) p = (struct Job *)vec_get(&active_jobs, n - 1);
    if (p == 0) {
      n = vec_size(&finished_jobs);
      if (n > 0) p = (struct Job *)vec_get(&finished_jobs, n - 1);
    }
  } else {
    p = get_job(jobid);
  }

  if (p == 0) {
    snprintf(buff, 255, "[get_cmd] Job %i not finished or not running.\n", jobid);
    send_list_line(s, buff);
    return;
  }
  cmd = (char *)malloc(strlen(p->command) + 1);
  sprintf(cmd, "%s\n", p->command);
  send_list_line(s, cmd);
  free(cmd);
}

void s_send_state(int s, int jobid) {
  struct Job *p = 0;

  if (jobid == -1) {
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
    snprintf(buff, 255, "The job %i cannot be stated.\n", jobid);
    send_list_line(s, buff);
    return;
  }

  send_state(s, p->state);
}

void s_send_output(int s, int jobid) {
  struct Job *p = 0;
  struct Msg m = default_msg();

  if (jobid == -1) {
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
    if (p != 0 && p->state != RUNNING && p->state != FINISHED && p->state != SKIPPED
        && p->state != PAUSE)
      p = 0;
    /* Thaw PAUSEd jobs so signals can be delivered */
    if (p != 0 && p->state == PAUSE) {
      cgroups_thaw_job(p);
    }
  }

  if (p == 0) {
    if (jobid == -1)
      snprintf(buff, 255, "The last job has not finished or is not running.\n");
    else
      snprintf(buff, 255, "[s_send_output] Job %i not finished or not running.\n", jobid);
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

void s_sort_jobs(void) {
  size_t n = vec_size(&active_jobs);
  vec_t running;
  vec_t other;
  vec_init(&running);
  vec_init(&other);

  for (size_t i = 0; i < n; i++) {
    struct Job *p = (struct Job *)vec_get(&active_jobs, i);
    if (p->state == RUNNING)
      vec_push(&running, p);
    else
      vec_push(&other, p);
  }

  vec_clear(&active_jobs);
  for (size_t i = 0; i < vec_size(&running); i++)
    vec_push(&active_jobs, vec_get(&running, i));
  for (size_t i = 0; i < vec_size(&other); i++)
    vec_push(&active_jobs, vec_get(&other, i));

  vec_destroy(&running);
  vec_destroy(&other);
}

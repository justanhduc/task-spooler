/*
    Task Spooler - a task queue system for the unix user
    Copyright (C) 2007-2013  Lluís Batlle i Rossell

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
  field = cJSON_CreateNumber(p->jobid);
  if (field == NULL) { error("Error initializing JSON field ID for job %i.", p->jobid); return 0; }
  cJSON_AddItemToObject(job, "ID", field);

  const char *state_string = jstate2string(p->state);
  field = cJSON_CreateStringReference(state_string);
  if (field == NULL) { error("Error initializing JSON field State for job %i.", p->jobid); return 0; }
  cJSON_AddItemToObject(job, "State", field);

  field = cJSON_CreateNumber(p->num_slots);
  if (field == NULL) { error("Error initializing JSON field Proc for job %i.", p->jobid); return 0; }
  cJSON_AddItemToObject(job, "Proc.", field);

  field = cJSON_CreateStringReference(p->user->name);
  if (field == NULL) { error("Error initializing JSON field User for job %i.", p->jobid); return 0; }
  cJSON_AddItemToObject(job, "User", field);

  if (p->label != NULL)
    field = cJSON_CreateStringReference(p->label);
  else
    field = cJSON_CreateNull();
  if (field == NULL) { error("Error initializing JSON field Label for job %i.", p->jobid); return 0; }
  cJSON_AddItemToObject(job, "Label", field);

  field = cJSON_CreateStringReference(p->output_filename);
  if (field == NULL) { error("Error initializing JSON field Output for job %i.", p->jobid); return 0; }
  cJSON_AddItemToObject(job, "Output", field);

  if (p->state == FINISHED)
    field = cJSON_CreateNumber(p->result.errorlevel);
  else
    field = cJSON_CreateNull();
  if (field == NULL) { error("Error initializing JSON field E-Level for job %i.", p->jobid); return 0; }
  cJSON_AddItemToObject(job, "E-Level", field);

  if (p->state == FINISHED) {
    field = cJSON_CreateNumber(p->result.real_sec);
    if (field == NULL) { error("Error initializing JSON field Time for job %i.", p->jobid); return 0; }
  } else {
    field = cJSON_CreateNull();
    if (field == NULL) { error("Error initializing JSON field Time for job %i.", p->jobid); return 0; }
  }
  cJSON_AddItemToObject(job, "Time_ms", field);

  field = cJSON_CreateStringReference(p->command + p->command_strip);
  if (field == NULL) { error("Error initializing JSON field Command for job %i.", p->jobid); return 0; }
  cJSON_AddItemToObject(job, "Command", field);

  return 1;
}

void s_list(int s, int ts_UID, enum ListFormat listFormat) {
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
        if (p->user == USER(ts_UID) || ts_UID == 0) {
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
      if (p->user == USER(ts_UID) || ts_UID == 0) {
        buffer = joblist_line(p);
        send_list_line(s, buffer);
        free(buffer);
      }
    }
    if (ts_UID == 0)
      s_user_status_all(s);
    else
      s_user_status(s, ts_UID);
  } else if (listFormat == JSON) {
    cJSON *jobs = cJSON_CreateArray();
    if (jobs == NULL) { error("Error initializing JSON array."); goto end; }

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
  fd_nprintf(s, 100, "Slots: %-3d\n", p->num_slots);
  if (p->output_filename != NULL) {
    fd_nprintf(s, strlen(p->output_filename) + 30, "Ouput: %s\n", p->output_filename);
  } else {
    fd_nprintf(s, strlen(p->work_dir) + 30, "Workdir: %s\n", p->work_dir);
  }
  if (p->email)
    fd_nprintf(s, 100, "Email: %s\n", p->email);

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

  time_t t_wall = i64abs(p->wall_time);
  time_repr_t r = format_time(t_wall);
  fd_nprintf(s, 100, "Wall-time: %.4f %c\n----\n", r.value, r.unit);
  time_t t_work = get_work_time_by_job(p);
  r = format_time(t_work);
  fd_nprintf(s, 100, "Work time: %.4f %c\n", r.value, r.unit);

  time_t t_pause = get_pause_time_by_job(p);
  time_t t_real = t_pause + t_work;
  double p_rate = (double)(t_pause) / t_real;
  if (p_rate > 0.05 || 1) {
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
    if (p != 0 && p->state != RUNNING && p->state != FINISHED && p->state != SKIPPED)
      p = 0;
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

/*
    Task Spooler - a task queue system for the unix user
    Copyright (C) 2007-2009  Lluís Batlle i Rossell

    Please find the license in the provided COPYING file.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "main.h"
#include "user.h"
#include "list.h"
#include "jobs.h"
#include "runtime_limit.h"
#include "cgroups.h"
#include "error.h"

/* return 0 for running and 1 for sleep and -1 for error */
int is_sleep(const struct Job* p) {
    int pid = p->pid;  
    if (pid <= 0) return -1;
    if (cgroups_is_frozen(p) == 1) {
      return 1;
    }

    char filename[256];
    char status;

    snprintf(filename, sizeof(filename), "/proc/%d/stat", pid);

    FILE *fp = fopen(filename, "r");
    if (!fp) {
        return -1;
    }

    int ret = fscanf(fp, "%*d (%*[^)]) %c", &status);

    fclose(fp);

    if (ret != 1) {
        return -1;
    }

    return (status == 'T' || status == 't');
}

static char *shorten(char *line, int len) {
  char *newline = (char *)malloc((len + 1) * sizeof(char));
  if (strlen(line) <= len)
    strcpy(newline, line);
  else {
    snprintf(newline, len - 4, "%s", line);
    strcat(newline, "...");
  }
  return newline;
}

char *joblistdump_headers() {
  char *line;

  line = malloc(600);
  snprintf(line, 600,
           "#!/bin/sh\n# - task spooler (ts) job dump\n"
           "# This file has been created because a SIGTERM killed\n"
           "# your queue server.\n"
           "# The finished commands are listed first.\n"
           "# The commands running or to be run are stored as you would\n"
           "# probably run them. Take care - some quotes may have got"
           " broken\n\n");

  return line;
}

char *joblist_headers() {
  char *line;
  char extra[100] = "";
  if (user_locker != NULL) {
    time_t dt = get_monotonic_sec() - locker_time;
    time_repr_t r = format_time(dt);

    snprintf(extra, 100, "Locked by `%s` for %3.2f%c.",
             user_locker->name, r.value, r.unit);
  }

  line = malloc(256);
  snprintf(line, 256,
           "%-4s %-9s %-6s %-7s %-10s %7s  %-20s  Log [run=%i/%i %.2f%%] %s\n",
           "ID", "State", "Proc.", "User", "Label", "Time", "Command",
           busy_slots, max_slots, 100.0 * busy_slots / max_slots, extra);
  return line;
}

static int max(int a, int b) { return a > b ? a : b; }

static const char* jstate2string_result(const struct Job* p) {
  if (p->result.errorlevel != 0 || p->result.signal != 0 || p->result.died_by_signal != 0) {
    return "failed";
  } else {
    return jstate2string(p->state);
  }
}

static const char *ofilename_shown(const struct Job *p) {
  const char *output_filename;

  /* Scheduled/delayed job: show start time + remaining */
  if (p->schedule_time > 0 && p->state == QUEUED) {
      static char timebuf[96];
      time_t now = get_monotonic_sec();
      time_t boot = time(NULL) - now;
      time_t wall = p->schedule_time + boot;
      struct tm *tm = localtime(&wall);
      char datebuf[64];
      strftime(datebuf, sizeof(datebuf), "%c", tm);
      time_t left = p->schedule_time - now;
      if (left > 0) {
          time_repr_t r = format_time(left);
          snprintf(timebuf, sizeof(timebuf), "Run at %s, after %.2f%c",
                   datebuf, r.value, r.unit);
      } else {
          snprintf(timebuf, sizeof(timebuf), "Run at %s", datebuf);
      }
      return timebuf;
  }

  if (p->state == SKIPPED) {
    output_filename = "(no output)";
  } else if (p->store_output) {
    if (p->state == QUEUED) {
      output_filename = "(file)";
    } else {
      if (p->output_filename == 0)
        /* This may happen due to concurrency
         * problems */
        output_filename = "(...)";
      else
        output_filename =
            p->output_filename; // shorten(p->output_filename, 20);
    }
  } else
    output_filename = "stdout";

  return output_filename;
}

static char *print_noresult(const struct Job *p) {
  const char *jobstate;
  const char *output_filename;
  int maxlen;
  char *line;
  /* 20 chars should suffice for a string like "[int,int,..]&& " */
  char dependstr[1024] = "[]";
  int cmd_len;
  jobstate = jstate2string(p->state);

  if (p->state == RUNNING) {
    if (p->pid == 0) {
      jobstate = "N/A";
    } else {
      if (is_sleep(p) == 1) {
        jobstate = "sleep  ";
      }
    }
  }

  if (p->state == PAUSE && is_sleep(p) == 1) {
    if (p->wall_time < 0) {
      jobstate = "timeout"; // TODO delete this
    } else {
      jobstate = "pause  "; // TODO delete this
    }
  }

  if (p->schedule_time > 0 && p->state == QUEUED)
      jobstate = "Wait   ";

  output_filename = ofilename_shown(p);

  char *uname = (p->user != NULL)
                    ? p->user->name : "???";
  maxlen = 4 + 1 + 10 + 1 + 20 + 1 + 8 + 1 + 25 + 1 + strlen(p->command) + 20 +
           strlen(uname) + 240 +
           strlen(output_filename); /* 20 is the margin for errors */

  if (p->label)
    maxlen += 3 + strlen(p->label);
  if (p->depend_on_size) {
    maxlen += sizeof(dependstr);
    int pos = 0;
    if (p->depend_on[0] == -1)
      pos += snprintf(&dependstr[pos], sizeof(dependstr), "[ ");
    else
      pos +=
          snprintf(&dependstr[pos], sizeof(dependstr), "[%i", p->depend_on[0]);

    for (int i = 1; i < p->depend_on_size; i++) {
      if (p->depend_on[i] == -1)
        pos += snprintf(&dependstr[pos], sizeof(dependstr), ", ");
      else
        pos += snprintf(&dependstr[pos], sizeof(dependstr), ",%i",
                        p->depend_on[i]);
    }
    pos += snprintf(&dependstr[pos], sizeof(dependstr), "]");
  }

  time_t t_real;
  time_repr_t r = {0, 's'};
  char buf[128] = " ";
  if (p->state == QUEUED || p->pid == 0) {
    t_real = 0;
  } else {
    time_t t_pause = get_pause_time_by_job(p);
    t_real = get_work_time_by_job(p) + t_pause;
    r = format_time(t_real);
    double p_rate = 100.0 * (double)(t_pause) / t_real;
    if (p_rate < 5) {
        snprintf(buf, sizeof(buf), "%c", r.unit);
    } else {
      snprintf(buf, sizeof(buf), "%c (%.0f%%)", r.unit, p_rate);
    }
  }

  line = (char *)malloc(maxlen);
  if (line == NULL)
    error("Malloc for %i failed.\n", maxlen);

  cmd_len = max((strlen(p->command) + (term_width - maxlen)), 20);
  char *cmd = shorten(p->command + p->command_strip, cmd_len);
  if (p->label) {
    char *label = shorten(p->label, 10); 
    snprintf(line, maxlen, "%-4i %-9s %-6i %-7s %-10s %6.2f%s  %-21s | %s\n",
             p->jobid, jobstate, p->num_slots, uname, label, r.value, buf, cmd,
             output_filename);
    free(label);
    free(cmd);
  } else {
    char *cmd = shorten(p->command + p->command_strip, cmd_len);
    char *label = "(..)";
    snprintf(line, maxlen, "%-4i %-9s %-6i %-7s %-10s %6.2f%s  %-21s | %s\n",
             p->jobid, jobstate, p->num_slots, uname, label, r.value, buf, cmd,
             output_filename);
    free(cmd);
  }
  return line;
}

static char *print_result(const struct Job *p) {
  const char *jobstate;
  int maxlen;
  char *line;
  const char *output_filename;
  /* 20 chars should suffice for a string like "[int,int,..]&& " */
  char dependstr[1024] = "[]&&";
  time_t real_sec = p->result.real_sec;
  if (real_sec == 0.0) {
    real_sec = p->info.end_time - p->info.start_time; // TODO
  }

  time_repr_t r = format_time(real_sec);
  int cmd_len;

  jobstate = jstate2string_result(p);
  output_filename = ofilename_shown(p);

  char *uname = (p->user != NULL)
                    ? p->user->name : "???";
  maxlen = 4 + 1 + 10 + 1 + 20 + 1 + 8 + 1 + 25 + 1 + strlen(p->command) + 20 +
           strlen(uname) + 240 +
           strlen(output_filename); /* 20 is the margin for errors */

  if (p->label)
    maxlen += 3 + strlen(p->label);
  if (p->depend_on_size) {
    maxlen += sizeof(dependstr);
    int pos = 0;
    if (p->depend_on[0] == -1)
      pos += snprintf(&dependstr[pos], sizeof(dependstr), "[ ");
    else
      pos +=
          snprintf(&dependstr[pos], sizeof(dependstr), "[%i", p->depend_on[0]);

    for (int i = 1; i < p->depend_on_size; i++) {
      if (p->depend_on[i] == -1)
        pos += snprintf(&dependstr[pos], sizeof(dependstr), ", ");
      else
        pos += snprintf(&dependstr[pos], sizeof(dependstr), ",%i",
                        p->depend_on[i]);
    }
    pos += snprintf(&dependstr[pos], sizeof(dependstr), "]&& ");
  }

  line = (char *)malloc(maxlen);
  if (line == NULL)
    error("Malloc for %i failed.\n", maxlen);

  cmd_len = max((strlen(p->command) + (term_width - maxlen)), 20);
  char *cmd = shorten(p->command + p->command_strip, cmd_len);
  if (p->label) {
    char *label = shorten(p->label, 10);
    snprintf(line, maxlen, "%-4i %-9s %-6i %-7s %-10s %6.2f%c  %-21s | %s\n",
             p->jobid, jobstate, p->num_slots, uname, label, r.value, r.unit, cmd,
             output_filename);
    free(label);
    free(cmd);
  } else {
    char *cmd = shorten(p->command + p->command_strip, cmd_len);
    char *label = "(..)";
    snprintf(line, maxlen, "%-4i %-9s %-6i %-7s %-10s %6.2f%c  %-21s | %s\n",
             p->jobid, jobstate, p->num_slots, uname, label, r.value, r.unit, cmd,
             output_filename);
    free(cmd);
  }

  return line;
}

static char *plainprint_noresult(const struct Job *p) {
  const char *jobstate;
  const char *output_filename;
  int maxlen;
  char *line;
  /* 20 chars should suffice for a string like "[int,int,..]&& " */
  char dependstr[256] = "[]&&";

  jobstate = jstate2string(p->state);
  if (p->schedule_time > 0 && p->state == QUEUED)
      jobstate = "Wait   ";
  output_filename = ofilename_shown(p);
  char *uname = (p->user != NULL)
                    ? p->user->name : "???";
  maxlen = 4 + 1 + 10 + 1 + 20 + 1 + 8 + 1 + 25 + 1 + strlen(p->command) + 20 +
           strlen(uname) + 2; /* 20 is the margin for errors */

  char* label = "(..)";
  if (p->label) {
    label = p->label;
  }
  maxlen += 3 + strlen(label);

  if (p->depend_on_size) {
    maxlen += sizeof(dependstr);
    int pos = 0;
    if (p->depend_on[0] == -1)
      pos += snprintf(&dependstr[pos], sizeof(dependstr), "[ ");
    else
      pos +=
          snprintf(&dependstr[pos], sizeof(dependstr), "[%i", p->depend_on[0]);

    for (int i = 1; i < p->depend_on_size; i++) {
      if (p->depend_on[i] == -1)
        pos += snprintf(&dependstr[pos], sizeof(dependstr), ", ");
      else
        pos += snprintf(&dependstr[pos], sizeof(dependstr), ",%i",
                        p->depend_on[i]);
    }
    pos += snprintf(&dependstr[pos], sizeof(dependstr), "]&& ");
  }

  line = (char *)malloc(maxlen);
  if (line == NULL)
    error("Malloc for %i failed.\n", maxlen);
  
  time_t t_pause = get_pause_time_by_job(p);
  time_t t_real = get_work_time_by_job(p) + t_pause;
  double p_rate = 100.0 * (double)(t_pause) / t_real;
  time_repr_t r = format_time(t_real);
  char buf[128] = "";
  if (p_rate < 5) {
    snprintf(buf, sizeof(buf), "%c", r.unit);
  } else {
    snprintf(buf, sizeof(buf), "%c (%.0f%%)", r.unit, p_rate);
  }
  // printf("get runtime %.3f sec for %d\n", runtime, p->pid);
    /*
    if (rate < 80 && is_timeout == 0) {
      sprintf(buf, "%s %d%%", unit, rate);
    } else {
      sprintf(buf, "%s", unit);
    }
    */
  
  snprintf(line, maxlen, "%i\t%s\t%d\t%s\t%s\t%i\t%.2f%s\t%s\t%s\t%s\n", 
    p->jobid, jobstate, p->num_slots, USER(p->user)->name, label,
    p->result.errorlevel, r.value, buf, p->command + p->command_strip,
    dependstr, output_filename);

  return line;
}


static char *plainprint_result(const struct Job *p) {
  const char *jobstate;
  int maxlen;
  char *line;
  const char *output_filename;
  /* 20 chars should suffice for a string like "[int,int,..]&& " */
  char dependstr[256] = "[]";
  time_t real_sec = p->result.real_sec;

  if (real_sec == 0.0) {
    real_sec = p->info.end_time - p->info.start_time; // TODO
  }
  time_repr_t r = format_time(real_sec);

  jobstate = jstate2string_result(p);
  output_filename = ofilename_shown(p);

  maxlen = 4 + 1 + 10 + 1 + 20 + 1 + 8 + 1 + 25 + 1 + strlen(p->command) +
           30 + strlen(USER(p->user)->name); /* 30 is the margin for errors */

  
  char* label = "(..)";
  if (p->label) {
    label = p->label;
  }
  maxlen += 3 + strlen(label);

  if (p->depend_on_size) {
    maxlen += sizeof(dependstr);
    int pos = 0;
    if (p->depend_on[0] == -1)
      pos += snprintf(&dependstr[pos], sizeof(dependstr), "[ ");
    else
      pos += snprintf(&dependstr[pos], sizeof(dependstr), "[%i", p->depend_on[0]);

    for (int i = 1; i < p->depend_on_size; i++) {
      if (p->depend_on[i] == -1)
        pos += snprintf(&dependstr[pos], sizeof(dependstr), ", ");
      else
        pos += snprintf(&dependstr[pos], sizeof(dependstr), ",%i", p->depend_on[i]);
    }
    pos += snprintf(&dependstr[pos], sizeof(dependstr), "]");
  }

  line = (char *)malloc(maxlen);
  if (line == NULL)
    error("Malloc for %i failed.\n", maxlen);

  snprintf(line, maxlen, "%i\t%s\t%d\t%s\t%s\t%i\t%.2f%c\t%s\t%s\t%s\n", 
    p->jobid, jobstate, p->num_slots, USER(p->user)->name, label,
    p->result.errorlevel, r.value, r.unit, p->command + p->command_strip,
    dependstr, output_filename);
  return line;
}

char *joblist_line(const struct Job *p) {
  char *line;

  if (p->state == FINISHED)
    line = print_result(p);
  else
    line = print_noresult(p);

  return line;
}

char *joblist_line_plain(const struct Job *p) {
  char *line;

  if (p->state == FINISHED)
    line = plainprint_result(p);
  else
    line = plainprint_noresult(p);

  return line;
}

char *joblistdump_torun(const struct Job *p) {
  int maxlen;
  char *line;

  maxlen = 10 + strlen(p->command) + 20; /* 20 is the margin for errors */

  line = (char *)malloc(maxlen);
  if (line == NULL)
    error("Malloc for %i failed.\n", maxlen);

  snprintf(line, maxlen, "%s\n", p->command);

  return line;
}

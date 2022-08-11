/*
    Task Spooler - a task queue system for the unix user
    Copyright (C) 2007-2009  Lluís Batlle i Rossell

    Please find the license in the provided COPYING file.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "main.h"
#include "user.h"

/* From jobs.c */
extern int busy_slots;
extern int max_slots;

static int check_ifsleep(int pid) {
  char filename[256];
  char name[256];
  char status = '\0';
  FILE *fp = NULL;
  snprintf(filename, 256, "/proc/%d/stat", pid);

  fp = fopen(filename, "r");
  if (fp == NULL) {
    fprintf(stderr, "Error: Couldn't open [%s]\n", filename);
    return -1;
  }
  int token = fscanf(fp, "%d %s %c", &pid, name, &status);
  if (token < 3) {
    return -1;
  }
  fclose(fp);

  if (status == 'T') {
    return 1;
  } else {
    return 0;
  }
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

  line = malloc(100);
  snprintf(line, 100,
           "%-4s %-7s %-7s %-6s %-10s %6s  %-20s   %s [run=%i/%i %.2f%%]\n",
           "ID", "State", "Proc.", "User", "Label", "Time", "Command", "Log",
           busy_slots, max_slots, 100.0 * busy_slots / max_slots);

  return line;
}

static int max(int a, int b) { return a > b ? a : b; }

static const char *ofilename_shown(const struct Job *p) {
  const char *output_filename;

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
  char dependstr[20] = "";
  int cmd_len;

  jobstate = jstate2string(p->state);
  if (p->pid != 0 && check_ifsleep(p->pid) == 1) {
    jobstate = "holdon";
  }
  output_filename = ofilename_shown(p);

  char *uname = user_name[p->user_id];
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

  struct timeval starttv = p->info.start_time;
  struct timeval endtv;
  float real_ms;
  char *unit;
  if (p->state == QUEUED || p->pid == 0) {
    real_ms = 0;
    unit = " ";
  } else {
    gettimeofday(&endtv, NULL);
    real_ms = endtv.tv_sec - starttv.tv_sec +
              ((float)(endtv.tv_usec - starttv.tv_usec) / 1000000.);
    unit = time_rep(&real_ms);
  }

  line = (char *)malloc(maxlen);
  if (line == NULL)
    error("Malloc for %i failed.\n", maxlen);

  cmd_len = max((strlen(p->command) + (term_width - maxlen)), 20);
  char *cmd = shorten(p->command, cmd_len);
  if (p->label) {
    char *label = shorten(p->label, 10);
    snprintf(line, maxlen,
             "%-4i %-10s %-3i %-7s %-10s %5.2f%s  %-20s %d | %s\n", p->jobid,
             jobstate, p->num_slots, uname, label, real_ms, unit, cmd, p->pid,
             output_filename);
    free(label);
    free(cmd);
  } else {
    char *cmd = shorten(p->command, cmd_len);
    char *label = "(..)";
    snprintf(line, maxlen, "%-4i %-10s %-3i %-7s %-10s %5.2f%s  %-20s | %s\n",
             p->jobid, jobstate, p->num_slots, uname, label, real_ms, unit, cmd,
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
  char dependstr[20] = "";
  float real_ms = p->result.real_ms;
  char *unit = time_rep(&real_ms);
  int cmd_len;

  jobstate = jstate2string(p->state);
  output_filename = ofilename_shown(p);

  char *uname = user_name[p->user_id];
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
  char *cmd = shorten(p->command, cmd_len);
  if (p->label) {
    char *label = shorten(p->label, 10);
    snprintf(line, maxlen, "%-4i %-10s %-3i %-7s %-10s %5.2f%s  %-20s | %s\n",
             p->jobid, jobstate, p->num_slots, uname, label, real_ms, unit, cmd,
             output_filename);
    free(label);
    free(cmd);
  } else {
    char *cmd = shorten(p->command, cmd_len);
    char *label = "(..)";
    snprintf(line, maxlen, "%-4i %-10s %-3i %-7s %-10s %5.2f%s  %-20s | %s\n",
             p->jobid, jobstate, p->num_slots, uname, label, real_ms, unit, cmd,
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
  char dependstr[20] = "";

  jobstate = jstate2string(p->state);
  output_filename = ofilename_shown(p);
  char *uname = user_name[p->user_id];
  maxlen = 4 + 1 + 10 + 1 + 20 + 1 + 8 + 1 + 25 + 1 + strlen(p->command) + 20 +
           strlen(uname) + 2; /* 20 is the margin for errors */

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

  if (p->label)
    snprintf(line, maxlen, "%i\t%s\t%s\t%s\t%s\t%s\t[%s]\t%s\n", p->jobid,
             jobstate, output_filename, "", "", dependstr, p->label,
             p->command);
  else
    snprintf(line, maxlen, "%i\t%s\t%s\t%s\t%s\t%s\t\t%s\n", p->jobid, jobstate,
             output_filename, "", "", dependstr, p->command);

  return line;
}

static char *plainprint_result(const struct Job *p) {
  const char *jobstate;
  int maxlen;
  char *line;
  const char *output_filename;
  /* 20 chars should suffice for a string like "[int,int,..]&& " */
  char dependstr[20] = "";
  float real_ms = p->result.real_ms;
  char *unit = time_rep(&real_ms);

  jobstate = jstate2string(p->state);
  output_filename = ofilename_shown(p);

  maxlen = 4 + 1 + 10 + 1 + 20 + 1 + 8 + 1 + 25 + 1 + strlen(p->command) +
           20; /* 20 is the margin for errors */

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

  if (p->label)
    snprintf(line, maxlen, "%i\t%s\t%s\t%i\t%.2f\t%s\t%s\t[%s]\t%s\n", p->jobid,
             jobstate, output_filename, p->result.errorlevel, real_ms, unit,
             dependstr, p->label, p->command);
  else
    snprintf(line, maxlen, "%i\t%s\t%s\t%i\t%.2f\t%s\t%s\t\t%s\n", p->jobid,
             jobstate, output_filename, p->result.errorlevel, real_ms, unit,
             dependstr, p->command);

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

  snprintf(line, maxlen, "ts %s\n", p->command);

  return line;
}

char *time_rep(float *t) {
  float time_in_sec = *t;
  char *unit = "s";
  if (time_in_sec > 60) {
    time_in_sec /= 60;
    unit = "m";

    if (time_in_sec > 60) {
      time_in_sec /= 60;
      unit = "h";

      if (time_in_sec > 24) {
        time_in_sec /= 24;
        unit = "d";
      }
    }
  }
  *t = time_in_sec;
  return unit;
}

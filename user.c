
#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "main.h"
#include "user.h"

void send_list_line(int s, const char *str);
void error(const char *str, ...);

const char *get_user_path() {
  char *str;
  str = getenv("TS_USER_PATH");
  if (str == NULL || strlen(str) == 0) {
    return "/home/kylin/task-spooler/user.txt";
  } else {
    return str;
  }
}

int get_env(const char *env, int v0) {
  char *str;
  str = getenv(env);
  if (str == NULL || strlen(str) == 0) {
    return v0;
  } else {
    int i = atoi(str);
    if (i < 0)
      i = v0;
    return i;
  }
}

long str2int(const char *str) {
  long i;
  if (sscanf(str, "%ld", &i) == 0) {
    printf("Error in convert %s to number\n", str);
    exit(-1);
  }
  return i;
}

const char *set_server_logfile() {
  logfile_path = getenv("TS_LOGFILE_PATH");
  if (logfile_path == NULL || strlen(logfile_path) == 0) {
    logfile_path = "/home/kylin/task-spooler/log.txt";
  }
  return logfile_path;
}

void write_logfile(const struct Job *p) {
  // char buf[1024] = "";
  FILE *f = fopen(logfile_path, "a");
  if (f == NULL) {
    return;
  }
  char buf[100];
  time_t now = time(0);
  strftime(buf, 100, "%Y-%m-%d %H:%M:%S", localtime(&now));
  // snprintf(buf, 1024, "[%d] %s @ %s\n", p->jobid, p->command, date);
  int user_id = p->user_id;
  char *label = "..";
  if (p->label)
    label = p->label;
  fprintf(f, "[%d] %s P:%d <%s> Pid: %d CMD: %s @ %s\n", p->jobid,
          user_name[user_id], p->num_slots, label, p->pid, p->command, buf);
  fclose(f);
}

void debug_write(const char *str) {
  FILE *f = fopen(logfile_path, "a");
  if (f == NULL) {
    return;
  }
  char buf[100];
  time_t now = time(0);
  strftime(buf, 100, "%Y-%m-%d %H:%M:%S", localtime(&now));
  fprintf(f, "%s @ %s\n", buf, str);
  fclose(f);
}

/*
static int find_user_by_name(const char *name) {
  for (int i = 0; i < user_number; i++) {
    if (strcmp(user_name[i], name) == 0)
      return i;
  }
  return -1;
}
*/

void read_user_file(const char *path) {
  server_uid = getuid();
  if (server_uid != 0) {
    error("the service is not run as root!");
  }
  FILE *fp;
  fp = fopen(path, "r");
  if (fp == NULL)
    exit(EXIT_FAILURE);
  char *line = NULL;
  size_t len = 0;
  size_t read;
  int UID, slots;
  char name[USER_NAME_WIDTH];

  while ((read = getline(&line, &len, fp)) != -1) {
    if (line[0] == '#')
      continue;

    int res = sscanf(line, "%d %256s %d", &UID, name, &slots);
    if (res != 3) {
      printf("error in read %s at line %s", path, line);
      continue;
    } else {
      int user_id = get_user_id(UID);
      if (user_id == -1) {
        if (user_number >= USER_MAX)
          continue;

        user_id = user_number;
        user_number++;

        user_UID[user_id] = UID;
        user_max_slots[user_id] = slots;
        strncpy(user_name[user_id], name, USER_NAME_WIDTH - 1);
      } else {
        user_max_slots[user_id] = slots;
      }
    }
  }

  fclose(fp);
  if (line)
    free(line);
}

const char *uid2user_name(int uid) {
  if (uid == 0)
    return "Root";
  int user_id = get_user_id(uid);
  if (user_id != -1) {
    return user_name[user_id];
  } else {
    return "Unknown";
  }
}

void s_user_status_all(int s) {
  char buffer[256];
  char *extra;
  send_list_line(s, "-- Users ----------- \n");
  for (size_t i = 0; i < user_number; i++) {
    extra = user_max_slots[i] < 0 ? "Locked" : "";
    snprintf(buffer, 256, "[%04d] %3d/%-4d %20s Run. %2d %s\n", user_UID[i],
             user_busy[i], abs(user_max_slots[i]), user_name[i], user_jobs[i],
             extra);
    send_list_line(s, buffer);
  }
  snprintf(buffer, 256, "Service at UID:%d\n", server_uid);
  send_list_line(s, buffer);
}

void s_user_status(int s, int i) {
  char buffer[256];
  char *extra = "";
  if (user_max_slots[i] < 0)
    extra = "Locked";
  snprintf(buffer, 256, "[%04d] %3d/%-4d %20s Run. %2d %s\n", user_UID[i],
           user_busy[i], abs(user_max_slots[i]), user_name[i], user_jobs[i],
           extra);
  send_list_line(s, buffer);
}

int get_user_id(int uid) {
  for (int i = 0; i < user_number; i++) {
    if (uid == user_UID[i]) {
      return i;
    }
  }
  return -1;
}
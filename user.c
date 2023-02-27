
#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

#include "main.h"
#include "user.h"

#define DEFAUL_USER_PATH "/home/kylin/task-spooler/user.txt"
#define DEFAUL_LOG_PATH "/home/kylin/task-spooler/log.txt"

void send_list_line(int s, const char *str);
void error(const char *str, ...);

int user_locked[USER_MAX] = {0};

const char *get_user_path() {
  char *str;
  str = getenv("TS_USER_PATH");
  if (str == NULL || strlen(str) == 0) {
    return DEFAUL_USER_PATH;
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

char* linux_cmd(char* CMD, char* out, int out_size) {
  FILE *fp;
  /* Open the command for reading. */
  fp = popen(CMD, "r");
  if (fp == NULL) {
    printf("Failed to run command: %s\n", CMD);
    exit(1);
  }

  /* Read the output a line at a time - output it. */
  while (fgets(out, out_size, fp) != NULL) {
    ; // printf("%s", path);
  }
  char* end = memchr(out, '\n', out_size);
  if (end != NULL) *end = '\0';
  /* close */
  pclose(fp);
  return out;
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
    logfile_path = DEFAUL_LOG_PATH;
  }
  return logfile_path;
}

void check_running_task(int pid) {
  char cmd[256], filename[256] = "";
  snprintf(cmd, sizeof(cmd), "readlink -f /proc/%d/fd/1", command_line.taskpid);
  linux_cmd(cmd, filename, sizeof(filename));
  
  int namesize = strnlen(filename, 255)+1;
  char* f = (char*) malloc(namesize);
  strncpy(f, filename, namesize);
  if (strlen(f) == 0) {
    error("PID: %d is dead", pid);
  } else {
    printf("tast stdout > %s\n", filename);
  }
  struct stat t_stat;
  snprintf(filename, 256, "/proc/%d/stat", pid);
  if (stat(filename, &t_stat) != -1) {
    command_line.start_time = t_stat.st_ctime;
  }
  command_line.outfile = f;
}



void write_logfile(const struct Job *p) {
  // char buf[1024] = "";
  FILE *f = fopen(logfile_path, "a");
  if (f == NULL) {
    return;
  }
  static char buf[100];
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

int read_first_jobid_from_logfile(const char *path) {
  FILE *fp;
  fp = fopen(path, "r");
  if (fp == NULL)
    return 1000; // default start from 1000
  char *line = NULL;
  size_t len = 0;
  size_t read;
  int jobid;

  while ((read = getline(&line, &len, fp)) != -1) {
  }
  int res = sscanf(line, "[%d]", &jobid);
  if (jobid <= 0 || res != 1) {
    jobid = 999;
  }

  printf("last line is %s with jobid = %d\n", line, jobid);
  fclose(fp);
  return jobid + 1;
}

void read_user_file(const char *path) {
  server_uid = getuid();
  // if (server_uid != root_UID) {
  //  error("the service is not run as root!");
  //}
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
    if (strncmp("TS_SLOTS", line, 8) == 0) {
      int res = sscanf(line, "TS_SLOTS = %d", &slots);
      if (slots > 0 && res == 1) {
        printf("TS_SLOTS = %d\n", slots);
        s_set_max_slots(0, slots);
        continue;
      }
    } else if (strncmp("TS_FIRST_JOBID", line, 14) == 0) {
      int res = sscanf(line, "TS_FIRST_JOBID = %d", &slots);
      if (slots > 0 && res == 1) {
        printf("TS_FIRST_JOBID = %d\n", slots);
        s_set_jobids(slots);
        continue;
      }
    }
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
    extra = user_locked[i] != 0 ? "Locked" : "";
    if (user_max_slots[i] == 0 && user_busy[i] == 0) continue;
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
  if (user_locked[i] != 0)
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

void kill_pid(int ppid, const char *signal) {
  FILE *fp;
  char command[1024];
  char *path = get_kill_sh_path();
  sprintf(command, "bash %s %d %s", path, ppid, signal);
  // printf("command = %s\n", command);
  fp = popen(command, "r");
  free(path);
  pclose(fp);
}

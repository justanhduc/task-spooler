
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "defaults.h"
#include "main.h"
#include "user.h"

void send_list_line(int s, const char *str);
void error(const char *str, ...);

vec_t users_vec;
int server_uid;
char *logfile_path;

const char *get_user_path() {
  char *str;
  str = getenv("TS_USER_PATH");
  if (str == NULL || strlen(str) == 0) {
    return DEFAULT_USER_PATH;
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

//按空格自动分割子串的函数
const char *set_server_logfile() {
  logfile_path = getenv("TS_LOGFILE_PATH");
  if (logfile_path == NULL || strlen(logfile_path) == 0) {
    logfile_path = DEFAULT_LOG_PATH;
  }
  return logfile_path;
}

/*
  ts_UID from user_number is 1-indexing
  The minimal ts_UID for a valid user is 1;
*/
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
  char *label = "..";
  if (p->label)
    label = p->label;
  fprintf(f, "[%d] %s P:%d <%s> Pid: %d CMD: %s @ %s\n", p->jobid,
          p->user->name, p->num_slots, label, p->pid, p->command, buf);
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
  fclose(fp);

  if (line == NULL) {
    free(line);
    return 999 + 1;
  }

  int res = sscanf(line, "[%d]", &jobid);
  if (res != 1 || jobid <= 0) {
    jobid = 999;
  }

  printf("last line is %s with jobid = %d\n", line, jobid);
  free(line);
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
      if (res == 1 && slots > 0) {
        printf("TS_SLOTS = %d\n", slots);
        s_set_max_slots(0, slots);
        continue;
      }
    } else if (strncmp("TS_FIRST_JOBID", line, 14) == 0) {
      int res = sscanf(line, "TS_FIRST_JOBID = %d", &slots);
      if (res == 1 && slots > 0) {
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
      int ts_UID = get_tsUID(UID);
      if (ts_UID == -1) {
        if ((int)vec_size(&users_vec) >= USER_MAX)
          continue;

        ts_UID = (int)vec_size(&users_vec);

        struct User *u = (struct User *)calloc(1, sizeof(struct User));
        if (u == NULL) {
          continue;
        }
        u->uid = UID;
        u->max_slots = slots;
        strncpy(u->name, name, USER_NAME_WIDTH - 1);
        u->name[USER_NAME_WIDTH - 1] = '\0';
        vec_push(&users_vec, u);
      } else {
        USER(ts_UID)->max_slots = slots;
      }
    }
  }

  fclose(fp);
  if (line)
    free(line);
}

const char *uid2user_name(int uid) {
  // if (uid == 0)
  //  return "Root";
  int ts_UID = get_tsUID(uid);
  if (ts_UID != -1) {
    return USER(ts_UID)->name;
  } else {
    return "Unknown";
  }
}

int get_tsUID(int uid) {
  size_t n = vec_size(&users_vec);
  for (size_t i = 0; i < n; i++) {
    struct User *u = (struct User *)vec_get(&users_vec, i);
    if (uid == (int)u->uid) {
      return (int)i;
    }
  }
  return -1;
}

void kill_pids(int parent_pid, int signal, const char* cmd) {
  char path[256];
  DIR *dir;
  struct dirent *entry;

  snprintf(path, sizeof(path), "/proc/%d/task", parent_pid);

  if ((dir = opendir(path)) == NULL) {
    // printf("Cannot find PID from %s\n", path);
    return;
  }

  while ((entry = readdir(dir)) != NULL) {
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
      continue;
    }

    int tid = atoi(entry->d_name);
    // printf("TID: %d\n", tid);
    snprintf(path, sizeof(path), "/proc/%d/task/%d/children", parent_pid, tid);

    FILE *file = fopen(path, "r");
    if (file == NULL) {
      printf("cannot open %s\n", path);
      continue;
    }
    int cPID;
    while (fscanf(file, "%d", &cPID) == 1) {
      // printf("Children PID: %d\n", cPID);
      kill_pids(cPID, signal, cmd);
    }
    fclose(file);
  }
  closedir(dir);

  if (signal >= 0 && kill(parent_pid, signal) != 0) {
    printf("kill %d -%d ", parent_pid, signal);
    perror("Error resuming process");
  }

  if (cmd != NULL) {
    char buf[1024] = "";
    snprintf(buf, 1024, "%s %d", cmd, parent_pid);
    printf("%s\n", buf);
    int result = system(buf);

    if (result == -1) {
      ; // perror("Error executing command");
    }
  }
}

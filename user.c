#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "user.h"

void send_list_line(int s, const char *str);
void error(const char *str, ...);

void read_user_file(const char *path) {
  server_uid = getuid();
  if (server_uid != 0) {
    error("the service is not run as root!");
  }
  user_number = 0;
  FILE *fp;
  fp = fopen(path, "r");
  if (fp == NULL)
    exit(EXIT_FAILURE);
  char *line = NULL;
  size_t len = 0;
  size_t read;
  while ((read = getline(&line, &len, fp)) != -1) {
    if (line[0] == '#')
      continue;

    int res = sscanf(line, "%d %256s %d", &user_UID[user_number],
                     user_name[user_number], &user_max_slots[user_number]);
    if (res != 3)
      printf("error in read %s at line %s", path, line);
    else {
      // printf("%d %s %d\n", user_ID[user_number], user_name[user_number],
      //       user_slot[user_number]);
      user_busy[user_number] = 0;
      user_jobs[user_number] = 0;
      user_queue[user_number] = 0;
      user_number++;
    }
  }

  fclose(fp);
  if (line)
    free(line);
}

void s_user_status_all(int s) {
  char buffer[256];
  for (size_t i = 0; i < user_number; i++) {
    snprintf(buffer, 256, "[%04d] %3d/%3d %20s %d\n", user_UID[i], user_busy[i],
             user_max_slots[i], user_name[i], user_jobs[user_number]);
    send_list_line(s, buffer);
  }
  snprintf(buffer, 256, "Service at UID:%d\n", server_uid);
  send_list_line(s, buffer);
}

void s_user_status(int s, int i) {
  char buffer[256];

  snprintf(buffer, 256, "[%04d] %3d/%3d %20s %d\n", user_UID[i], user_busy[i],
           user_max_slots[i], user_name[i], user_jobs[user_number]);
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
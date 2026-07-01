/*
    Task Spooler PLUS - a multi-user job scheduler like slurm.
    Copyright (C) 2007-2026  Kylin JIANG - Lluís Batlle i Rossell

    Please find the license in the provided COPYING file.
*/
#include "utils.h"
#include "error.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int64_t i64abs(int64_t x) { return x < 0 ? -x : x; }

/*
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
*/

char *charArray_string(int num, char **array) {
  int size;
  int i;
  char *commandstring;

  size = 0;
  for (i = 0; i < num; ++i) {
    size = size + strlen(array[i]) + 1;
  }

  commandstring = (char *)malloc(size);
  if (commandstring == NULL) {
    fprintf(stderr, "Error in malloc for commandstring");
    return NULL;
  }

  strcpy(commandstring, array[0]);
  for (i = 1; i < num; ++i) {
    strcat(commandstring, " ");
    strcat(commandstring, array[i]);
  }

  return commandstring;
}

char **split_str(const char *str0, int *size) {
  char **result = (char **)malloc(sizeof(char *));
  char *str = (char *)malloc(sizeof(char) * (strlen(str0) + 1));
  strcpy(str, str0);
  int n = 0;
  char *token;
  token = strtok(str, " ");
  while (token != NULL) {
    result = realloc(result, (n + 1) * sizeof(char *));
    if (result == NULL) {
      return NULL;
    }
    result[n] = token;
    n++;
    token = strtok(NULL, " ");
  }
  *size = n;
  return result;
}

int64_t str2int64(const char *str) {
  long long i;
  if (sscanf(str, "%lld", &i) != 1) {
    error("Error: in convert %s to int64_t\n", str);
  }
  return (int64_t)i;
}

char *ints_to_chars(int n, int *array, const char *delim) {
  int size = n * 12 + n * strlen(delim) + 1;
  char *tmp = (char *)malloc(size * sizeof(char));
  tmp[0] = '\0';
  int j = 0;
  for (int i = 0; i < n; i++) {
    j += sprintf(tmp + j, "%d", array[i]);
    if (i < n - 1) {
      strcat(tmp, delim);
      j += strlen(delim);
    }
  }
  return tmp;
}

int *chars_to_ints(int *size, char *str, const char *delim) {
  int count = 0;
  for (int i = 0; str[i]; i++) {
    if (str[i] == delim[0]) {
      count++;
    }
  }
  count++;
  int *result = malloc(count * sizeof(int));
  result[0] = '\0';
  char *token = strtok(str, delim);
  int index = 0;
  while (token != NULL) {
    result[index++] = atoi(token);
    token = strtok(NULL, delim);
  }
  *size = count;
  return result;
}

char *insert_chars_check(int pos, const char *input, const char *s_arg) {
  const int arg_len = strlen(s_arg);
  const int input_len = strlen(input) + 1;
  const int pos_head = pos - arg_len;
  char *str = NULL;

  if (strncmp(input + pos_head, s_arg, arg_len) == 0) {
    str = (char *)malloc((input_len + 1) * sizeof(char));
    if (str == NULL) {
      fprintf(stderr, "Memory allocation failed.\n");
      return NULL;
    }
    strcpy(str, input);
    return str;
  } else {
    int len = input_len + arg_len + 1;
    str = (char *)calloc(len, sizeof(char));
    if (str == NULL) {
      fprintf(stderr, "Memory allocation failed.\n");
      return NULL;
    }
    strncpy(str, input, pos);
    strcat(str, s_arg);
    strcat(str, input + pos);
  }
  return str;
}

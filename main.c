/*
    Task Spooler PLUS - a multi-user job scheduler like slurm.
    Copyright (C) 2007-2026  Kylin JIANG - Lluís Batlle i Rossell

    Please find the license in the provided COPYING file.
*/
#include <getopt.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <time.h> // time()
#include <unistd.h>

#include "defaults.h"
#include "main.h"
#include "version.h"
#include "user.h"
#include "utils.h"
#include "runtime_limit.h"
#include "error.h"
#include "server_start.h"
#include "client.h"
#include "server.h"
#include "signals.h"

int client_uid;
const int MAX_LEN = 1024 * 10;
extern char *optarg;
extern int optind, opterr, optopt;

/* Globals */
struct CommandLine command_line;
int term_width;

/* Globals for the environment of getopt */
static char getopt_env[20] = "POSIXLY_CORRECT=YES";
static char *old_getopt_env;

static char version[1024];

static void init_version() {
  char *ts_version = TS_MAKE_STR(TS_VERSION);
  sprintf(version,
          "Task Spooler PLUS %s - a multi-user job scheduler like slurm.\n"
          "Copyright (C) 2007-%d  Kylin JIANG - Duc Nguyen - Lluis Batlle i "
          "Rossell\n",
          ts_version, 2026);
}

static void default_command_line() {
  command_line.request = c_LIST;
  command_line.need_server = 0;
  command_line.store_output = 1;
  command_line.should_go_background = 1;
  command_line.should_keep_finished = 1;
  command_line.gzip = 0;
  command_line.linux_cmd = NULL;
  command_line.label = NULL;
  command_line.depend_on_size = 0;
  command_line.depend_on = NULL; /* -1 means depend on previous */
  command_line.max_slots = 1;
  command_line.wait_enqueuing = 1;
  command_line.stderr_apart = 0;
  command_line.num_slots = 1;
  command_line.require_elevel = 0;
  command_line.logfile = NULL;
  command_line.on_finish_cmd = getenv("TS_ONFINISH");
  command_line.rt_pid = 0;
  command_line.rt_output = NULL;
  command_line.rt_real_sec = 0;
  command_line.rt_user_sec = 0;
  command_line.rt_system_sec = 0;
  command_line.rt_pause_duration = 0;
  command_line.taskpid = 0;
  command_line.n_retry = 0;
  command_line.start_time = 0;
  command_line.wall_time = DEFAULT_MAX_WALL_TIME; // in hours
  command_line.schedule_time = 0;
  command_line.jobid = 0;
  command_line.list_format = DEFAULT;
  command_line.only_daemon = 0;
  command_line.start_service = 0;
}

struct Msg default_msg() {
  struct Msg m;
  memset(&m, 0, sizeof(struct Msg));
  return m;
}

struct Result default_result() {
  struct Result result;
  memset(&result, 0, sizeof(struct Result));
  return result;
}

void get_command(int index, int argc, char **argv) {
  command_line.command.array = &(argv[index]);
  command_line.command.num = argc - index;
}

char *get_tmp() {
  const char *tmpFolder = getenv("TMPDIR");
  if (tmpFolder == NULL) {
    tmpFolder = "/tmp/";
  }
  const char *fileNameFormat = "ts_out.%06d";
  const int folderLength = strlen(tmpFolder);
  const int maxFileNameLength = folderLength + strlen(fileNameFormat) + 10;

  char *fileName = (char *)malloc(maxFileNameLength * sizeof(char));
  char *fileName2 = (char *)malloc(maxFileNameLength * sizeof(char));

  if (fileName == NULL || fileName2 == NULL) {
    fprintf(stderr, "Error: memory allocation failed in get_tmp().\n");
    return NULL;
  }

  srand((unsigned int)time(NULL));
  unsigned int r1 = (unsigned int)rand() % 10000;
  unsigned int r2 = (unsigned int)rand() % 10000;
  unsigned int r3 = (unsigned int)rand() % 10000;

  const int randomRange = 100000;
  unsigned long long randomOffset = (unsigned long long)r1 * randomRange * randomRange
                                  + (unsigned long long)r2 * randomRange + r3;
  int finalRandomNumber = (int)(randomOffset % (unsigned long long)randomRange);

  snprintf(fileName2, maxFileNameLength, "%s/ts_out.%d", tmpFolder, finalRandomNumber);
  printf("save to %s\n", fileName2);
  free(fileName);
  return fileName2;
}

static int get_two_jobs(const char *str, int *j1, int *j2) {
  char tmp[50];
  char *tmp2;

  if (strlen(str) >= 50)
    return 0;
  strcpy(tmp, str);

  tmp2 = strchr(tmp, '-');
  if (tmp2 == NULL)
    return 0;

  /* We change the '-' to '\0', so we have a delimiter,
   * and we can access the two strings for the ids */
  *tmp2 = '\0';
  /* Skip the '\0', and point tmp2 to the second id */
  ++tmp2;

  *j1 = atoi(tmp);
  *j2 = atoi(tmp2);
  return 1;
}

int strtok_int(char *str, char *delim, int *ids) {
  int count = 0;
  char *ptr = strtok(str, delim);
  while (ptr != NULL) {
    ids[count++] = atoi(ptr);
    ptr = strtok(NULL, delim);
  }
  return count;
}

static struct option longOptions[] = {
    {"get-label", required_argument, NULL, 'a'},
    {"add-wtime", required_argument, NULL, 0},
    {"count-running", no_argument, NULL, 'R'},
    {"help", no_argument, NULL, 0},
    {"serialize", required_argument, NULL, 'M'},
    {"last-queue-id", no_argument, NULL, 'q'},
    {"full-cmd", required_argument, NULL, 'F'},
    {"get-logdir", no_argument, NULL, 0},
    {"set-logdir", required_argument, NULL, 0},
    {"getenv", required_argument, NULL, 0},
    {"setenv", required_argument, NULL, 0},
    {"unsetenv", required_argument, NULL, 0},
    {"suspend", optional_argument, NULL, 0},
    {"resume", optional_argument, NULL, 0},
    {"hold", required_argument, NULL, 0},
    {"cont", required_argument, NULL, 0},
    {"lock-ts", no_argument, NULL, 0},
    {"unlock-ts", no_argument, NULL, 0},
    {"daemon", no_argument, NULL, 0},
    {"only-daemon", no_argument, NULL, 0},
    {"tmp", no_argument, NULL, 0},
    {"jobid", required_argument, NULL, 'J'},
    {"stime", required_argument, NULL, 0},
    {"check-daemon", no_argument, NULL, 0},
    {"no-bind", no_argument, NULL, 0},
    {"no-bind-defrag", no_argument, NULL, 0},
    {"n-retry", required_argument, NULL, 0},
    {"wtime", required_argument, NULL, 0},
    {"find-by-pid", required_argument, NULL, 0},
    {"at", required_argument, NULL, 0},
    {"on-finish", required_argument, NULL, 0},
    {"start-service", no_argument, NULL, 0},
    {NULL, 0, NULL, 0}};

void parse_opts(int argc, char **argv) {
  int c;
  int res;
  int optionIdx = 0;

  /* Parse options */
  while (1) {
    c = getopt_long(
        argc, argv,
        ":AXRTVhKzClnfBE:a:F:t:c:o:p:w:k:r:u:s:U:qi:N:J:L:dS:D:W:O:M:",
        longOptions, &optionIdx);

    if (c == -1)
      break;

    switch (c) {
    case 0:
      if (strcmp(longOptions[optionIdx].name, "get-logdir") == 0) {
        command_line.request = c_GET_LOGDIR;
      } else if (strcmp(longOptions[optionIdx].name, "daemon") == 0) {
        command_line.request = c_DAEMON;
      } else if (strcmp(longOptions[optionIdx].name, "only-daemon") == 0) {
        command_line.request = c_ONLY_DAEMON;
        command_line.only_daemon = 1;
      } else if (strcmp(longOptions[optionIdx].name, "tmp") == 0) {
        command_line.outfile = get_tmp();
      } else if (strcmp(longOptions[optionIdx].name, "check-daemon") == 0) {
        command_line.request = c_CHECK_DAEMON;
        command_line.need_server = 0;
      } else if (strcmp(longOptions[optionIdx].name, "set-logdir") == 0) {
        command_line.request = c_SET_LOGDIR;
        command_line.label = optarg; /* reuse this variable */
      } else if (strcmp(longOptions[optionIdx].name, "help") == 0) {
        command_line.request = c_SHOW_HELP;
      } else if (strcmp(longOptions[optionIdx].name, "get-label") == 0) {
        command_line.request = c_GET_LABEL;
        command_line.jobid = str2int64(optarg);
      } else if (strcmp(longOptions[optionIdx].name, "add-wtime") == 0) {
        command_line.request = c_ADD_WTIME;
        if (parse_time(optarg, &command_line.wall_time) != 0) {
          fprintf(stderr, "Error: invalid duration '%s' for --add-wtime (examples, 30s, 3.4m, 1.5H, 2d).\n", optarg);
          exit(EXIT_FAILURE);
        }
      } else if (strcmp(longOptions[optionIdx].name, "hold") == 0) {
        command_line.request = c_HOLD_JOB;
        command_line.jobid = str2int64(optarg);
      } else if (strcmp(longOptions[optionIdx].name, "cont") == 0) {
        command_line.request = c_CONT_JOB;
        command_line.jobid = str2int64(optarg);
      } else if (strcmp(longOptions[optionIdx].name, "lock-ts") == 0) {
        command_line.request = c_LOCK_SERVER;
      } else if (strcmp(longOptions[optionIdx].name, "unlock-ts") == 0) {
        command_line.request = c_UNLOCK_SERVER;
      } else if (strcmp(longOptions[optionIdx].name, "suspend") == 0) {
        command_line.request = c_SUSPEND_USER;
        command_line.label = optarg; /* reuse this var */
      } else if (strcmp(longOptions[optionIdx].name, "resume") == 0) {
        command_line.request = c_RESUME_USER;
        command_line.label = optarg; /* reuse this var */
      } else if (strcmp(longOptions[optionIdx].name, "getenv") == 0) {
        command_line.request = c_GET_ENV;
        command_line.label = optarg; /* reuse this var */
      } else if (strcmp(longOptions[optionIdx].name, "setenv") == 0) {
        command_line.request = c_SET_ENV;
        command_line.label = optarg; /* reuse this var */
      } else if (strcmp(longOptions[optionIdx].name, "unsetenv") == 0) {
        command_line.request = c_UNSET_ENV;
        command_line.label = optarg; /* reuse this var */
      } else if (strcmp(longOptions[optionIdx].name, "full-cmd") == 0) {
        command_line.request = c_SHOW_CMD;
        if (optarg != NULL) {
          command_line.jobid = str2int64(optarg);
        } else {
          command_line.jobid = -1;
        }
      } else if (strcmp(longOptions[optionIdx].name, "stime") == 0) {
        command_line.start_time = str2int64(optarg);
      } else if (strcmp(longOptions[optionIdx].name, "find-by-pid") == 0) {
        command_line.request = c_FIND_PID;
        command_line.taskpid = str2int64(optarg);
        if (command_line.taskpid <= 0) {
          error("Error: invalid PID '%s' for --find-by-pid.\n", optarg);
        }
      } else if (strcmp(longOptions[optionIdx].name, "wtime") == 0) {
        if (parse_time(optarg, &command_line.wall_time) != 0) {
          error("Error: invalid duration '%s' for --wtime (examples, 30s, 3.4m, 1.5H, 2d).\n", optarg);
        }
      } else if (strcmp(longOptions[optionIdx].name, "at") == 0) {
        if (parse_schedule(optarg, &command_line.schedule_time) != 0)
            error("Error: invalid schedule '%s' for --at (e.g. +5m, 14:00, 2025-06-01 14:00)\n", optarg);
      } else if (strcmp(longOptions[optionIdx].name, "on-finish") == 0) {
        command_line.on_finish_cmd = optarg;
      } else if (strcmp(longOptions[optionIdx].name, "start-service") == 0) {
        command_line.start_service = 1;
      } else if (strcmp(longOptions[optionIdx].name, "no-bind") == 0) {
        command_line.no_cpu_binding = 1;
      } else if (strcmp(longOptions[optionIdx].name, "no-bind-defrag") == 0) {
        command_line.no_bind_defrag = 1;
      } else if (strcmp(longOptions[optionIdx].name, "n-retry") == 0) {
        command_line.n_retry = str2int64(optarg);
        if (command_line.n_retry < 0)
          command_line.n_retry = 0;
      } else {
        error("Error: invalid option %s", longOptions[optionIdx].name);
      }
      break;
    case 'K':
      command_line.request = c_KILL_SERVER;
      command_line.should_go_background = 0;
      break;
    case 'T':
      command_line.request = c_KILL_ALL;
      break;
    case 'X':
      command_line.request = c_REFRESH_USER;
      break;
    case 'k':
      printf("c_KILL_JOB = %s\n", optarg);
      command_line.request = c_KILL_JOB;
      command_line.jobid = str2int64(optarg);
      break;
    case 'r':
      command_line.request = c_REMOVEJOB;
      command_line.jobid = str2int64(optarg);
      break;
    case 'l':
      command_line.request = c_LIST;
      break;
    case 'A':
      command_line.request = c_LIST_ALL;
      break;
    case 'h':
      command_line.request = c_SHOW_HELP;
      break;
    case 'd':
      command_line.depend_on = (int *)malloc(sizeof(int));
      command_line.depend_on_size = 1;
      command_line.depend_on[0] = -1;
      break;
    case 'V':
      command_line.request = c_SHOW_VERSION;
      break;
    case 'C':
      command_line.request = c_CLEAR_FINISHED;
      break;
    case 'c':
      command_line.request = c_CAT;
      command_line.jobid = str2int64(optarg);
      break;
    case 'o':
      command_line.request = c_SHOW_OUTPUT_FILE;
      command_line.jobid = str2int64(optarg);
      break;
    case 'O':
      command_line.logfile = optarg;
      break;
    case 'n':
      command_line.store_output = 0;
      break;
    case 'L':
      command_line.label = optarg;
      break;
    case 'z':
      command_line.gzip = 1;
      break;
    case 'f':
      command_line.should_go_background = 0;
      break;
    case 't':
      command_line.request = c_TAIL;
      command_line.jobid = str2int64(optarg);
      break;
    case 'p':
      command_line.request = c_SHOW_PID;
      command_line.jobid = str2int64(optarg);
      break;
    case 'i':
      command_line.request = c_INFO;
      command_line.jobid = str2int64(optarg);
      break;
    case 'q':
      command_line.request = c_LAST_ID;
      break;
    case 'a':
      command_line.request = c_GET_LABEL;
      command_line.jobid = str2int64(optarg);
      break;
    case 'F':
      command_line.request = c_SHOW_CMD;
      command_line.jobid = str2int64(optarg);
      break;
    case 'N':
      command_line.num_slots = str2int64(optarg);
      if (command_line.num_slots < 0)
        command_line.num_slots = 0;
      break;
    case 'J':
      command_line.jobid = str2int64(optarg);
      if (command_line.jobid < 0)
        command_line.jobid = 0;
      break;
    /*
    case 'Z':
      command_line.taskpid = str2int64(optarg);
      if (command_line.taskpid <= 0)
        command_line.taskpid = 0;
      else {
        char cmd[256], out[256] = "";
        snprintf(cmd, sizeof(cmd), "readlink -f /proc/%d/fd/1",
    command_line.taskpid); linux_cmd(cmd, out, sizeof(out));

        printf("outfile: %s\n", out);
        if (strlen(out) == 0) {
          printf("PID: %d is dead\n", command_line.taskpid);
          return;
        }
      }
      break;
    */
    case 'w':
      command_line.request = c_WAITJOB;
      command_line.jobid = str2int64(optarg);
      break;
    case 'u':
      command_line.request = c_URGENT;
      command_line.jobid = str2int64(optarg);
      break;
    case 's':
      command_line.request = c_GET_STATE;
      command_line.jobid = str2int64(optarg);
      break;
    case 'S':
      command_line.request = c_SET_MAX_SLOTS;
      command_line.max_slots = str2int64(optarg);
      if (command_line.max_slots < 1) {
        error("Error: at least one slot must be specified.\n");
      }
      break;
    case 'D':
      command_line.depend_on = (int *)malloc(strlen(optarg) * sizeof(int));
      command_line.depend_on_size =
          strtok_int(optarg, ",", command_line.depend_on);
      break;
    case 'W':
      command_line.depend_on = (int *)malloc(strlen(optarg) * sizeof(int));
      command_line.depend_on_size =
          strtok_int(optarg, ",", command_line.depend_on);
      command_line.require_elevel = 1;
      break;
    case 'U':
      command_line.request = c_SWAP_JOBS;
      res = get_two_jobs(optarg, &command_line.jobid, &command_line.jobid2);
      if (!res) {
        error("Error: Invalid <id-id> format for -U: %s.\n", optarg);
      }
      if (command_line.jobid == command_line.jobid2) {
        error("Error: The two Jobids in -U <id-id> must be different.\n");
      }
      break;
    case 'B':
      /* I picked 'B' quite at random among the letters left */
      command_line.wait_enqueuing = 0;
      break;
    case 'E':
      command_line.stderr_apart = 1;
      break;
    case 'R':
      command_line.request = c_COUNT_RUNNING;
      break;
    case 'M':
      command_line.request = c_LIST;

      if (strcmp(optarg, "default") == 0)
        command_line.list_format = DEFAULT;
      else if (strcmp(optarg, "json") == 0)
        command_line.list_format = JSON;
      else if (strcmp(optarg, "tab") == 0)
        command_line.list_format = TAB;
      else {
        error("Error: Invalid argument for option -M or --tab: %s.\n", optarg);
      }
      break;
    case ':':
      switch (optopt) {
      case 't':
        command_line.request = c_TAIL;
        command_line.jobid = -1; /* This means the 'last' job */
        break;
      case 'c':
        command_line.request = c_CAT;
        command_line.jobid = -1; /* This means the 'last' job */
        break;
      case 'o':
        command_line.request = c_SHOW_OUTPUT_FILE;
        command_line.jobid = -1; /* This means the 'last' job */
        break;
      case 'p':
        command_line.request = c_SHOW_PID;
        command_line.jobid = -1; /* This means the 'last' job */
        break;
      case 'i':
        command_line.request = c_INFO;
        command_line.jobid = -1; /* This means the 'last' job */
        break;
      case 'r':
        command_line.request = c_REMOVEJOB;
        command_line.jobid = -1; /* This means the 'last'
                                    added job */
        break;
      case 'w':
        command_line.request = c_WAITJOB;
        command_line.jobid = -1; /* This means the 'last'
                                    added job */
        break;
      case 'u':
        command_line.request = c_URGENT;
        command_line.jobid = -1; /* This means the 'last'
                                    added job */
        break;
      case 's':
        command_line.request = c_GET_STATE;
        command_line.jobid = -1; /* This means the 'last'
                                    added job */
        break;
      case 'k':
        command_line.request = c_KILL_JOB;
        command_line.jobid = -1; /* This means the 'last' job */
        break;
      case 'S':
        command_line.request = c_GET_MAX_SLOTS;
        break;
      case 'a':
        command_line.request = c_GET_LABEL;
        command_line.jobid = -1;
        break;
      case 'F':
        command_line.request = c_SHOW_CMD;
        command_line.jobid = -1;
        break;
      case 'M':
        command_line.request = c_LIST;
        command_line.list_format = DEFAULT;
        break;
      default:
        error("Error: Option -%c requires an argument.\n", optopt);
      }
      break;
    case '?':
      error("Error: Unknown option -%c.\n", optopt);
    }
  }

  command_line.command.num = 0;

  /* if the request is still the default option...
   * (the default values should be centralized) */
  if (optind < argc && command_line.request == c_LIST) {
    command_line.request = c_QUEUE;
    get_command(optind, argc, argv);
    // check_length
    int n_len = 0;
    for (size_t i = 0; i < command_line.command.num; i++)
    {
      n_len += strnlen(command_line.command.array[i], 1024);
    }
    if (n_len > MAX_LEN) {
      error("too long command: %d chars, max %d", n_len, MAX_LEN);
    }
    
  }

  command_line.linux_cmd = charArray_string(argc, argv);

  if (command_line.request != c_SHOW_HELP &&
      command_line.request != c_SHOW_VERSION)
    command_line.need_server = 1;

  if (!command_line.store_output && !command_line.should_go_background)
    command_line.should_keep_finished = 0;
}

static void fill_first_3_handles() {
  int tmp_pipe1[2];
  int tmp_pipe2[2];
  /* This will fill handles 0 and 1 */
  pipe(tmp_pipe1);
  /* This will fill handles 2 and 3 */
  pipe(tmp_pipe2);

  close(tmp_pipe2[1]);
}

static void go_background() {
  int pid;
  pid = fork();

  switch (pid) {
  case -1:
    error("fork failed");
    break;
  case 0:
    close(0);
    close(1);
    close(2);
    /* This is a weird thing. But we will later want to
     * allocate special files to the 0, 1 or 2 fds. It's
     * almost impossible, if other important things got
     * allocated here. */
    fill_first_3_handles();
    setsid();
    break;
  default:
    exit(EXIT_SUCCESS);
  }
}

static void print_help(const char *cmd) {
  puts(version);
  printf("\nusage: %s [action] [-ngfmdE] [-L <lab>] [-D <id>] [cmd...]\n\n", cmd);
  printf("Environment Variables:\n");
  printf("  TS_SOCKET        : Unix socket path (default: $TMPDIR/socket-ts.root)\n");
  printf("  TS_SERVICE_NAME  : Service name (default: %s)\n", DEFAULT_HPC_NAME);
  printf("  TS_MAXFINISHED   : Max finished jobs in queue (default: %d)\n", DEFAULT_MAXFINISHED);
  printf("  TS_MAXCONN       : Max concurrent connections (max %d, default: %d)\n", MAXCONN, MAXCONN);
  printf("  TS_ENV           : Command to gather job info during enqueue\n");
  printf("  TS_ONFINISH      : Default on-finish command (overridden by --on-finish)\n");
  printf("  TS_SAVELIST      : Crash recovery file for job list\n");
  printf("  TS_SLOTS         : Max concurrent jobs (server start, default: 1)\n");
  printf("  TS_USER_PATH     : User config file path (server start)\n");
  printf("  TS_LOGFILE_PATH  : Job log path (server start)\n");
  printf("  TS_SQLITE_PATH   : SQLite DB path for logs (server start)\n");
  printf("  TS_FIRST_JOBID   : Initial job ID (server start, default: 1000)\n");
  printf("  TS_SORTJOBS      : Job queue sorting control (server start)\n");
  printf("  TS_MAX_WALL_TIME : Max job wall-time in seconds (server start, default: %d)\n", DEFAULT_MAX_WALL_TIME);
  printf("  TMPDIR           : Temporary Output files directory\n");

  printf("\nLong option actions:\n");
  printf("  --getenv [var]              Get server environment variable\n");
  printf("  --setenv [var]              Set server environment flag\n");
  printf("  --unsetenv [var]            Remove server environment flag\n");
  printf("  --get-label, -a [id]        Show job label (last added if unspecified)\n");
  printf("  --full-cmd, -F [id]         Show full command (last added if unspecified)\n");
  printf("  --check-daemon              Verify daemon status\n");
  printf("  --count-running, -R         Count running jobs\n");
  printf("  --last-queue-id, -q         Show last added job ID\n");
  printf("  --get-logdir                Display log directory path\n");
  printf("  --set-logdir [path]         Configure log directory\n");
  printf("  --serialize, -M [fmt]       Export job list (default/json/tab)\n");
  printf("                              Use -M json -J <id> for single job JSON\n");
  printf("  --tmp                       Store logs in tmp folder\n");
  printf("  --hold [id]                 Pause specified job\n");
  printf("  --cont [id]                 Resume paused job\n");
  printf("  --suspend [user]            Pause tasks and lock account\n");
  printf("  --resume [user]             Resume tasks and unlock account\n");
  printf("  --lock                      Lock server (%d sec; root no timeout)\n", DEFAULT_USER_LOCK_TIME);
  printf("  --unlock                    Release server lock\n");
  printf("  --wtime [time]              Wall time limit (30s, 3.4m, 1.5H, 2d)\n");
  printf("  --find-by-pid [pid]         Find job that owns a PID\n");
  printf("  --on-finish [tmpl]          Run command after job finishes\n");
  printf("                              Placeholders: {jobid} {output} {exitcode} {pid}\n");
  printf("                              {label} {command} {realtime} {slots}\n");
  printf("  --at [time]                 Schedule: +5m, 14:00, 2025-06-01T14:00\n");
  printf("  --no-bind                   Disable CPU binding\n");
  printf("  --no-bind-defrag            Disable CPU defrag (server start)\n");
  printf("  --n-retry [N]               Max retries if job fails within 15 sec\n");
  printf("  --add-wtime [time]          Increase job wall time (root only)\n");
  printf("  --job, -J [id]              Specify job ID for command\n");
  printf("  --daemon                    Run as daemon (root only)\n");
  printf("  --only-daemon               Run as sole daemon, kill competing root instances (root only)\n");

  printf("\nActions:\n");
  printf("  -A              List all users' jobs\n");
  printf("  -X              Reload user config (root only, max %d users)\n", USER_MAX);
  printf("  -K              Stop server (root only)\n");
  printf("  -C              Clear finished jobs\n");
  printf("  -l              Show job list (default)\n");
  printf("  -S [num]        Get/set max slots (root only)\n");
  printf("  -t [id]         Tail last 10 lines\n");
  printf("  -c [id]         Show complete output\n");
  printf("  -p [id]         Show PID\n");
  printf("  -o [id]         Show output file path\n");
  printf("  -i [id]         Show job info\n");
  printf("  -s [id]         Show job state\n");
  printf("  -r [id]         Remove job\n");
  printf("  -w [id]         Wait for job completion\n");
  printf("  -k [id]         Send SIGTERM\n");
  printf("  -T              SIGTERM all jobs (root only)\n");
  printf("  -u [id]         Prioritize job\n");
  printf("  -U [id-id]      Swap two jobs\n");
  printf("  -h, --help      Show this help\n");
  printf("  -V              Show version\n");

  printf("\nJob options:\n");
  printf("  -B              Exit if queue full\n");
  printf("  -n              Discard output\n");
  printf("  -E              Separate stderr to .e file\n");
  printf("  -O [name]       Set output filename\n");
  printf("  -z              Gzip output\n");
  printf("  -f              Run in foreground\n");
  printf("  -d              Run after last queued job\n");
  printf("  -D [ids]        Run after specified job IDs complete\n");
  printf("  -W [ids]        Run after successful (exit 0) job IDs\n");
  printf("  -L [label]      Assign a label\n");
  printf("  -N [num]        Request slots (default 1)\n");
}

static void print_version() { puts(version); }

static void set_getopt_env() {
  old_getopt_env = getenv("POSIXLY_CORRECT");
  putenv(getopt_env);
}

static void unset_getopt_env() {
  if (old_getopt_env == NULL) {
    /* Wipe the string from the environment */
    putenv("POSIXLY_CORRECT");
  } else {
    snprintf(getopt_env, 20, "POSIXLY_CORRECT=%s", old_getopt_env);
  }
}

static void get_terminal_width() {
  struct winsize ws;
  ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws);
  term_width = ws.ws_col;
}

// TODO add the benchmark for the performance.
int main(int argc, char **argv) {
  int errorlevel = 0;
  jobsort_flag = 0;
  user_locker = NULL;
  client_uid = getuid();
  // printf("client_uid = %u\n", client_uid);
  init_version();
  get_terminal_width();
  process_type = CLIENT;

  set_getopt_env();
  /* This is needed in a gnu system, so getopt works well */
  default_command_line();
  parse_opts(argc, argv);
  unset_getopt_env();

  /* This will be inherited by the server, if it's run */
  ignore_sigpipe();

  if (command_line.request == c_CHECK_DAEMON) {
    c_check_daemon();
  }

  if (command_line.need_server) {
    if (command_line.request == c_DAEMON
        || command_line.request == c_ONLY_DAEMON) {
      ensure_server_up(1);
    } else {
      ensure_server_up(0);
    }
    c_check_version();
  }

  switch (command_line.request) {
  case c_REFRESH_USER:
    if (client_uid == 0) {
      c_refresh_user();
      c_wait_server_lines();
    } else {
      printf("Only the root can refresh the task-spooler user configurations\n");
    }
    break;
  case c_DAEMON:
    break;
  case c_ONLY_DAEMON:
    break;
  case c_CHECK_DAEMON:
    break;
  case c_HOLD_JOB:
    c_hold_job(command_line.jobid);
    // c_wait_server_lines();
    break;

  case c_CONT_JOB:
    c_cont_job(command_line.jobid);
    break;
  case c_LOCK_SERVER:
    errorlevel = c_lock_server();
    break;
  case c_UNLOCK_SERVER:
    errorlevel = c_unlock_server();
    break;
  case c_SUSPEND_USER: {
    int stop_uid = client_uid;
    if (command_line.label != NULL) {
      if (client_uid == 0) {
        struct passwd *pwd = getpwnam(command_line.label);
        if (pwd == NULL) {
          printf("Error: Cannot find user: %s to stop\n", command_line.label);
          return -1;
        }
        stop_uid = pwd->pw_uid;
      } else {
        printf("Error: Cannot stop the user %s\n", command_line.label);
        return -1;
      }
    }
    if (stop_uid != 0) {
      printf("To stop user ID: %d\n", stop_uid);
    } else {
      printf("To stop all users by `Root`\n");
    }
    c_suspend_user(stop_uid);
    c_wait_server_lines();
  } break;
  case c_RESUME_USER: {
    int cont_uid = client_uid;
    if (command_line.label != NULL) {
      if (client_uid == 0) {
        struct passwd *pwd = getpwnam(command_line.label);
        if (pwd == NULL) {
          printf("Error: Cannot find user: %s to resume\n", command_line.label);
          return -1;
        }
        cont_uid = pwd->pw_uid;
      } else {
        printf("Error: Cannot resume the user %s\n", command_line.label);
        return -1;
      }
    }
    if (cont_uid != 0) {
      printf("To resume user ID: %d\n", cont_uid);
    } else {
      printf("To resume all users from `Root`\n");
    }
    c_resume_user(cont_uid);
    c_wait_server_lines();
  } break;
  case c_SHOW_VERSION:
    print_version();
    break;
  case c_SHOW_HELP:
    print_help(argv[0]);
    break;
  case c_QUEUE:
    if (command_line.command.num <= 0)
      error("Tried to queue a void command. parameters: %i",
            command_line.command.num);
    if (!command_line.need_server)
      error("The command %i needs the server", command_line.request);
    if (command_line.schedule_time > get_monotonic_sec()) {
        time_t wall = command_line.schedule_time + (time(NULL) - get_monotonic_sec());
        struct tm *tm = localtime(&wall);
        char buf[64];
        strftime(buf, sizeof(buf), "%c", tm);
        printf("scheduled in %s after, at %s\n",
               format_schedule_delta(command_line.schedule_time), buf);
        fflush(stdout);
    }
    c_new_job();
    command_line.jobid = c_wait_newjob_ok();
    if (command_line.store_output) {
      printf("New JobID: %i\n", command_line.jobid);
      fflush(stdout);
    }
    if (command_line.should_go_background) {
      go_background();
      c_wait_server_commands();
    } else {
      errorlevel = c_wait_server_commands();
    }
    break;
  case c_LIST_ALL:
    if (!command_line.need_server)
      error("The command %i needs the server", command_line.request);
    c_list_jobs_all();
    c_wait_server_lines();
    break;
  case c_LIST:
    if (!command_line.need_server)
      error("The command %i needs the server", command_line.request);
    c_list_jobs();
    c_wait_server_lines();
    break;
  case c_KILL_SERVER:
    if (!command_line.need_server)
      error("The command %i needs the server", command_line.request);
    c_shutdown_server();
    break;
  case c_CLEAR_FINISHED:
    if (!command_line.need_server)
      error("The command %i needs the server", command_line.request);
    c_clear_finished();
    break;
  case c_TAIL:
    if (!command_line.need_server)
      error("The command %i needs the server", command_line.request);
    errorlevel = c_tail();
    /* This will not return! */
    break;
  case c_CAT:
    if (!command_line.need_server)
      error("The command %i needs the server", command_line.request);
    errorlevel = c_cat();
    /* This will not return! */
    break;
  case c_SHOW_OUTPUT_FILE:
    if (!command_line.need_server)
      error("The command %i needs the server", command_line.request);
    c_show_output_file();
    break;
  case c_SHOW_PID:
    if (!command_line.need_server)
      error("The command %i needs the server", command_line.request);
    c_show_pid();
    break;
  case c_KILL_ALL:
    if (!command_line.need_server)
      error("The command %i needs the server", command_line.request);
    c_kill_all_jobs();
    break;
  case c_KILL_JOB:
    if (!command_line.need_server)
      error("The command %i needs the server", command_line.request);
    c_kill_job();
    break;
  case c_INFO:
    if (!command_line.need_server)
      error("The command %i needs the server", command_line.request);
    c_show_info();
    break;
  case c_LAST_ID:
    if (!command_line.need_server)
      error("The command %i needs the server", command_line.request);
    c_show_last_id();
    break;
  case c_GET_LABEL:
    if (!command_line.need_server)
      error("The command %i needs the server", command_line.request);
    c_show_label();
    break;
  case c_ADD_WTIME:
    c_add_wtime();
    break;
  case c_SHOW_CMD:
    if (!command_line.need_server)
      error("The command %i needs the server", command_line.request);
    c_show_cmd();
    break;
  case c_REMOVEJOB:
    if (!command_line.need_server)
      error("The command %i needs the server", command_line.request);
    c_remove_job();
    break;
  case c_WAITJOB:
    if (!command_line.need_server)
      error("The command %i needs the server", command_line.request);
    errorlevel = c_wait_job();
    break;
  case c_URGENT:
    if (!command_line.need_server)
      error("The command %i needs the server", command_line.request);
    c_move_urgent();
    break;
  case c_SET_MAX_SLOTS:
    if (client_uid != 0) {
      printf("Reset max slots is only supported by root user!\n");
      break;
    } else {
      c_send_max_slots(command_line.max_slots);
      c_wait_server_lines();
    }
    break;
  case c_GET_MAX_SLOTS:
    c_get_max_slots();
    break;
  case c_SWAP_JOBS:
    if (!command_line.need_server)
      error("The command %i needs the server", command_line.request);
    c_swap_jobs();
    break;
  case c_COUNT_RUNNING:
    if (!command_line.need_server)
      error("The command %i needs the server", command_line.request);
    c_get_count_running();
    break;
  case c_GET_STATE:
    if (!command_line.need_server)
      error("The command %i needs the server", command_line.request);
    /* This will also print the state into stdout */
    c_get_state();
    break;
  case c_GET_LOGDIR:
    c_get_logdir();
    break;
  case c_SET_LOGDIR:
    c_set_logdir();
    break;
  case c_GET_ENV:
    if (!command_line.need_server)
      error("The command %i needs the server", command_line.request);
    c_get_env();
    break;
  case c_SET_ENV:
    if (!command_line.need_server)
      error("The command %i needs the server", command_line.request);
    c_set_env();
    break;
  case c_UNSET_ENV:
    if (!command_line.need_server)
      error("The command %i needs the server", command_line.request);
    c_unset_env();
    break;
  case c_FIND_PID:
    if (!command_line.need_server)
      error("The command %i needs the server", command_line.request);
    printf("%i\n", c_find_pid(command_line.taskpid, 1 /* deep search */));
    break;
  }

  if (command_line.need_server) {
    close(server_socket);
  }
  free(command_line.depend_on);

  return errorlevel;
}

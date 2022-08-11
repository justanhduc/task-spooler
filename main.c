/*
    Task Spooler - a task queue system for the unix user
    Copyright (C) 2007-2009  Lluís Batlle i Rossell

    Please find the license in the provided COPYING file.
*/
#define TS_VERSION_FALLBACK "1.3.1"

/* from https://github.com/LLNL/lbann/issues/117
 * and https://gcc.gnu.org/onlinedocs/cpp/Stringizing.html#Stringizing */
#define TS_MAKE_STR(x) _TS_MAKE_STR(x)
#define _TS_MAKE_STR(x) #x

#include <getopt.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <unistd.h>

#include "main.h"

extern char *optarg;
extern int optind, opterr, optopt;

/* Globals */
struct CommandLine command_line;
int term_width;

/* Globals for the environment of getopt */
static char getopt_env[] = "POSIXLY_CORRECT=YES";
static char *old_getopt_env;

static char version[1024];

static void init_version() {
#ifdef TS_VERSION
  char *ts_version = TS_MAKE_STR(TS_VERSION);
  sprintf(version,
          "Task Spooler %s - a task queue system for the unix user.\n"
          "Copyright (C) 2007-2020  Duc Nguyen - Lluis Batlle i Rossell",
          ts_version);
#else
  sprintf(version,
          "Task Spooler %s - a task queue system for the unix user.\n"
          "Copyright (C) 2007-2020  Duc Nguyen - Lluis Batlle i Rossell",
          TS_VERSION_FALLBACK);
#endif
}

static void default_command_line() {
  command_line.request = c_LIST;
  command_line.plain_list = 0;
  command_line.need_server = 0;
  command_line.store_output = 1;
  command_line.should_go_background = 1;
  command_line.should_keep_finished = 1;
  command_line.gzip = 0;
  command_line.send_output_by_mail = 0;
  command_line.label = 0;
  command_line.depend_on_size = 0;
  command_line.depend_on = NULL; /* -1 means depend on previous */
  command_line.max_slots = 1;
  command_line.wait_enqueuing = 1;
  command_line.stderr_apart = 0;
  command_line.num_slots = 1;
  command_line.require_elevel = 0;
  command_line.logfile = NULL;
}

struct Msg default_msg() {
  struct Msg m;
  memset(&m, 0, sizeof(struct Msg));
  m.uid = getuid();
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
    {"get_label", required_argument, NULL, 'a'},
    {"count_running", no_argument, NULL, 'R'},
    {"last_queue_id", no_argument, NULL, 'q'},
    {"full_cmd", optional_argument, NULL, 'F'},
    {"plain", no_argument, NULL, 0},
    {"get_logdir", no_argument, NULL, 0},
    {"set_logdir", required_argument, NULL, 0},
    {"getenv", required_argument, NULL, 0},
    {"setenv", required_argument, NULL, 0},
    {"unsetenv", required_argument, NULL, 0},
    {"stop", optional_argument, NULL, 0},
    {"cont", optional_argument, NULL, 0},
    {NULL, 0, NULL, 0}};

void parse_opts(int argc, char **argv) {
  int c;
  int res;
  int optionIdx = 0;

  /* Parse options */
  while (1) {
    c = getopt_long(argc, argv,
                    ":AXRTVhKzClnfmBEr:a:F:t:c:o:p:w:k:u:s:U:qi:N:L:dS:D:W:O:",
                    longOptions, &optionIdx);

    if (c == -1)
      break;

    switch (c) {
    case 0:
      if (strcmp(longOptions[optionIdx].name, "get_logdir") == 0) {
        command_line.request = c_GET_LOGDIR;
      } else if (strcmp(longOptions[optionIdx].name, "set_logdir") == 0) {
        command_line.request = c_SET_LOGDIR;
        command_line.label = optarg; /* reuse this variable */
      } else if (strcmp(longOptions[optionIdx].name, "get_label") == 0) {
        command_line.request = c_GET_LABEL;
        command_line.jobid = atoi(optarg);
      } else if (strcmp(longOptions[optionIdx].name, "stop") == 0) {
        command_line.request = c_STOP_USER;
        command_line.label = optarg; /* reuse this var */
      } else if (strcmp(longOptions[optionIdx].name, "cont") == 0) {
        command_line.request = c_CONT_USER;
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
      } else if (strcmp(longOptions[optionIdx].name, "plain") == 0) {
        command_line.request = c_LIST;
        command_line.plain_list = 1;
      } else
        error("Wrong option %s.", longOptions[optionIdx].name);
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
      command_line.request = c_KILL_JOB;
      command_line.jobid = atoi(optarg);
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
      command_line.jobid = atoi(optarg);
      break;
    case 'o':
      command_line.request = c_SHOW_OUTPUT_FILE;
      command_line.jobid = atoi(optarg);
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
    case 'm':
      command_line.send_output_by_mail = 1;
      break;
    case 't':
      command_line.request = c_TAIL;
      command_line.jobid = atoi(optarg);
      break;
    case 'p':
      command_line.request = c_SHOW_PID;
      command_line.jobid = atoi(optarg);
      break;
    case 'i':
      command_line.request = c_INFO;
      command_line.jobid = atoi(optarg);
      break;
    case 'q':
      command_line.request = c_LAST_ID;
      break;
    case 'a':
      command_line.request = c_GET_LABEL;
      command_line.jobid = atoi(optarg);
      break;
    case 'F':
      command_line.request = c_SHOW_CMD;
      command_line.jobid = atoi(optarg);
      break;
    case 'N':
      command_line.num_slots = atoi(optarg);
      if (command_line.num_slots < 0)
        command_line.num_slots = 0;
      break;
    case 'r':
      command_line.request = c_REMOVEJOB;
      command_line.jobid = atoi(optarg);
      break;
    case 'w':
      command_line.request = c_WAITJOB;
      command_line.jobid = atoi(optarg);
      break;
    case 'u':
      command_line.request = c_URGENT;
      command_line.jobid = atoi(optarg);
      break;
    case 's':
      command_line.request = c_GET_STATE;
      command_line.jobid = atoi(optarg);
      break;
    case 'S':
      command_line.request = c_SET_MAX_SLOTS;
      command_line.max_slots = atoi(optarg);
      if (command_line.max_slots < 1) {
        fprintf(stderr, "You should set at minimum 1 slot.\n");
        exit(-1);
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
        fprintf(stderr, "Wrong <id-id> for -U.\n");
        exit(-1);
      }
      if (command_line.jobid == command_line.jobid2) {
        fprintf(stderr, "Wrong <id-id> for -U. "
                        "Use different ids.\n");
        exit(-1);
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
      default:
        fprintf(stderr, "Option %c missing argument.\n", optopt);
        exit(-1);
      }
      break;
    case '?':
      fprintf(stderr, "Wrong option %c.\n", optopt);
      exit(-1);
    }
  }

  command_line.command.num = 0;

  /* if the request is still the default option...
   * (the default values should be centralized) */
  if (optind < argc && command_line.request == c_LIST) {
    command_line.request = c_QUEUE;
    get_command(optind, argc, argv);
  }

  if (command_line.request != c_SHOW_HELP &&
      command_line.request != c_SHOW_VERSION)
    command_line.need_server = 1;

  if (!command_line.store_output && !command_line.should_go_background)
    command_line.should_keep_finished = 0;

  if (command_line.send_output_by_mail &&
      ((!command_line.store_output) || command_line.gzip)) {
    fprintf(stderr,
            "For e-mail, you should store the output (not through gzip)\n");
    exit(-1);
  }
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
    exit(0);
  }
}

static void print_help(const char *cmd) {
  puts(version);
  printf("usage: %s [action] [-ngfmdE] [-L <lab>] [-D <id>] [cmd...]\n", cmd);
  printf("Env vars:\n");
  printf("  TS_SOCKET  the path to the unix socket used by the ts command.\n");
  printf("  TS_MAILTO  where to mail the result (on -m). Local user by "
         "default.\n");
  printf("  TS_MAXFINISHED  maximum finished jobs in the queue.\n");
  printf("  TS_MAXCONN  maximum number of ts connections at once.\n");
  printf("  TS_ONFINISH  binary called on job end (passes jobid, error, "
         "outfile, command).\n");
  printf("  TS_ENV  command called on enqueue. Its output determines the job "
         "information.\n");
  printf("  TS_SAVELIST  filename which will store the list, if the server "
         "dies.\n");
  printf("  TS_SLOTS   amount of jobs which can run at once, read on server "
         "start.\n");
  printf("  TS_USER_PATH  path to the user configuration file, read on server "
         "starts.\n");
  printf("  TS_LOGFILE_PATH  path to the job log file, read on server "
         "starts\n");
  printf("  TS_JOBID  The first job ID (default: 1000), read on server "
         "starts.\n");
  printf("  TMPDIR     directory where to place the output files and the "
         "default socket.\n");
  printf("Long option actions:\n");
  printf("  --getenv   [var]                get the value of the specified "
         "variable in server environment.\n");
  printf("  --setenv   [var]                set the specified flag to server "
         "environment.\n");
  printf("  --unsetenv   [var]              remove the specified flag from "
         "server environment.\n");
  printf("  --get_label      || -a [id]     show the job label. Of the last "
         "added, if not specified.\n");
  printf("  --full_cmd       || -F [id]     show full command. Of the last "
         "added, if not specified.\n");
  printf(
      "  --count_running  || -R          return the number of running jobs\n");
  printf(
      "  --last_queue_id  || -q          show the job ID of the last added.\n");
  printf(
      "  --get_logdir                    get the path containing log files.\n");
  printf(
      "  --set_logdir [path]             set the path containing log files.\n");
  printf("  --plain                         list jobs in plain tab-separated "
         "texts.\n");
  printf("Actions:\n");
  printf("  -A           Show all users information\n");
  printf("  -X           Refresh the user configuration (only available for "
         "root)\n");
  printf("  -K           kill the task spooler server (only available for "
         "root)\n");
  printf("  -C           clear the list of finished jobs for current user\n");
  printf("  -l           show the job list (default action)\n");
  printf("  -S [num]     get/set the number of max simultaneous jobs of the "
         "server.  (only available for root)\n");
  printf("  -t [id]      \"tail -n 10 -f\" the output of the job. Last run if "
         "not specified.\n");
  printf("  -c [id]      like -t, but shows all the lines. Last run if not "
         "specified.\n");
  printf(
      "  -p [id]      show the PID of the job. Last run if not specified.\n");
  printf("  -o [id]      show the output file. Of last job run, if not "
         "specified.\n");
  printf("  -i [id]      show job information. Of last job run, if not "
         "specified.\n");
  printf("  -s [id]      show the job state. Of the last added, if not "
         "specified.\n");
  printf("  -r [id]      remove a job. The last added, if not specified.\n");
  printf("  -w [id]      wait for a job. The last added, if not specified.\n");
  printf("  -k [id]      send SIGTERM to the job process group. The last run, "
         "if not specified.\n");
  printf("  -T           send SIGTERM to all running job groups.  (only "
         "available for root)\n");
  printf(
      "  -u [id]      put that job first. The last added, if not specified.\n");
  printf("  -U <id-id>   swap two jobs in the queue.\n");
  printf("  -h           show this help\n");
  printf("  -V           show the program version\n");
  printf("Options adding jobs:\n");
  printf("  -B           in case of full clients on the server, quit instead "
         "of waiting.\n");
  printf("  -n           don't store the output of the command.\n");
  printf("  -E           Keep stderr apart, in a name like the output file, "
         "but adding '.e'.\n");
  printf("  -O           Set name of the log file (without any path).\n");
  printf("  -z           gzip the stored output (if not -n).\n");
  printf("  -f           don't fork into background.\n");
  printf("  -m           send the output by e-mail (uses sendmail).\n");
  printf("  -d           the job will be run after the last job ends.\n");
  printf(
      "  -D <id,...>  the job will be run after the job of given IDs ends.\n");
  printf("  -W <id,...>  the job will be run after the job of given IDs ends "
         "well (exit code 0).\n");
  printf("  -L [label]   name this task with a label, to be distinguished on "
         "listing.\n");
  printf("  -N [num]     number of slots required by the job (1 default).\n");
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
  } else
    sprintf(getopt_env, "POSIXLY_CORRECT=%s", old_getopt_env);
}

static void get_terminal_width() {
  struct winsize ws;
  ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws);
  term_width = ws.ws_col;
}

int main(int argc, char **argv) {
  int errorlevel = 0;
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

  if (command_line.need_server) {
    ensure_server_up();
    c_check_version();
  }

  switch (command_line.request) {
  case c_REFRESH_USER:
    if (client_uid == 0) {
      c_refresh_user();
      c_wait_server_lines();
    } else {
      printf("Only the root can shutdown the task-spooler server\n");
    }

    break;
  case c_STOP_USER: {
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
    printf("Stop user ID: %d\n", stop_uid);
    c_stop_user(stop_uid);
    c_wait_server_lines();
  } break;
  case c_CONT_USER: {
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
    printf("Resume user ID: %d\n", cont_uid);
    c_cont_user(cont_uid);
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
    c_send_max_slots(command_line.max_slots);
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
  }

  if (command_line.need_server) {
    close(server_socket);
  }
  free(command_line.depend_on);

  return errorlevel;
}

/*
    Task Spooler - a task queue system for the unix user
    Copyright (C) 2007-2013  Lluís Batlle i Rossell

    Please find the license in the provided COPYING file.
*/

enum { CMD_LEN = 500, PROTOCOL_VERSION = 730 };

enum MsgTypes {
  KILL_SERVER,
  NEWJOB,
  NEWJOB_OK,
  RUNJOB,
  RUNJOB_OK,
  ENDJOB,
  LIST,
  LIST_ALL,
  LIST_LINE,
  REFRESH_USERS,
  HOLD_JOB,
  RESTART_JOB,
  LOCK_SERVER,
  UNLOCK_SERVER,
  STOP_USER,
  CONT_USER,
  CLEAR_FINISHED,
  ASK_OUTPUT,
  ANSWER_OUTPUT,
  REMOVEJOB,
  REMOVEJOB_OK,
  WAITJOB,
  WAIT_RUNNING_JOB,
  WAITJOB_OK,
  URGENT,
  URGENT_OK,
  GET_STATE,
  ANSWER_STATE,
  SWAP_JOBS,
  SWAP_JOBS_OK,
  INFO,
  INFO_DATA,
  SET_MAX_SLOTS,
  GET_MAX_SLOTS,
  GET_MAX_SLOTS_OK,
  GET_VERSION,
  VERSION,
  NEWJOB_NOK,
  NEWJOB_PID_NOK,
  COUNT_RUNNING,
  GET_LABEL,
  LAST_ID,
  KILL_ALL,
  GET_CMD,
  GET_LOGDIR,
  SET_LOGDIR,
  GET_ENV,
  SET_ENV,
  UNSET_ENV
};

enum Request {
  c_QUEUE,
  c_TAIL,
  c_KILL_SERVER,
  c_LIST,
  c_LIST_ALL,
  c_DAEMON,
  c_REFRESH_USER,
  c_STOP_USER,
  c_CONT_USER,
  c_LOCK_SERVER,
  c_UNLOCK_SERVER,
  c_HOLD_JOB,
  c_RESTART_JOB,
  c_CLEAR_FINISHED,
  c_SHOW_HELP,
  c_SHOW_VERSION,
  c_CAT,
  c_SHOW_OUTPUT_FILE,
  c_SHOW_PID,
  c_REMOVEJOB,
  c_WAITJOB,
  c_URGENT,
  c_GET_STATE,
  c_SWAP_JOBS,
  c_INFO,
  c_SET_MAX_SLOTS,
  c_GET_MAX_SLOTS,
  c_KILL_JOB,
  c_COUNT_RUNNING,
  c_GET_LABEL,
  c_LAST_ID,
  c_KILL_ALL,
  c_SHOW_CMD,
  c_GET_LOGDIR,
  c_SET_LOGDIR,
  c_GET_ENV,
  c_SET_ENV,
  c_UNSET_ENV
};

struct CommandLine {
  enum Request request;
  int plain_list;
  int need_server;
  int store_output;
  int stderr_apart;
  int should_go_background;
  int should_keep_finished;
  int send_output_by_mail;
  int gzip;
  int *depend_on; /* -1 means depend on previous */
  int depend_on_size;
  int max_slots; /* How many jobs to run at once */
  int jobid;     /* When queuing a job, main.c will fill it automatically from
                    the server answer to NEWJOB */
  int jobid2;
  int wait_enqueuing;
  struct {
    char **array;
    int num;
  } command;
  char *label;
  char *logfile;
  char *outfile;
  int num_slots;      /* Slots for the job to use. Default 1 */
  int taskpid;       /* to restore task by pid */
  int require_elevel; /* whether requires error level of dependencies or not */
  long start_time;
};

enum ProcessType { CLIENT, SERVER };

extern struct CommandLine command_line;
extern enum ProcessType process_type;
extern int server_socket; /* Used in the client */
extern char *logdir;
extern int term_width;

struct Msg;

enum Jobstate { QUEUED, RUNNING, FINISHED, SKIPPED, HOLDING_CLIENT, RELINK };

struct Msg {
  enum MsgTypes type;
  int jobid;
  union {
    struct {
      int command_size;
      int store_output;
      int should_keep_finished;
      int label_size;
      int env_size;
      int depend_on_size;
      int wait_enqueuing;
      int num_slots;
      int taskpid;
      long start_time;
    } newjob;
    struct {
      int ofilename_size;
      int store_output;
      int pid;
    } output;
    struct Result {
      int errorlevel;
      int died_by_signal;
      int signal;
      float user_ms;
      float system_ms;
      float real_ms;
      int skipped;
    } result;
    int size;
    enum Jobstate state;
    struct {
      int jobid1;
      int jobid2;
    } swap;
    int last_errorlevel;
    int max_slots;
    int version;
    int count_running;
    char *label;
    struct {
      int plain_list;
      int term_width;
    } list;
  } u;
};

struct Procinfo {
  char *ptr;
  int nchars;
  int allocchars;
  struct timeval enqueue_time;
  struct timeval start_time;
  struct timeval end_time;
};

struct Job {
  struct Job *next;
  int jobid;
  char *command;
  enum Jobstate state;
  struct Result result; /* Defined in msg.h */
  char *output_filename;
  int store_output;
  int pid;
  int ts_UID;
  int should_keep_finished;
  int *depend_on;
  int depend_on_size;
  int *notify_errorlevel_to;
  int notify_errorlevel_to_size;
  int dependency_errorlevel;
  char *label;
  struct Procinfo info;
  int num_slots;
};

enum ExitCodes {
  EXITCODE_OK = 0,
  EXITCODE_UNKNOWN_ERROR = -1,
  EXITCODE_QUEUE_FULL = 2,
  EXITCODE_RELINK_FAILED = 3
};

/* main.c */

struct Msg default_msg();

struct Result default_result();

/* client.c */
void c_new_job();

void c_list_jobs();

void c_shutdown_server();

void c_wait_server_lines();

void c_clear_finished();

int c_wait_server_commands();

void c_send_runjob_ok(const char *ofname, int pid);

int c_tail();

int c_cat();

void c_show_output_file();

void c_remove_job();

void c_show_pid();

void c_kill_job();

int c_wait_job();

int c_wait_running_job();

int c_wait_job_recv();

void c_move_urgent();

int c_wait_newjob_ok();

void c_get_state();

void c_swap_jobs();

void c_show_info();

void c_show_last_id();

char *build_command_string();

void c_send_max_slots(int max_slots);

void c_get_max_slots();

void c_check_version();

void c_get_count_running();

void c_show_label();

void c_kill_all_jobs();

void c_show_cmd();

void c_get_logdir();

void c_set_logdir();

char *get_logdir();

void c_get_env();

void c_set_env();

void c_unset_env();

/* jobs.c */
void s_list(int s, int ts_UID);
void s_list_all(int s);

void s_list_plain(int s);

int s_newjob(int s, struct Msg *m, int ts_UID);

void s_removejob(int jobid);

void job_finished(const struct Result *result, int jobid);

int next_run_job();

void s_mark_job_running(int jobid);

void s_clear_finished(int ts_UID);

void s_process_runjob_ok(int jobid, char *oname, int pid);

void s_send_output(int socket, int jobid);

int s_remove_job(int s, int *jobid, int client_uid);

void s_remove_notification(int s);

void check_notify_list(int jobid);

void s_wait_job(int s, int jobid);

void s_wait_running_job(int s, int jobid);

void s_move_urgent(int s, int jobid);

void s_send_state(int s, int jobid);

void s_swap_jobs(int s, int jobid1, int jobid2);

void s_count_running_jobs(int s);

void dump_jobs_struct(FILE *out);

void dump_notifies_struct(FILE *out);

void joblist_dump(int fd);

const char *jstate2string(enum Jobstate s);

void s_job_info(int s, int jobid);

void s_send_last_id(int s);

void s_send_runjob(int s, int jobid);

void s_set_max_slots(int s, int new_max_slots);

void s_get_max_slots(int s);

int job_is_running(int jobid);

int job_is_holding_client(int jobid);

int wake_hold_client();

void s_get_label(int s, int jobid);

void s_kill_all_jobs(int s);

void s_get_logdir(int s);

void s_set_logdir(const char *);

void s_get_env(int s, int size);

void s_set_env(int s, int size);

void s_unset_env(int s, int size);

/* server.c */
void server_main(int notify_fd, char *_path);

void dump_conns_struct(FILE *out);

void s_send_cmd(int s, int jobid);

/* server_start.c */
int try_connect(int s);

void wait_server_up(int fd);

int ensure_server_up(int);

void notify_parent(int fd);

void create_socket_path(char **path);

/* execute.c */
int run_job(int jobid, struct Result *res);

/* client_run.c */
void c_run_tail(const char *filename);

void c_run_cat(const char *filename);

/* mail.c */
void send_mail(int jobid, int errorlevel, const char *ofname,
               const char *command);

void hook_on_finish(int jobid, int errorlevel, const char *ofname,
                    const char *command);

/* error.c */
void error(const char *str, ...);

void warning(const char *str, ...);

void debug(const char *str, ...);

/* signals.c */
void ignore_sigpipe();

void restore_sigmask();

void block_sigint();

void unblock_sigint_and_install_handler();

/* msg.c */
void send_bytes(int fd, const char *data, int bytes);

int recv_bytes(int fd, char *data, int bytes);

void send_msg(int fd, const struct Msg *m);

int recv_msg(int fd, struct Msg *m);

void send_ints(int fd, const int *data, int num);

int *recv_ints(int fd, int *num);

/* msgdump.c */
void msgdump(FILE *, const struct Msg *m);

/* error.c */
void error_msg(const struct Msg *m, const char *str, ...);

void warning_msg(const struct Msg *m, const char *str, ...);

/* list.c */
char *joblist_headers();

char *joblist_line(const struct Job *p);

char *joblist_line_plain(const struct Job *p);

char *joblistdump_torun(const struct Job *p);

char *joblistdump_headers();

char *time_rep(float *t);

/* print.c */
int fd_nprintf(int fd, int maxsize, const char *fmt, ...);

/* info.c */

void pinfo_dump(const struct Procinfo *p, int fd);

void pinfo_addinfo(struct Procinfo *p, int maxsize, const char *line, ...);

void pinfo_free(struct Procinfo *p);

int pinfo_size(const struct Procinfo *p);

void pinfo_set_enqueue_time(struct Procinfo *p);

void pinfo_set_start_time(struct Procinfo *p);

void pinfo_set_end_time(struct Procinfo *p);

float pinfo_time_until_now(const struct Procinfo *p);

float pinfo_time_run(const struct Procinfo *p);

void pinfo_init(struct Procinfo *p);

/* env.c */
char *get_environment();

/* tail.c */
int tail_file(const char *fname, int last_lines);

/* user.c */
static const int root_UID = 0;
char *get_kill_sh_path();
void read_user_file(const char *path);
int get_tsUID(int uid);
void c_refresh_user();
const char *get_user_path();
const char *set_server_logfile();
void write_logfile(const struct Job *p);
int get_env(const char *env, int v0);
long str2int(const char *str);
void debug_write(const char *str);
const char *uid2user_name(int uid);
int read_first_jobid_from_logfile(const char *path);
void kill_pid(int ppid, const char *signal);
char* linux_cmd(char* CMD, char* out, int out_size);
void check_running_task(int pid);

/* locker */
int user_locker;
time_t locker_time;
int jobsort_flag;
// FILE* dbf; // # DEBUG
int check_ifsleep(int pid);
int check_running_dead(int jobid);

/* jobs.c */
void s_user_status_all(int s);
void s_user_status(int s, int i);
void s_refresh_users(int s);
int s_get_job_tsUID(int jobid);
void s_stop_all_users(int s);
void s_stop_user(int s, int uid);
void s_cont_user(int s, int uid);
void s_cont_all_users(int s);
void s_hold_job(int s, int jobid, int uid);
void s_restart_job(int s, int jobid, int uid);
void s_lock_server(int s, int uid);
void s_unlock_server(int s, int uid);
int s_check_locker(int s, int uid);
void s_set_jobids(int i);
void s_sort_jobs();
int s_check_relink(int s, struct Msg *m, int ts_UID);


/* client.c */
void c_list_jobs_all();
void c_stop_user(int uid);
void c_cont_user(int uid);
void c_hold_job(int jobid);
int c_lock_server();
int c_unlock_server();
void c_restart_job(int jobid);

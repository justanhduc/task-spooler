/*
    Task Spooler - a task queue system for the unix user
    Copyright (C) 2007-2013  Lluís Batlle i Rossell

    Please find the license in the provided COPYING file.
*/
#include <stdio.h>
#include <sys/time.h>

enum {
    CMD_LEN = 500,
    PROTOCOL_VERSION = 730
};

enum MsgTypes {
    KILL_SERVER,
    NEWJOB,
    NEWJOB_OK,
    RUNJOB,
    RUNJOB_OK,
    ENDJOB,
    LIST,
    LIST_GPU,
    LIST_LINE,
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
    COUNT_RUNNING,
    GET_LABEL,
    LAST_ID,
    KILL_ALL,
    GET_CMD,
    GET_ENV,
    SET_ENV,
    UNSET_ENV,
    SET_FREE_PERC,
    GET_FREE_PERC,
    GET_LOGDIR,
    SET_LOGDIR
};

enum Request {
    c_QUEUE,
    c_TAIL,
    c_KILL_SERVER,
    c_LIST,
    c_LIST_GPU,
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
    c_GET_ENV,
    c_SET_ENV,
    c_UNSET_ENV,
    c_SET_FREE_PERC,
    c_GET_FREE_PERC,
    c_GET_LOGDIR,
    c_SET_LOGDIR
};

enum ListFormat {
    DEFAULT,
    JSON,
    TAB
};

struct CommandLine {
    enum Request request;
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
    int jobid; /* When queuing a job, main.c will fill it automatically from
                  the server answer to NEWJOB */
    int jobid2;
    int wait_enqueuing;
    struct {
        char **array;
        int num;
    } command;
    char *label;
    int num_slots; /* Slots for the job to use. Default 1 */
    int require_elevel;  /* whether requires error level of dependencies or not */
    int gpus;
    int *gpu_nums;
    int wait_free_gpus;
    char *logfile;
    enum ListFormat list_format;
};

enum Process_type {
    CLIENT,
    SERVER
};

extern struct CommandLine command_line;
extern enum Process_type process_type;
extern int server_socket; /* Used in the client */
extern char* logdir;
extern int term_width;

struct Msg;

enum Jobstate {
    QUEUED,
    ALLOCATING,
    RUNNING,
    FINISHED,
    SKIPPED,
    HOLDING_CLIENT
};

struct Msg {
    enum MsgTypes type;

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
            int gpus;
            int wait_free_gpus;
        } newjob;
        struct {
            int ofilename_size;
            int store_output;
            int pid;
        } output;
        int jobid;
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
        int term_width;
        enum ListFormat list_format;
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
    int should_keep_finished;
    int *depend_on;
    int depend_on_size;
    int *notify_errorlevel_to;
    int notify_errorlevel_to_size;
    int dependency_errorlevel;
    char *label;
    struct Procinfo info;
    int num_slots;
    int num_gpus;
    int *gpu_ids;
    int wait_free_gpus;
};

enum ExitCodes {
    EXITCODE_OK = 0,
    EXITCODE_UNKNOWN_ERROR = -1,
    EXITCODE_QUEUE_FULL = 2
};


/* main.c */
int strtok_int(char* str, char* delim, int* ids);

struct Msg default_msg();

struct Result default_result();

/* client.c */
void c_new_job();

void c_list_jobs();

void c_list_gpu_jobs();

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

void c_get_env();

void c_set_env();

void c_unset_env();

void c_set_free_percentage();

void c_get_free_percentage();

void c_get_logdir();

void c_set_logdir();

char* get_logdir();

/* jobs.c */
void s_list(int s, enum ListFormat listFormat);

#ifndef CPU
void s_list_gpu(int s);
#endif

int s_newjob(int s, struct Msg *m);

void s_removejob(int jobid);

void job_finished(const struct Result *result, int jobid);

int next_run_job();

void s_mark_job_running(int jobid);

void s_clear_finished();

void s_process_runjob_ok(int jobid, char *oname, int pid);

void s_send_output(int socket, int jobid);

int s_remove_job(int s, int *jobid);

void s_remove_notification(int s);

void check_notify_list(int jobid);

void s_wait_job(int s, int jobid);

void s_wait_running_job(int s, int jobid);

void s_move_urgent(int s, int jobid);

void s_send_state(int s, int jobid);

void s_swap_jobs(int s, int jobid1, int jobid2);

void s_count_running_jobs(int s);

int s_count_allocating_jobs();

void dump_jobs_struct(FILE *out);

void dump_notifies_struct(FILE *out);

void joblist_dump(int fd);

const char *jstate2string(enum Jobstate s);

void s_job_info(int s, int jobid);

void s_send_last_id(int s);

void s_send_runjob(int s, int jobid);

void s_set_max_slots(int new_max_slots);

void s_get_max_slots(int s);

int job_is_running(int jobid);

int job_is_holding_client(int jobid);

int wake_hold_client();

void s_send_label(int s, int jobid);

void s_kill_all_jobs(int s);

void s_get_env(int s, int size);

void s_set_env(int s, int size);

void s_unset_env(int s, int size);

void s_set_free_percentage(int new_percentage);

void s_get_free_percentage(int s);

void s_get_logdir(int s);

void s_set_logdir(const char*);

/* server.c */
void server_main(int notify_fd, char *_path);

void dump_conns_struct(FILE *out);

void s_send_cmd(int s, int jobid);

/* server_start.c */
int try_connect(int s);

void wait_server_up(int fd);

int ensure_server_up();

void notify_parent(int fd);

void create_socket_path(char **path);

/* execute.c */
int run_job(struct Result *res);

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

char *jobgpulist_header();

char *joblist_line(const struct Job *p);

char *joblist_line_plain(const struct Job *p);

char *joblistdump_torun(const struct Job *p);

char *joblistdump_headers();

#ifndef CPU
char *jobgpulist_line(const struct Job *p);
#endif

char *time_rep(float* t);

/* print.c */
int fd_nprintf(int fd, int maxsize, const char *fmt, ...);

char *ints_to_chars(int *array, int n, const char *delim);

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

#ifndef CPU
/* gpu.c */
int *getGpuList(int *num);

void initGPU();

void broadcastUsedGpus(int num, const int *list);

void broadcastFreeGpus(int num, const int *list);

int isInUse(int id);

void setFreePercentage(int percent);

int getFreePercentage();

void cleanupGpu();
#endif
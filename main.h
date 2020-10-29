/*
    Task Spooler - a task queue system for the unix user
    Copyright (C) 2007-2013  Lluís Batlle i Rossell

    Please find the license in the provided COPYING file.
*/
enum
{
    CMD_LEN=500,
    PROTOCOL_VERSION=730
};

enum msg_types
{
    KILL_SERVER,
    NEWJOB,
    NEWJOB_OK,
    RUNJOB,
    RUNJOB_OK,
    ENDJOB,
    LIST,
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
    NEWJOB_NOK
};

enum Request
{
    c_QUEUE,
    c_TAIL,
    c_KILL_SERVER,
    c_LIST,
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
    c_KILL_JOB
};

struct Command_line {
    enum Request request;
    int need_server;
    int store_output;
    int stderr_apart;
    int should_go_background;
    int should_keep_finished;
    int send_output_by_mail;
    int gzip;
    int do_depend;
    int depend_on; /* -1 means depend on previous */
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
};

enum Process_type {
    CLIENT,
    SERVER
};

extern struct Command_line command_line;
extern int server_socket;
extern enum Process_type process_type;
extern int server_socket; /* Used in the client */

struct msg;

enum Jobstate
{
    QUEUED,
    RUNNING,
    FINISHED,
    SKIPPED,
    HOLDING_CLIENT
};

struct msg
{
    enum msg_types type;

    union
    {
        struct {
            int command_size;
            int store_output;
            int should_keep_finished;
            int label_size;
            int env_size;
            int do_depend;
            int depend_on; /* -1 means depend on previous */
            int wait_enqueuing;
            int num_slots;
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
    } u;
};

struct Procinfo
{
    char *ptr;
    int nchars;
    int allocchars;
    struct timeval enqueue_time;
    struct timeval start_time;
    struct timeval end_time;
};

struct Job
{
    struct Job *next;
    int jobid;
    char *command;
    enum Jobstate state;
    struct Result result; /* Defined in msg.h */
    char *output_filename;
    int store_output;
    int pid;
    int should_keep_finished;
    int do_depend;
    int depend_on;
    int *notify_errorlevel_to;
    int notify_errorlevel_to_size;
    int dependency_errorlevel;
    char *label;
    struct Procinfo info;
    int num_slots;
};

enum ExitCodes
{
    EXITCODE_OK            =  0,
    EXITCODE_UNKNOWN_ERROR = -1,
    EXITCODE_QUEUE_FULL    = 2
};


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
char *build_command_string();
void c_send_max_slots(int max_slots);
void c_get_max_slots();
void c_check_version();

/* jobs.c */
void s_list(int s);
int s_newjob(int s, struct msg *m);
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
void dump_jobs_struct(FILE *out);
void dump_notifies_struct(FILE *out);
void joblist_dump(int fd);
const char * jstate2string(enum Jobstate s);
void s_job_info(int s, int jobid);
void s_send_runjob(int s, int jobid);
void s_set_max_slots(int new_max_slots);
void s_get_max_slots(int s);
int job_is_running(int jobid);
int job_is_holding_client(int jobid);
int wake_hold_client();

/* server.c */
void server_main(int notify_fd, char *_path);
void dump_conns_struct(FILE *out);

/* server_start.c */
int try_connect(int s);
void wait_server_up();
int ensure_server_up();
void notify_parent(int fd);
void create_socket_path(char **path);

/* execute.c */
int run_job();

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

/* signals.c */
void ignore_sigpipe();
void restore_sigmask();
void block_sigint();
void unblock_sigint_and_install_handler();

/* msg.c */
void send_bytes(const int fd, const char *data, int bytes);
int recv_bytes(const int fd, char *data, int bytes);
void send_msg(const int fd, const struct msg *m);
int recv_msg(const int fd, struct msg *m);

/* msgdump.c */
void msgdump(FILE *, const struct msg *m);

/* error.c */
void error_msg(const struct msg *m, const char *str, ...);
void warning_msg(const struct msg *m, const char *str, ...);

/* list.c */
char * joblist_headers();
char * joblist_line(const struct Job *p);
char * joblistdump_torun(const struct Job *p);
char * joblistdump_headers();

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
char * get_environment();

/* tail.c */
int tail_file(const char *fname, int last_lines);

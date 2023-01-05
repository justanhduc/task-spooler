/*
    Task Spooler - a task queue system for the unix user
    Copyright (C) 2007-2009  Lluís Batlle i Rossell

    Please find the license in the provided COPYING file.
*/
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <signal.h>
#include <time.h>

#include "main.h"

static void c_end_of_job(const struct Result *res);

static void c_wait_job_send();

static void c_wait_running_job_send();

char *build_command_string() {
    int size;
    int i;
    int num;
    char **array;
    char *commandstring;

    size = 0;
    num = command_line.command.num;
    array = command_line.command.array;

    /* Count bytes needed */
    for (i = 0; i < num; ++i) {
        /* The '1' is for spaces, and at the last i,
         * for the null character */
        size = size + strlen(array[i]) + 1;
    }

    /* Alloc */
    commandstring = (char *) malloc(size);
    if (commandstring == NULL)
        error("Error in malloc for commandstring");

    /* Build the command */
    strcpy(commandstring, array[0]);
    for (i = 1; i < num; ++i) {
        strcat(commandstring, " ");
        strcat(commandstring, array[i]);
    }

    return commandstring;
}

void c_new_job() {
    struct Msg m = default_msg();
    char *new_command;
    char *myenv;

    m.type = NEWJOB;

    new_command = build_command_string();

    myenv = get_environment();

    /* global */
    m.u.newjob.command_size = strlen(new_command) + 1; /* add null */
    if (myenv)
        m.u.newjob.env_size = strlen(myenv) + 1; /* add null */
    else
        m.u.newjob.env_size = 0;
    if (command_line.label)
        m.u.newjob.label_size = strlen(command_line.label) + 1; /* add null */
    else
        m.u.newjob.label_size = 0;
    m.u.newjob.store_output = command_line.store_output;
    m.u.newjob.depend_on_size = command_line.depend_on_size;
    m.u.newjob.should_keep_finished = command_line.should_keep_finished;
    m.u.newjob.command_size = strlen(new_command) + 1; /* add null */
    m.u.newjob.wait_enqueuing = command_line.wait_enqueuing;
    m.u.newjob.num_slots = command_line.num_slots;
    m.u.newjob.gpus = command_line.gpus;
    m.u.newjob.wait_free_gpus = command_line.wait_free_gpus;

    /* Send the message */
    send_msg(server_socket, &m);

    /* send GPU IDs */
    if (!command_line.wait_free_gpus)
        send_ints(server_socket, command_line.gpu_nums, command_line.gpus);

    /* send dependencies */
    if (command_line.depend_on_size)
        send_ints(server_socket, command_line.depend_on, command_line.depend_on_size);

    /* Send the command */
    send_bytes(server_socket, new_command, m.u.newjob.command_size);

    /* Send the label */
    send_bytes(server_socket, command_line.label, m.u.newjob.label_size);

    /* Send the environment */
    send_bytes(server_socket, myenv, m.u.newjob.env_size);

    free(new_command);
    free(myenv);
    free(command_line.depend_on);
}

int c_wait_newjob_ok() {
    struct Msg m = default_msg();
    int res;

    res = recv_msg(server_socket, &m);
    if (res == -1)
        error("Error in wait_newjob_ok");
    if (m.type == NEWJOB_NOK) {
        fprintf(stderr, "Error, queue full\n");
        exit(EXITCODE_QUEUE_FULL);
    }
    if (m.type != NEWJOB_OK)
        error("Error getting the newjob_ok");

    return m.u.jobid;
}

int c_wait_server_commands() {
    struct Msg m = default_msg();
    int res;

    while (1) {
        res = recv_msg(server_socket, &m);
        if (res == -1)
            error("Error in wait_server_commands");

        if (res == 0)
            break;
        if (res != sizeof(m))
            error("Error in wait_server_commands");
        if (m.type == RUNJOB) {
            int num_gpus;
            int *freeGpuList = NULL;
            struct Result result = default_result();

            freeGpuList = recv_ints(server_socket, &num_gpus);
            result.skipped = 0;
            if (command_line.depend_on_size && command_line.require_elevel && m.u.last_errorlevel != 0) {
                result.errorlevel = -1;
                result.user_ms = 0.f;
                result.system_ms = 0.f;
                result.real_ms = 0.f;
                result.skipped = 1;
                c_send_runjob_ok(0, -1);
            } else {
                if (command_line.gpus) {
                    char tmp[1024];
                    char* tmp2 = ints_to_chars(freeGpuList, num_gpus, ",");
                    strcpy(tmp, "CUDA_VISIBLE_DEVICES=");
                    strcat(tmp, tmp2);
                    putenv(tmp);
                    free(freeGpuList);
                    free(tmp2);
                } else {
                    putenv("CUDA_VISIBLE_DEVICES=-1");
                }

                run_job(&result);
            }

            c_end_of_job(&result);
            return result.errorlevel;
        }
    }
    return -1;
}

void c_wait_server_lines() {
    struct Msg m = default_msg();
    int res;

    while (1) {
        res = recv_msg(server_socket, &m);
        if (res == -1)
            error("Error in wait_server_lines");

        if (res == 0)
            break;
        if (res != sizeof(m))
            error("Error in wait_server_lines 2");
        if (m.type == LIST_LINE) {
            char *buffer;
            buffer = (char *) malloc(m.u.size);
            recv_bytes(server_socket, buffer, m.u.size);
            printf("%s", buffer);
            free(buffer);
        }
    }
}

void c_list_jobs() {
    struct Msg m = default_msg();

    m.type = LIST;
    m.u.term_width = term_width;
    m.u.list_format = command_line.list_format;
    send_msg(server_socket, &m);
}

void c_list_gpu_jobs() {
    struct Msg m = default_msg();
    m.type = LIST_GPU;
    send_msg(server_socket, &m);
}

/* Exits if wrong */
void c_check_version() {
    struct Msg m = default_msg();
    int res;

    m.type = GET_VERSION;
    /* Double send, so an old ts will answer for sure at least once */
    send_msg(server_socket, &m);
    send_msg(server_socket, &m);

    /* Set up a 2 second timeout to receive the
    version msg. */

    res = recv_msg(server_socket, &m);
    if (res == -1)
        error("Error calling recv_msg in c_check_version");
    if (m.type != VERSION || m.u.version != PROTOCOL_VERSION) {
        printf("Wrong server version. Received %i, expecting %i\n",
               m.u.version, PROTOCOL_VERSION);

        error("Wrong server version. Received %i, expecting %i",
              m.u.version, PROTOCOL_VERSION);
    }

    /* Receive also the 2nd send_msg if we got the right version */
    res = recv_msg(server_socket, &m);
    if (res == -1)
        error("Error calling the 2nd recv_msg in c_check_version");
}

void c_show_info() {
    struct Msg m = default_msg();
    int res;

    m.type = INFO;
    m.u.jobid = command_line.jobid;

    send_msg(server_socket, &m);

    while (1) {
        res = recv_msg(server_socket, &m);
        if (res == -1)
            error("Error in wait_server_lines");

        if (res == 0)
            break;

        if (res != sizeof(m))
            error("Error in wait_server_lines 2");
        if (m.type == LIST_LINE) {
            char *buffer;
            buffer = (char *) malloc(m.u.size);
            recv_bytes(server_socket, buffer, m.u.size);
            printf("%s", buffer);
            free(buffer);
        }
        if (m.type == INFO_DATA) {
            char *buffer;
            enum {
                DSIZE = 1000
            };

            /* We're going to output data using the stdout fd */
            fflush(stdout);
            buffer = (char *) malloc(DSIZE);
            do {
                res = recv(server_socket, buffer, DSIZE, 0);
                if (res > 0)
                    write(1, buffer, res);
            } while (res > 0);
            free(buffer);
        }
    }
}

void c_show_last_id() {
    struct Msg m = default_msg();
    int res;

    m.type = LAST_ID;
    send_msg(server_socket, &m);

    /* Receive the answer */
    res = recv_msg(server_socket, &m);
    if (res != sizeof(m))
        error("Error in get_output_file");

    switch (m.type) {
        case LAST_ID:
            printf("%d\n", m.u.jobid);
        default:
            warning("Wrong internal message in get_output_file line size");
    }
}

void c_send_runjob_ok(const char *ofname, int pid) {
    struct Msg m = default_msg();

    /* Prepare the message */
    m.type = RUNJOB_OK;
    if (ofname) /* ofname == 0, skipped execution */
        m.u.output.store_output = command_line.store_output;
    else
        m.u.output.store_output = 0;
    m.u.output.pid = pid;
    if (m.u.output.store_output)
        m.u.output.ofilename_size = strlen(ofname) + 1;
    else
        m.u.output.ofilename_size = 0;

    send_msg(server_socket, &m);

    /* Send the filename */
    if (command_line.store_output)
        send_bytes(server_socket, ofname, m.u.output.ofilename_size);
}

static void c_end_of_job(const struct Result *res) {
    struct Msg m = default_msg();

    m.type = ENDJOB;
    m.u.result = *res; /* struct copy */

    send_msg(server_socket, &m);
}

void c_shutdown_server() {
    struct Msg m = default_msg();

    m.type = KILL_SERVER;
    send_msg(server_socket, &m);
}

void c_clear_finished() {
    struct Msg m = default_msg();

    m.type = CLEAR_FINISHED;
    send_msg(server_socket, &m);
}

static char *get_output_file(int *pid) {
    struct Msg m = default_msg();
    int res;
    char *string = 0;

    /* Send the request */
    m.type = ASK_OUTPUT;
    m.u.jobid = command_line.jobid;
    send_msg(server_socket, &m);

    /* Receive the answer */
    res = recv_msg(server_socket, &m);
    if (res != sizeof(m))
        error("Error in get_output_file");
    switch (m.type) {
        case ANSWER_OUTPUT:
            if (m.u.output.store_output) {
                /* Receive the output file name */
                string = 0;
                if (m.u.output.ofilename_size > 0) {
                    string = (char *) malloc(m.u.output.ofilename_size);
                    recv_bytes(server_socket, string, m.u.output.ofilename_size);
                }
                *pid = m.u.output.pid;
                return string;
            }
            *pid = m.u.output.pid;
            return 0;
            /* WILL NOT GO FURTHER */
        case LIST_LINE: /* Only ONE line accepted */
            string = (char *) malloc(m.u.size);
            res = recv_bytes(server_socket, string, m.u.size);
            if (res != m.u.size)
                error("Error in get_output_file line size");
            fprintf(stderr, "Error in the request: %s",
                    string);
            free(string);
            exit(-1);
            /* WILL NOT GO FURTHER */
        default:
            warning("Wrong internal message in get_output_file line size");
    }
    /* This will never be reached */
    return 0;
}

int c_tail() {
    char *str;
    int pid;
    str = get_output_file(&pid);
    if (str == 0) {
        fprintf(stderr, "The output is not stored. Cannot tail.\n");
        exit(-1);
    }

    c_wait_running_job_send();

    return tail_file(str, 10 /* Last lines to show */);
}

int c_cat() {
    char *str;
    int pid;
    str = get_output_file(&pid);
    if (str == 0) {
        fprintf(stderr, "The output is not stored. Cannot cat.\n");
        exit(-1);
    }
    c_wait_running_job_send();

    return tail_file(str, -1 /* All the lines */);
}

void c_show_output_file() {
    char *str;
    int pid;
    /* This will exit if there is any error */
    str = get_output_file(&pid);
    if (str == 0) {
        fprintf(stderr, "The output is not stored.\n");
        exit(-1);
    }
    printf("%s\n", str);
    free(str);
}

void c_show_pid() {
    int pid;
    /* This will exit if there is any error */
    get_output_file(&pid);
    printf("%i\n", pid);
}

void c_kill_job() {
    int pid = 0;
    /* This will exit if there is any error */
    get_output_file(&pid);

    if (pid == -1 || pid == 0) {
        fprintf(stderr, "Error: strange PID received: %i\n", pid);
        exit(-1);
    }

    /* Send SIGTERM to the process group, as pid is for process group */
    kill(-pid, SIGTERM);
}

void c_kill_all_jobs() {
    struct Msg m = default_msg();
    int res;

    /* Send the request */
    m.type = KILL_ALL;
    send_msg(server_socket, &m);

    /* Receive the answer */
    res = recv_msg(server_socket, &m);
    if (res != sizeof(m))
        error("Error in kill_all");
    switch (m.type) {
        case COUNT_RUNNING:
            for (int i = 0; i < m.u.count_running; ++i) {
                int pid;
                res = recv(server_socket, &pid, sizeof(int), 0);
                if (res != sizeof(int))
                    error("Error in receiving PID kill_all");
                kill(-pid, SIGTERM);
            }
            return;
        default:
            warning("Wrong internal message in kill_all");
    }
}

void c_remove_job() {
    struct Msg m = default_msg();
    int res;
    char *string = 0;

    /* Send the request */
    m.type = REMOVEJOB;
    m.u.jobid = command_line.jobid;
    send_msg(server_socket, &m);

    /* Receive the answer */
    res = recv_msg(server_socket, &m);
    if (res != sizeof(m))
        error("Error in remove_job");
    switch (m.type) {
        case REMOVEJOB_OK:
            return;
            /* WILL NOT GO FURTHER */
        case LIST_LINE: /* Only ONE line accepted */
            string = (char *) malloc(m.u.size);
            res = recv_bytes(server_socket, string, m.u.size);
            fprintf(stderr, "Error in the request: %s",
                    string);
            free(string);
            exit(-1);
            /* WILL NOT GO FURTHER */
        default:
            warning("Wrong internal message in remove_job");
    }
    /* This will never be reached */
}

int c_wait_job_recv() {
    struct Msg m = default_msg();
    int res;
    char *string = 0;

    /* Receive the answer */
    res = recv_msg(server_socket, &m);
    if (res != sizeof(m))
        error("Error in wait_job");
    switch (m.type) {
        case WAITJOB_OK:
            return m.u.result.errorlevel;
            /* WILL NOT GO FURTHER */
        case LIST_LINE: /* Only ONE line accepted */
            string = (char *) malloc(m.u.size);
            res = recv_bytes(server_socket, string, m.u.size);
            if (res != m.u.size)
                error("Error in wait_job - line size");
            fprintf(stderr, "Error in the request: %s",
                    string);
            free(string);
            exit(-1);
            /* WILL NOT GO FURTHER */
        default:
            warning("Wrong internal message in c_wait_job");
    }
    /* This will never be reached */
    return -1;
}

static void c_wait_job_send() {
    struct Msg m = default_msg();

    /* Send the request */
    m.type = WAITJOB;
    m.u.jobid = command_line.jobid;
    send_msg(server_socket, &m);
}

static void c_wait_running_job_send() {
    struct Msg m = default_msg();

    /* Send the request */
    m.type = WAIT_RUNNING_JOB;
    m.u.jobid = command_line.jobid;
    send_msg(server_socket, &m);
}

/* Returns the errorlevel */
int c_wait_job() {
    c_wait_job_send();
    return c_wait_job_recv();
}

/* Returns the errorlevel */
int c_wait_running_job() {
    c_wait_running_job_send();
    return c_wait_job_recv();
}

void c_send_max_slots(int max_slots) {
    struct Msg m = default_msg();

    /* Send the request */
    m.type = SET_MAX_SLOTS;
    m.u.max_slots = command_line.max_slots;
    send_msg(server_socket, &m);
}

void c_get_max_slots() {
    struct Msg m = default_msg();
    int res;

    /* Send the request */
    m.type = GET_MAX_SLOTS;
    m.u.max_slots = command_line.max_slots;
    send_msg(server_socket, &m);

    /* Receive the answer */
    res = recv_msg(server_socket, &m);
    if (res != sizeof(m))
        error("Error in move_urgent");
    switch (m.type) {
        case GET_MAX_SLOTS_OK:
            printf("%i\n", m.u.max_slots);
            return;
        default:
            warning("Wrong internal message in get_max_slots");
    }
}

void c_move_urgent() {
    struct Msg m = default_msg();
    int res;
    char *string = 0;

    /* Send the request */
    m.type = URGENT;
    m.u.jobid = command_line.jobid;
    send_msg(server_socket, &m);

    /* Receive the answer */
    res = recv_msg(server_socket, &m);
    if (res != sizeof(m))
        error("Error in move_urgent");
    switch (m.type) {
        case URGENT_OK:
            return;
            /* WILL NOT GO FURTHER */
        case LIST_LINE: /* Only ONE line accepted */
            string = (char *) malloc(m.u.size);
            res = recv_bytes(server_socket, string, m.u.size);
            if (res != m.u.size)
                error("Error in move_urgent - line size");
            fprintf(stderr, "Error in the request: %s",
                    string);
            free(string);
            exit(-1);
            /* WILL NOT GO FURTHER */
        default:
            warning("Wrong internal message in move_urgent");
    }
    /* This will never be reached */
    return;
}

void c_get_state() {
    struct Msg m = default_msg();
    int res;
    char *string = 0;

    /* Send the request */
    m.type = GET_STATE;
    m.u.jobid = command_line.jobid;
    send_msg(server_socket, &m);

    /* Receive the answer */
    res = recv_msg(server_socket, &m);
    if (res != sizeof(m))
        error("Error in get_state - line size");
    switch (m.type) {
        case ANSWER_STATE:
            printf("%s\n", jstate2string(m.u.state));
            return;
            /* WILL NOT GO FURTHER */
        case LIST_LINE: /* Only ONE line accepted */
            string = (char *) malloc(m.u.size);
            res = recv_bytes(server_socket, string, m.u.size);
            if (res != m.u.size)
                error("Error in get_state - line size");
            fprintf(stderr, "Error in the request: %s",
                    string);
            free(string);
            exit(-1);
            /* WILL NOT GO FURTHER */
        default:
            warning("Wrong internal message in get_state");
    }
    /* This will never be reached */
    return;
}

void c_swap_jobs() {
    struct Msg m = default_msg();
    int res;
    char *string = 0;

    /* Send the request */
    m.type = SWAP_JOBS;
    m.u.swap.jobid1 = command_line.jobid;
    m.u.swap.jobid2 = command_line.jobid2;
    send_msg(server_socket, &m);

    /* Receive the answer */
    res = recv_msg(server_socket, &m);
    if (res != sizeof(m))
        error("Error in swap_jobs");
    switch (m.type) {
        case SWAP_JOBS_OK:
            return;
            /* WILL NOT GO FURTHER */
        case LIST_LINE: /* Only ONE line accepted */
            string = (char *) malloc(m.u.size);
            res = recv_bytes(server_socket, string, m.u.size);
            if (res != m.u.size)
                error("Error in swap_jobs - line size");
            fprintf(stderr, "Error in the request: %s",
                    string);
            free(string);
            exit(-1);
            /* WILL NOT GO FURTHER */
        default:
            warning("Wrong internal message in swap_jobs");
    }
    /* This will never be reached */
    return;
}

void c_get_count_running() {
    struct Msg m = default_msg();
    int res;

    /* Send the request */
    m.type = COUNT_RUNNING;
    send_msg(server_socket, &m);

    /* Receive the answer */
    res = recv_msg(server_socket, &m);
    if (res != sizeof(m))
        error("Error in count_running - line size");

    switch (m.type) {
        case COUNT_RUNNING:
            printf("%i\n", m.u.count_running);
            return;
        default:
            warning("Wrong internal message in count_running");
    }

    /* This will never be reached */
    return;
}

void c_show_label() {
    struct Msg m = default_msg();
    int res;
    char *string = 0;

    /* Send the request */
    m.type = GET_LABEL;
    m.u.jobid = command_line.jobid;
    send_msg(server_socket, &m);

    /* Receive the answer */
    res = recv_msg(server_socket, &m);
    if (res != sizeof(m))
        error("Error in get_label");

    switch (m.type) {
        case LIST_LINE:
            string = (char *) malloc(m.u.size);
            res = recv_bytes(server_socket, string, m.u.size);
            if (res != m.u.size)
                error("Error in get_label - line size");

            printf("%s", string);
            free(string);
            return;
        default:
            warning("Wrong internal message in get_label");
    }

    /* This will never be reached */
    return;
}

void c_show_cmd() {
    struct Msg m = default_msg();
    int res;
    char *string = 0;

    /* Send the request */
    m.type = GET_CMD;
    m.u.jobid = command_line.jobid;
    send_msg(server_socket, &m);

    /* Receive the answer */
    res = recv_msg(server_socket, &m);
    if (res != sizeof(m))
        error("Error in show_cmd");

    switch (m.type) {
        case LIST_LINE:
            string = (char *) malloc(m.u.size);
            res = recv_bytes(server_socket, string, m.u.size);
            if (res != m.u.size)
                error("Error in show_cmd - line size");

            printf("%s\n", string);
            free(string);
            return;
        default:
            warning("Wrong internal message in show_cmd");
    }
}

void c_get_env() {
    struct Msg m = default_msg();
    int res;
    char *string = 0;

    /* Send the request */
    m.type = GET_ENV;
    m.u.size = strlen(command_line.label) + 1;
    send_msg(server_socket, &m);
    send_bytes(server_socket, command_line.label, m.u.size);

    /* Receive the answer */
    res = recv_msg(server_socket, &m);
    if (res != sizeof(m))
        error("Error in get_env");

    switch (m.type) {
        case LIST_LINE:
            if (m.u.size) {
                string = (char *) malloc(m.u.size);
                res = recv_bytes(server_socket, string, m.u.size);
                if (res != m.u.size)
                    error("Error in get_env - line size");

                printf("%s\n", string);
                free(string);
            } else
                printf("\n");

            return;
        default:
            warning("Wrong internal message in get_env");
    }
}

void c_set_env() {
    struct Msg m = default_msg();

    /* Send the request */
    m.type = SET_ENV;
    m.u.size = strlen(command_line.label) + 1;
    send_msg(server_socket, &m);
    send_bytes(server_socket, command_line.label, m.u.size);
}

void c_unset_env() {
    struct Msg m = default_msg();

    /* Send the request */
    m.type = UNSET_ENV;
    m.u.size = strlen(command_line.label) + 1;
    send_msg(server_socket, &m);
    send_bytes(server_socket, command_line.label, m.u.size);
}

void c_set_free_percentage() {
    struct Msg m = default_msg();
    m.type = SET_FREE_PERC;
    m.u.size = command_line.gpus;
    send_msg(server_socket, &m);
}

void c_get_free_percentage() {
    struct Msg m = default_msg();
    int res;

    m.type = GET_FREE_PERC;
    send_msg(server_socket, &m);

    /* Receive the answer */
    res = recv_msg(server_socket, &m);
    if (res != sizeof(m))
        error("Error in get_free_percentage");

    switch (m.type) {
        case GET_FREE_PERC:
            printf("%d\n", m.u.size);
            return;
        default:
            warning("Wrong internal message in get_free_percentage");
    }
}

char* get_logdir() {
    struct Msg m = default_msg();
    int res;
    char *string = 0;

    /* Send the request */
    m.type = GET_LOGDIR;
    send_msg(server_socket, &m);

    /* Receive the answer */
    res = recv_msg(server_socket, &m);
    if (res != sizeof(m))
        error("Error in get_logdir");

    switch (m.type) {
        case LIST_LINE:
            string = (char *) malloc(m.u.size);
            res = recv_bytes(server_socket, string, m.u.size);
            if (res != m.u.size)
                error("Error in get_logdir - line size");

            return string;
        default:
            warning("Wrong internal message in get_logdir");
    }
    return string;
}

void c_get_logdir() {
    char* path;
    path = get_logdir();
    printf("%s\n", path);
    free(path);
}

void c_set_logdir() {
    struct Msg m = default_msg();

    /* Send the request */
    m.type = SET_LOGDIR;
    m.u.size = strlen(command_line.label) + 1;
    send_msg(server_socket, &m);
    send_bytes(server_socket, command_line.label, m.u.size);
}

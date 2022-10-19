/*
    Task Spooler - a task queue system for the unix user
    Copyright (C) 2007-2013  Lluís Batlle i Rossell

    Please find the license in the provided COPYING file.
*/
#define _DEFAULT_SOURCE
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <sys/socket.h>

#include "main.h"
#include "cjson/cJSON.h"

/* The list will access them */
int busy_slots = 0;
int max_slots = 1;

struct Notify {
    int socket;
    int jobid;
    struct Notify *next;
};

/* Globals */
static struct Job *firstjob = 0;
static struct Job *first_finished_job = 0;
static int jobids = 0;
/* This is used for dependencies from jobs
 * already out of the queue */
static int last_errorlevel = 0; /* Before the first job, let's consider
                                   a good previous result */
/* We need this to handle well "-d" after a "-nf" run */
static int last_finished_jobid;

static struct Notify *first_notify = 0;

/* server will access them */
int max_jobs;

static struct Job *get_job(int jobid);

void notify_errorlevel(struct Job *p);

static void shuffle(int *array, size_t n) {
    if (n > 1) {
        size_t i;
        srand(time(NULL));
        for (i = 0; i < n - 1; i++) {
            size_t j = i + rand() / (RAND_MAX / (n - i) + 1);
            int t = array[j];
            array[j] = array[i];
            array[i] = t;
        }
    }
}

static void destroy_job(struct Job* p) {
    free(p->notify_errorlevel_to);
    free(p->command);
    free(p->output_filename);
    pinfo_free(&p->info);
    free(p->depend_on);
    free(p->label);
    free(p->gpu_ids);
    free(p);
}

static void send_list_line(int s, const char *str) {
    struct Msg m = default_msg();

    /* Message */
    m.type = LIST_LINE;
    m.u.size = strlen(str) + 1;

    send_msg(s, &m);

    /* Send the line */
    send_bytes(s, str, m.u.size);
}

static void send_urgent_ok(int s) {
    struct Msg m = default_msg();

    /* Message */
    m.type = URGENT_OK;

    send_msg(s, &m);
}

static void send_swap_jobs_ok(int s) {
    struct Msg m = default_msg();

    /* Message */
    m.type = SWAP_JOBS_OK;

    send_msg(s, &m);
}

static struct Job *find_previous_job(const struct Job *final) {
    struct Job *p;

    /* Show Queued or Running jobs */
    p = firstjob;
    while (p != 0) {
        if (p->next == final)
            return p;
        p = p->next;
    }

    return 0;
}

static struct Job *findjob(int jobid) {
    struct Job *p;

    /* Show Queued or Running jobs */
    p = firstjob;
    while (p != 0) {
        if (p->jobid == jobid)
            return p;
        p = p->next;
    }

    return 0;
}

static struct Job *findjob_holding_client() {
    struct Job *p;

    /* Show Queued or Running jobs */
    p = firstjob;
    while (p != 0) {
        if (p->state == HOLDING_CLIENT)
            return p;
        p = p->next;
    }

    return 0;
}

static struct Job *find_finished_job(int jobid) {
    struct Job *p;

    /* Show Queued or Running jobs */
    p = first_finished_job;
    while (p != 0) {
        if (p->jobid == jobid)
            return p;
        p = p->next;
    }

    return 0;
}

static int count_not_finished_jobs() {
    int count = 0;
    struct Job *p;

    /* Show Queued or Running jobs */
    p = firstjob;
    while (p != 0) {
        ++count;
        p = p->next;
    }
    return count;
}

static void add_notify_errorlevel_to(struct Job *job, int jobid) {
    int *p;
    int newsize = (job->notify_errorlevel_to_size + 1)
                  * sizeof(int);
    p = (int *) realloc(job->notify_errorlevel_to,
                        newsize);

    if (p == 0)
        error("Cannot allocate more memory for notify_errorlist_to for jobid %i,"
              " having already %i elements",
              job->jobid, job->notify_errorlevel_to_size);

    job->notify_errorlevel_to = p;
    job->notify_errorlevel_to_size += 1;
    job->notify_errorlevel_to[job->notify_errorlevel_to_size - 1] = jobid;
}

void s_kill_all_jobs(int s) {
    struct Job *p;
    s_count_running_jobs(s);

    /* send running job PIDs */
    p = firstjob;
    while (p != 0) {
        if (p->state == RUNNING)
            send(s, &p->pid, sizeof(int), 0);

        p = p->next;
    }
}

void s_count_running_jobs(int s) {
    int count = 0;
    struct Job *p;
    struct Msg m = default_msg();

    /* Count running jobs */
    p = firstjob;
    while (p != 0) {
        if (p->state == RUNNING)
            ++count;

        p = p->next;
    }

    /* Message */
    m.type = COUNT_RUNNING;
    m.u.count_running = count;
    send_msg(s, &m);
}

int s_count_allocating_jobs() {
    int count = 0;
    struct Job *p;

    /* Count running jobs */
    p = firstjob;
    while (p != 0) {
        if (p->state == ALLOCATING)
            count++;

        p = p->next;
    }
    return count;
}

void s_send_label(int s, int jobid) {
    struct Job *p = 0;
    char *label;

    if (jobid == -1) {
        /* Find the last job added */
        p = firstjob;

        if (p != 0)
            while (p->next != 0)
                p = p->next;

        /* Look in finished jobs if needed */
        if (p == 0) {
            p = first_finished_job;
            if (p != 0)
                while (p->next != 0)
                    p = p->next;
        }

    } else {
        p = get_job(jobid);
    }

    if (p == 0) {
        char tmp[50];
        sprintf(tmp, "Job %i not finished or not running.\n", jobid);
        send_list_line(s, tmp);
        return;
    }

    if (p->label) {
        label = (char *) malloc(strlen(p->label) + 1);
        sprintf(label, "%s\n", p->label);
    } else
        label = "";
    send_list_line(s, label);
    if (p->label)
        free(label);
}

void s_send_cmd(int s, int jobid) {
    struct Job *p = 0;
    char *cmd;

    if (jobid == -1) {
        /* Find the last job added */
        p = firstjob;

        if (p != 0)
            while (p->next != 0)
                p = p->next;

        /* Look in finished jobs if needed */
        if (p == 0) {
            p = first_finished_job;
            if (p != 0)
                while (p->next != 0)
                    p = p->next;
        }

    } else {
        p = get_job(jobid);
    }

    if (p == 0) {
        char tmp[50];
        sprintf(tmp, "Job %i not finished or not running.\n", jobid);
        send_list_line(s, tmp);
        return;
    }
    cmd = (char *) malloc(strlen(p->command) + 1);
    sprintf(cmd, "%s\n", p->command);
    send_list_line(s, cmd);
    free(cmd);
}

void s_mark_job_running(int jobid) {
    struct Job *p;
    p = findjob(jobid);
    if (!p)
        error("Cannot mark the jobid %i RUNNING.", jobid);
    p->state = RUNNING;
}

/* -1 means nothing awaken, otherwise returns the jobid awaken */
int wake_hold_client() {
    struct Job *p;
    p = findjob_holding_client();
    if (p) {
        p->state = (p->num_gpus) ? ALLOCATING : QUEUED;
        return p->jobid;
    }
    return -1;
}

const char *jstate2string(enum Jobstate s) {
    const char *jobstate;
    switch (s) {
        case QUEUED:
            jobstate = "queued";
            break;
        case ALLOCATING:
            jobstate = "allocating";
            break;
        case RUNNING:
            jobstate = "running";
            break;
        case FINISHED:
            jobstate = "finished";
            break;
        case SKIPPED:
            jobstate = "skipped";
            break;
        case HOLDING_CLIENT:
            jobstate = "skipped";
            break;
    }
    return jobstate;
}

/* Serialize a job and add it to the JSON array. Returns 1 for success, 0 for failure. */
static int add_job_to_json_array(struct Job *p, cJSON *jobs) {
    cJSON *job = cJSON_CreateObject();
    if (job == NULL)
    {
        error("Error initializing JSON object for job %i.", p->jobid);
        return 0;
    }
    cJSON_AddItemToArray(jobs, job);

    /* Add fields */
    cJSON *field;

    /* ID */
    field = cJSON_CreateNumber(p->jobid);
    if (field == NULL)
    {
        error("Error initializing JSON object for job %i field ID.", p->jobid);
        return 0;
    }
    cJSON_AddItemToObject(job, "ID", field);

    /* State */
    const char *state_string = jstate2string(p->state);
    field = cJSON_CreateStringReference(state_string);
    if (field == NULL)
    {
        error("Error initializing JSON object for job %i field State (value %d/%s).", p->jobid, p->state, state_string);
        return 0;
    }
    cJSON_AddItemToObject(job, "State", field);

    /* Output */
    field = cJSON_CreateStringReference(p->output_filename);
    if (field == NULL)
    {
        error("Error initializing JSON object for job %i field Output (value %s).", p->jobid, p->output_filename);
        return 0;
    }
    cJSON_AddItemToObject(job, "Output", field);

    /* E-Level */
    if (p->state == FINISHED) {
        field = cJSON_CreateNumber(p->result.errorlevel);
    }
    else {
        field = cJSON_CreateNull();
    }
    if (field == NULL)
    {
        error("Error initializing JSON object for job %i field E-Level.", p->jobid);
        return 0;
    }
    cJSON_AddItemToObject(job, "E-Level", field);

    /* Time */
    if (p->state == FINISHED) {
        field = cJSON_CreateNumber(p->result.real_ms);
        if (field == NULL)
        {
            error("Error initializing JSON object for job %i field Time_ms (value %d).", p->result.real_ms);
            return 0;
        }
    }
    else {
        field = cJSON_CreateNull();
        if (field == NULL)
        {
            error("Error initializing JSON object for job %i field Time_ms (no result).");
            return 0;
        }
    }
    cJSON_AddItemToObject(job, "Time_ms", field);

    /* GPUs */
    #ifndef CPU
    field = cJSON_CreateNumber(p->num_gpus);
    if (field == NULL)
    {
        error("Error initializing JSON object for job %i field GPUs (value %d).", p->num_gpus);
        return 0;
    }
    cJSON_AddItemToObject(job, "GPUs", field);
    #endif /*CPU*/

    /* Command */
    field = cJSON_CreateStringReference(p->command);
    if (field == NULL)
    {
        error("Error initializing JSON object for job %i field Command (value %s).", p->jobid, p->command);
        return 0;
    }
    cJSON_AddItemToObject(job, "Command", field);

    return 1;
}

void s_list(int s, enum ListFormat listFormat) {
    struct Job *p;
    char *buffer;

    if (listFormat == DEFAULT) {
        /* Times:   0.00/0.00/0.00 - 4+4+4+2 = 14*/
        buffer = joblist_headers();
        send_list_line(s, buffer);
        free(buffer);

        /* Show Queued or Running jobs */
        p = firstjob;
        while (p != 0) {
            if (p->state != HOLDING_CLIENT) {
                buffer = joblist_line(p);
                send_list_line(s, buffer);
                free(buffer);
            }
            p = p->next;
        }

        p = first_finished_job;

        /* Show Finished jobs */
        while (p != 0) {
            buffer = joblist_line(p);
            send_list_line(s, buffer);
            free(buffer);
            p = p->next;
        }
    }
    else if (listFormat == JSON) {
        cJSON *jobs = cJSON_CreateArray();
        if (jobs == NULL)
        {
            error("Error initializing JSON array.");
            goto end;
        }

        /* Serialize Queued or Running jobs */
        p = firstjob;
        while (p != 0) {
            if (p->state != HOLDING_CLIENT) {
                int success = add_job_to_json_array(p, jobs);
                if (success == 0) {
                    goto end;
                }
            }
            p = p->next;
        }

        /* Serialize Finished jobs */
        p = first_finished_job;
        while (p != 0) {
            int success = add_job_to_json_array(p, jobs);
            if (success == 0) {
                goto end;
            }
            p = p->next;
        }

        buffer = cJSON_PrintUnformatted(jobs);
        if (buffer == NULL)
        {
            error("Error converting jobs to JSON.");
            goto end;
        }
        
        // append newline
        size_t buffer_strlen = strlen(buffer);
        buffer = realloc(buffer, buffer_strlen+1+1);
        buffer[buffer_strlen] = '\n';

        send_list_line(s, buffer);
    end:
        cJSON_Delete(jobs);
        free(buffer);
    }
    else if (listFormat == TAB) {
        /* Show Queued or Running jobs */
        p = firstjob;
        while (p != 0) {
            if (p->state != HOLDING_CLIENT) {
                buffer = joblist_line_plain(p);
                send_list_line(s, buffer);
                free(buffer);
            }
            p = p->next;
        }

        p = first_finished_job;

        /* Show Finished jobs */
        while (p != 0) {
            buffer = joblist_line_plain(p);
            send_list_line(s, buffer);
            free(buffer);
            p = p->next;
        }
    }
}

#ifndef CPU
void s_list_gpu(int s) {
    struct Job *p = firstjob;
    char* buffer;

    buffer = jobgpulist_header();
    send_list_line(s, buffer);
    while (p != 0) {
        if (p->state == RUNNING && p->num_gpus) {
            buffer = jobgpulist_line(p);
            send_list_line(s, buffer);
            free(buffer);
        }
        p = p->next;
    }
}
#endif

static void init_job(struct Job *p) {
    p->next = 0;
    p->output_filename = 0;
    p->command = 0;
    p->depend_on = 0;
    p->depend_on_size = 0;
    p->gpu_ids = 0;
    p->label = 0;
    p->notify_errorlevel_to_size = 0;
    p->notify_errorlevel_to = 0;
    p->dependency_errorlevel = 0;
    pinfo_init(&p->info);
}

static struct Job *newjobptr() {
    struct Job *p;

    if (firstjob == 0) {
        firstjob = (struct Job *) malloc(sizeof(*firstjob));
        init_job(firstjob);
        return firstjob;
    }

    p = firstjob;
    while (p->next != 0)
        p = p->next;

    p->next = (struct Job *) malloc(sizeof(*p));
    init_job(p->next);
    return p->next;
}

/* Returns -1 if no last job id found */
static int find_last_jobid_in_queue(int neglect_jobid) {
    struct Job *p;
    int last_jobid = -1;

    p = firstjob;
    while (p != 0) {
        if (p->jobid != neglect_jobid &&
            p->jobid > last_jobid)
            last_jobid = p->jobid;
        p = p->next;
    }

    return last_jobid;
}

/* Returns -1 if no last job id found */
static int find_last_stored_jobid_finished() {
    struct Job *p;
    int last_jobid = -1;

    p = first_finished_job;
    while (p != 0) {
        if (p->jobid > last_jobid)
            last_jobid = p->jobid;
        p = p->next;
    }

    return last_jobid;
}

/* Returns job id or -1 on error */
int s_newjob(int s, struct Msg *m) {
    struct Job *p;
    int res;

    p = newjobptr();

    p->jobid = jobids++;

    /* GPUs */
    p->num_gpus = m->u.newjob.gpus;
    if (count_not_finished_jobs() < max_jobs)
        p->state = (p->num_gpus) ? ALLOCATING : QUEUED;
    else
        p->state = HOLDING_CLIENT;

    p->wait_free_gpus = m->u.newjob.wait_free_gpus;
    if (!p->wait_free_gpus)
        p->gpu_ids = recv_ints(s, &p->num_gpus);
    else {
        p->gpu_ids = (int *) malloc((p->num_gpus + 1) * sizeof(int));
        memset(p->gpu_ids, -1, (p->num_gpus + 1) * sizeof(int));
    }

    p->num_slots = m->u.newjob.num_slots;
    p->store_output = m->u.newjob.store_output;
    p->should_keep_finished = m->u.newjob.should_keep_finished;

    /* this error level here is used internally to decide whether a job should be run or not
     * so it only matters whether the error level is 0 or not.
     * thus, summing the absolute error levels of all dependencies is sufficient.*/
    if (m->u.newjob.depend_on_size) {
        int *depend_on;
        depend_on = recv_ints(s, &p->depend_on_size);

        /* Depend on the last queued job. */
        int idx = 0;
        for (int i = 0; i < p->depend_on_size; i++) {
            /* filter out dependencies that are current jobs */
            if (depend_on[i] >= p->jobid)
                continue;

            p->depend_on = (int*) realloc(p->depend_on, (idx + 1) * sizeof(int));
            /* As we already have 'p' in the queue,
             * neglect it during the find_last_jobid_in_queue() */
            if (depend_on[i] == -1) {
                p->depend_on[idx] = find_last_jobid_in_queue(p->jobid);

                /* We don't trust the last jobid in the queue (running or queued)
                 * if it's not the last added job. In that case, let
                 * the next control flow handle it as if it could not
                 * do_depend on any still queued job. */
                if (last_finished_jobid > p->depend_on[idx])
                    p->depend_on[idx] = -1;

                /* If it's queued still without result, let it know
                 * its result to p when it finishes. */
                if (p->depend_on[idx] != -1) {
                    struct Job *depended_job;
                    depended_job = findjob(p->depend_on[idx]);
                    if (depended_job != 0)
                        add_notify_errorlevel_to(depended_job, p->jobid);
                    else
                        warning("The jobid %i is queued to do_depend on the jobid %i"
                                " suddenly non existent in the queue", p->jobid,
                                p->depend_on[idx]);
                } else /* Otherwise take the finished job, or the last_errorlevel */
                {
                    if (depend_on[i] == -1) {
                        int ljobid = find_last_stored_jobid_finished();
                        p->depend_on[idx] = ljobid;

                        /* If we have a newer result stored, use it */
                        /* NOTE:
                         *   Reading this now, I don't know how ljobid can be
                         *   greater than last_finished_jobid */
                        if (last_finished_jobid < ljobid) {
                            struct Job *parent;
                            parent = find_finished_job(ljobid);
                            if (!parent)
                                error("jobid %i suddenly disappeared from the finished list",
                                      ljobid);
                            p->dependency_errorlevel += abs(parent->result.errorlevel);
                        } else
                            p->dependency_errorlevel += abs(last_errorlevel);
                    }
                }
            } else {
                /* The user decided what's the job this new job depends on */
                struct Job *depended_job;
                p->depend_on[idx] = depend_on[i];
                depended_job = findjob(p->depend_on[idx]);

                if (depended_job != 0)
                    add_notify_errorlevel_to(depended_job, p->jobid);
                else {
                    struct Job *parent;
                    parent = find_finished_job(p->depend_on[idx]);
                    if (parent) {
                        p->dependency_errorlevel += abs(parent->result.errorlevel);
                    } else {
                        /* We consider as if the job not found
                           didn't finish well */
                        p->dependency_errorlevel += 1;
                    }
                }
            }
            idx++;
        }
        free(depend_on);
        p->depend_on_size = idx;
    }

    /* if dependency list is empty after removing invalid dependencies, make it independent */
    if (p->depend_on_size == 0)
        p->depend_on = 0;

    pinfo_set_enqueue_time(&p->info);

    /* load the command */
    p->command = malloc(m->u.newjob.command_size);
    if (p->command == 0)
        error("Cannot allocate memory in s_newjob command_size (%i)",
              m->u.newjob.command_size);
    res = recv_bytes(s, p->command, m->u.newjob.command_size);
    if (res == -1)
        error("wrong bytes received");

    /* load the label */
    if (m->u.newjob.label_size > 0) {
        char *ptr;
        ptr = (char *) malloc(m->u.newjob.label_size);
        if (ptr == 0)
            error("Cannot allocate memory in s_newjob label_size(%i)",
                  m->u.newjob.label_size);
        res = recv_bytes(s, ptr, m->u.newjob.label_size);
        if (res == -1)
            error("wrong bytes received");
        p->label = ptr;
    }

    /* load the info */
    if (m->u.newjob.env_size > 0) {
        char *ptr;
        ptr = (char *) malloc(m->u.newjob.env_size);
        if (ptr == 0)
            error("Cannot allocate memory in s_newjob env_size(%i)",
                  m->u.newjob.env_size);
        res = recv_bytes(s, ptr, m->u.newjob.env_size);
        if (res == -1)
            error("wrong bytes received");
        pinfo_addinfo(&p->info, m->u.newjob.env_size + 100,
                      "Environment:\n%s", ptr);
        free(ptr);
    }
    return p->jobid;
}

/* This assumes the jobid exists */
void s_removejob(int jobid) {
    struct Job *p;
    struct Job *newnext;

    if (firstjob->jobid == jobid) {
        struct Job *newfirst;

        /* First job is to be removed */
        newfirst = firstjob->next;
        destroy_job(firstjob);
        firstjob = newfirst;
        return;
    }

    p = firstjob;
    /* Not first job */
    while (p->next != 0) {
        if (p->next->jobid == jobid)
            break;
        p = p->next;
    }
    if (p->next == 0)
        error("Job to be removed not found. jobid=%i", jobid);

    newnext = p->next->next;

    destroy_job(p->next);
    p->next = newnext;
}

/* -1 if no one should be run. */
int next_run_job() {
    struct Job *p;

    const int free_slots = max_slots - busy_slots;

    /* busy_slots may be bigger than the maximum slots,
     * if the user was running many jobs, and suddenly
     * trimmed the maximum slots down. */
    if (free_slots <= 0)
        return -1;

    /* If there are no jobs to run... */
    if (firstjob == 0)
        return -1;

#ifndef CPU
    /* Query GPUs */
    int numFree;
    int *freeGpuList = getGpuList(&numFree);
#endif

    /* Look for a runnable task */
    p = firstjob;
    while (p != 0) {
        if (p->state == QUEUED || p->state == ALLOCATING) {
#ifndef CPU
            if (p->num_gpus && p->wait_free_gpus) {
                if (numFree < p->num_gpus) {
                    /* if fewer GPUs than required then next */
                    p = p->next;
                    continue;
                }
                shuffle(freeGpuList, numFree);

                /* We have to check whether some GPUs are used by other jobs, but their RAMs are still free
                 * These GPUs should not be used.*/
                int i = 0, j = 0;
                /* loop until all GPUs required can be found, or there are enough GPUs */
                int *gpu_ids = (int*) malloc(p->num_gpus * sizeof(int));
                while (i < p->num_gpus && j < numFree) {
                    /* if the prospective GPUs are in used, select the next one */
                    if (!isInUse(freeGpuList[j]))
                        gpu_ids[i++] = freeGpuList[j];
                    j++;
                }
                /* some GPUs might already be claimed by other jobs, but the system still reports as free -> skip */
                if (i < p->num_gpus) {
                    p = p->next;
                    free(gpu_ids);
                    continue;
                }
                memcpy(p->gpu_ids, gpu_ids, p->num_gpus * sizeof(int));
                free(gpu_ids);
            }
#endif

            if (p->depend_on_size) {
                int ready = 1;
                for (int i = 0; i < p->depend_on_size; i++) {
                    struct Job *do_depend_job = get_job(p->depend_on[i]);
                    /* We won't try to run any job do_depending on an unfinished
                     * job */
                    if (do_depend_job != NULL &&
                        (do_depend_job->state == QUEUED || do_depend_job->state == RUNNING ||
                        do_depend_job->state == ALLOCATING)) {
                        /* Next try */
                        p = p->next;
                        ready = 0;
                        break;
                    }
                }
                if (ready != 1)
                    continue;
            }

            if (free_slots >= p->num_slots) {
                busy_slots = busy_slots + p->num_slots;
#ifndef CPU
                if (p->num_gpus)
                    broadcastUsedGpus(p->num_gpus, p->gpu_ids);

                free(freeGpuList);
#endif
                return p->jobid;
            }
        }
        p = p->next;
    }
#ifndef CPU
    free(freeGpuList);
#endif
    return -1;
}

/* Returns 1000 if no limit, The limit otherwise. */
static int get_max_finished_jobs() {
    char *limit;

    limit = getenv("TS_MAXFINISHED");
    if (limit == NULL)
        return 1000;
    return abs(atoi(limit));
}

/* Add the job to the finished queue. */
static void new_finished_job(struct Job *j) {
    struct Job *p;
    int count, max;

    max = get_max_finished_jobs();
    count = 0;

    if (first_finished_job == 0 && count < max) {
        first_finished_job = j;
        first_finished_job->next = 0;
        return;
    }

    ++count;

    p = first_finished_job;
    while (p->next != 0) {
        p = p->next;
        ++count;
    }

    /* If too many jobs, wipe out the first */
    if (count >= max) {
        struct Job *tmp;
        tmp = first_finished_job;
        first_finished_job = first_finished_job->next;
        destroy_job(tmp);
    }
    p->next = j;
    p->next->next = 0;
}

static int job_is_in_state(int jobid, enum Jobstate state) {
    struct Job *p;

    p = findjob(jobid);
    if (p == 0)
        return 0;
    if (p->state == state)
        return 1;
    return 0;
}

int job_is_running(int jobid) {
    return job_is_in_state(jobid, RUNNING);
}

int job_is_holding_client(int jobid) {
    return job_is_in_state(jobid, HOLDING_CLIENT);
}

static int in_notify_list(int jobid) {
    struct Notify *n, *tmp;

    n = first_notify;
    while (n != 0) {
        tmp = n;
        n = n->next;
        if (tmp->jobid == jobid)
            return 1;
    }
    return 0;
}

void job_finished(const struct Result *result, int jobid) {
    struct Job *p;

    if (busy_slots <= 0)
        error("Wrong state in the server. busy_slots = %i instead of greater than 0", busy_slots);

    p = findjob(jobid);
    if (p == 0)
        error("on jobid %i finished, it doesn't exist", jobid);

#ifndef CPU
    /* Recycle GPUs */
    broadcastFreeGpus(p->num_gpus, p->gpu_ids);
#endif

    /* The job may be not only in running state, but also in other states, as
     * we call this to clean up the jobs list in case of the client closing the
     * connection. */
    if (p->state == RUNNING)
        busy_slots = busy_slots - p->num_slots;

    /* Mark state */
    if (result->skipped)
        p->state = SKIPPED;
    else
        p->state = FINISHED;
    p->result = *result;
    last_finished_jobid = p->jobid;
    notify_errorlevel(p);
    pinfo_set_end_time(&p->info);

    if (p->result.died_by_signal)
        pinfo_addinfo(&p->info, 100, "Exit status: killed by signal %i\n", p->result.signal);
    else
        pinfo_addinfo(&p->info, 100, "Exit status: died with exit code %i\n", p->result.errorlevel);

    /* Find the pointing node, to
     * update it removing the finished job. */
    {
        struct Job **jpointer = 0;
        struct Job *newfirst = p->next;
        if (firstjob == p)
            jpointer = &firstjob;
        else {
            struct Job *p2;
            p2 = firstjob;
            while (p2 != 0) {
                if (p2->next == p) {
                    jpointer = &(p2->next);
                    break;
                }
                p2 = p2->next;
            }
        }

        /* Add it to the finished queue (maybe temporarily) */
        if (p->should_keep_finished || in_notify_list(p->jobid))
            new_finished_job(p);

        /* Remove it from the run queue */
        if (jpointer == 0)
            error("Cannot remove a finished job from the "
                  "queue list (jobid=%i)", p->jobid);

        *jpointer = newfirst;
    }
}

void s_clear_finished() {
    struct Job *p;

    if (first_finished_job == 0)
        return;

    p = first_finished_job;
    first_finished_job = 0;

    while (p != 0) {
        struct Job *tmp;
        tmp = p->next;
        destroy_job(p);
        p = tmp;
    }
}

void s_process_runjob_ok(int jobid, char *oname, int pid) {
    struct Job *p;
    p = findjob(jobid);
    if (p == 0)
        error("Job %i already run not found on runjob_ok", jobid);
    if (p->state != RUNNING)
        error("Job %i not running, but %i on runjob_ok", jobid,
              p->state);

    p->pid = pid;
    p->output_filename = oname;
    pinfo_set_start_time(&p->info);
}

void s_send_runjob(int s, int jobid) {
    struct Msg m = default_msg();
    struct Job *p;

    p = findjob(jobid);
    if (p == 0)
        error("Job %i was expected to run", jobid);

    m.type = RUNJOB;

    /* TODO
     * We should make the dependencies update the jobids they're do_depending on.
     * Then, on finish, these could set the errorlevel to send to its dependency childs.
     * We cannot consider that the jobs will leave traces in the finished job list (-nf?) . */

    m.u.last_errorlevel = p->dependency_errorlevel;
    send_msg(s, &m);

    /* send GPU IDs */
    send_ints(s, p->gpu_ids, p->num_gpus);
}

void s_job_info(int s, int jobid) {
    struct Job *p = 0;
    struct Msg m = default_msg();

    if (jobid == -1) {
        /* This means that we want the job info of the running task, or that
         * of the last job run */
        if (busy_slots > 0) {
            p = firstjob;
            if (p == 0)
                error("Internal state WAITING, but job not run."
                      "firstjob = %x", firstjob);
        } else {
            p = first_finished_job;
            if (p == 0) {
                send_list_line(s, "No jobs.\n");
                return;
            }
            while (p->next != 0)
                p = p->next;
        }
    } else {
        p = firstjob;
        while (p != 0 && p->jobid != jobid)
            p = p->next;

        /* Look in finished jobs if needed */
        if (p == 0) {
            p = first_finished_job;
            while (p != 0 && p->jobid != jobid)
                p = p->next;
        }
    }

    if (p == 0) {
        char tmp[50];
        sprintf(tmp, "Job %i not finished or not running.\n", jobid);
        send_list_line(s, tmp);
        return;
    }

    m.type = INFO_DATA;
    send_msg(s, &m);
    pinfo_dump(&p->info, s);
    fd_nprintf(s, 100, "Command: ");
    if (p->depend_on) {
        fd_nprintf(s, 100, "[%i,", p->depend_on[0]);
        for (int i = 1; i < p->depend_on_size; i++)
            fd_nprintf(s, 100, ",%i", p->depend_on[i]);
        fd_nprintf(s, 100, "]&& ");
    }
    write(s, p->command, strlen(p->command));
    fd_nprintf(s, 100, "\n");
    fd_nprintf(s, 100, "Slots required: %i\n", p->num_slots);
#ifndef CPU
    fd_nprintf(s, 100, "GPUs required: %d\n", p->num_gpus);
    fd_nprintf(s, 100, "GPU IDs: %s\n", ints_to_chars(
            p->gpu_ids, p->num_gpus ? p->num_gpus : 1, ","));
#endif
    fd_nprintf(s, 100, "Enqueue time: %s",
               ctime(&p->info.enqueue_time.tv_sec));
    if (p->state == RUNNING) {
        fd_nprintf(s, 100, "Start time: %s",
                   ctime(&p->info.start_time.tv_sec));
        float t = pinfo_time_until_now(&p->info);
        char *unit = time_rep(&t);
        fd_nprintf(s, 100, "Time running: %f%s\n", t, unit);
    } else if (p->state == FINISHED) {
        fd_nprintf(s, 100, "Start time: %s",
                   ctime(&p->info.start_time.tv_sec));
        fd_nprintf(s, 100, "End time: %s",
                   ctime(&p->info.end_time.tv_sec));
        float t = pinfo_time_run(&p->info);
        char *unit = time_rep(&t);
        fd_nprintf(s, 100, "Time run: %f%s\n", t, unit);
    }
}

void s_send_last_id(int s) {
    struct Msg m = default_msg();

    m.type = LAST_ID;
    m.u.jobid = jobids - 1;
    send_msg(s, &m);
}

void s_send_output(int s, int jobid) {
    struct Job *p = 0;
    struct Msg m = default_msg();

    if (jobid == -1) {
        /* This means that we want the output info of the running task, or that
         * of the last job run */
        if (busy_slots > 0) {
            p = firstjob;
            if (p == 0)
                error("Internal state WAITING, but job not run."
                      "firstjob = %x", firstjob);
        } else {
            p = first_finished_job;
            if (p == 0) {
                send_list_line(s, "No jobs.\n");
                return;
            }
            while (p->next != 0)
                p = p->next;
        }
    } else {
        p = get_job(jobid);
        if (p != 0 && p->state != RUNNING
            && p->state != FINISHED
            && p->state != SKIPPED)
            p = 0;
    }

    if (p == 0) {
        char tmp[50];
        if (jobid == -1)
            sprintf(tmp, "The last job has not finished or is not running.\n");
        else
            sprintf(tmp, "Job %i not finished or not running.\n", jobid);
        send_list_line(s, tmp);
        return;
    }

    if (p->state == SKIPPED) {
        char tmp[50];
        if (jobid == -1)
            sprintf(tmp, "The last job was skipped due to a dependency.\n");

        else
            sprintf(tmp, "Job %i was skipped due to a dependency.\n", jobid);
        send_list_line(s, tmp);
        return;
    }

    m.type = ANSWER_OUTPUT;
    m.u.output.store_output = p->store_output;
    m.u.output.pid = p->pid;
    if (m.u.output.store_output && p->output_filename)
        m.u.output.ofilename_size = strlen(p->output_filename) + 1;
    else
        m.u.output.ofilename_size = 0;
    send_msg(s, &m);
    if (m.u.output.ofilename_size > 0)
        send_bytes(s, p->output_filename, m.u.output.ofilename_size);
}

void notify_errorlevel(struct Job *p) {
    int i;

    last_errorlevel = p->result.errorlevel;

    for (i = 0; i < p->notify_errorlevel_to_size; ++i) {
        struct Job *notified;
        notified = get_job(p->notify_errorlevel_to[i]);
        if (notified) {
            notified->dependency_errorlevel += abs(p->result.errorlevel);
        }
    }
}

/* jobid is input/output. If the input is -1, it's changed to the jobid
 * removed */
int s_remove_job(int s, int *jobid) {
    struct Job *p = 0;
    struct Msg m = default_msg();
    struct Job *before_p = 0;

    if (*jobid == -1) {
        /* Find the last job added */
        p = firstjob;
        if (p != 0) {
            while (p->next != 0) {
                before_p = p;
                p = p->next;
            }
        } else {
            /* last 'finished' */
            p = first_finished_job;
            if (p) {
                while (p->next != 0) {
                    before_p = p;
                    p = p->next;
                }
            }
        }
    } else {
        p = firstjob;
        if (p != 0) {
            while (p->next != 0 && p->jobid != *jobid) {
                before_p = p;
                p = p->next;
            }
        }

        /* If not found, look in the 'finished' list */
        if (p == 0 || p->jobid != *jobid) {
            p = first_finished_job;
            if (p != 0) {
                while (p->next != 0 && p->jobid != *jobid) {
                    before_p = p;
                    p = p->next;
                }
                if (p->jobid != *jobid)
                    p = 0;
            }
        }
    }

    if (p == 0 || p->state == RUNNING || p == firstjob) {
        char tmp[50];
        if (*jobid == -1)
            sprintf(tmp, "The last job cannot be removed.\n");
        else
            sprintf(tmp, "The job %i cannot be removed.\n", *jobid);
        send_list_line(s, tmp);
        return 0;
    }

    /* Return the jobid found */
    *jobid = p->jobid;

    /* Tricks for the check_notify_list */
    p->state = FINISHED;
    p->result.errorlevel = -1;
    notify_errorlevel(p);

    /* Notify the clients in wait_job */
    check_notify_list(m.u.jobid);

    /* Update the list pointers */
    if (p == first_finished_job)
        first_finished_job = p->next;
    else
        before_p->next = p->next;

    destroy_job(p);

    m.type = REMOVEJOB_OK;
    send_msg(s, &m);
    return 1;
}

static void add_to_notify_list(int s, int jobid) {
    struct Notify *n;
    struct Notify *new;

    new = (struct Notify *) malloc(sizeof(*new));

    new->socket = s;
    new->jobid = jobid;
    new->next = 0;

    n = first_notify;
    if (n == 0) {
        first_notify = new;
        return;
    }

    while (n->next != 0)
        n = n->next;

    n->next = new;
}

static void send_waitjob_ok(int s, int errorlevel) {
    struct Msg m = default_msg();

    m.type = WAITJOB_OK;
    m.u.result.errorlevel = errorlevel;
    send_msg(s, &m);
}

static struct Job *
get_job(int jobid) {
    struct Job *j;

    j = findjob(jobid);
    if (j != NULL)
        return j;

    j = find_finished_job(jobid);

    if (j != NULL)
        return j;

    return 0;
}

/* Don't complain, if the socket doesn't exist */
void s_remove_notification(int s) {
    struct Notify *n;
    struct Notify *previous;
    n = first_notify;
    while (n != 0 && n->socket != s)
        n = n->next;
    if (n == 0 || n->socket != s)
        return;

    /* Remove the notification */
    previous = first_notify;
    if (n == previous) {
        first_notify = n->next;
        free(n);
        return;
    }

    /* if not the first... */
    while (previous->next != n)
        previous = previous->next;

    previous->next = n->next;
    free(n);
}

static void destroy_finished_job(struct Job *j) {
    if (j == first_finished_job)
        first_finished_job = j->next;
    else {
        struct Job *i;
        for (i = first_finished_job; i != 0; ++i) {
            if (i->next == j) {
                i->next = j->next;
                break;
            }
        }
        if (i == 0) {
            error("Cannot destroy the expected job %i", j->jobid);
        }
    }

    destroy_job(j);
}

/* This is called when a job finishes */
void check_notify_list(int jobid) {
    struct Notify *n, *tmp;
    struct Job *j;

    n = first_notify;
    while (n != 0) {
        tmp = n;
        n = n->next;
        if (tmp->jobid == jobid) {
            j = get_job(jobid);
            /* If the job finishes, notify the waiter */
            if (j->state == FINISHED || j->state == SKIPPED) {
                send_waitjob_ok(tmp->socket, j->result.errorlevel);
                /* We want to get the next Nofity* before we remove
                 * the actual 'n'. As s_remove_notification() simply
                 * removes the element from the linked list, we can
                 * safely follow on the list from n->next. */
                s_remove_notification(tmp->socket);

                /* Remove the jobs that were temporarily in the finished list,
                 * just for their notifiers. */
                if (!in_notify_list(jobid) && !j->should_keep_finished) {
                    destroy_finished_job(j);
                }
            }
        }
    }
}

void s_wait_job(int s, int jobid) {
    struct Job *p = 0;

    if (jobid == -1) {
        /* Find the last job added */
        p = firstjob;

        if (p != 0)
            while (p->next != 0)
                p = p->next;

        /* Look in finished jobs if needed */
        if (p == 0) {
            p = first_finished_job;
            if (p != 0)
                while (p->next != 0)
                    p = p->next;
        }
    } else {
        p = firstjob;
        while (p != 0 && p->jobid != jobid)
            p = p->next;

        /* Look in finished jobs if needed */
        if (p == 0) {
            p = first_finished_job;
            while (p != 0 && p->jobid != jobid)
                p = p->next;
        }
    }

    if (p == 0) {
        char tmp[50];
        if (jobid == -1)
            sprintf(tmp, "The last job cannot be waited.\n");
        else
            sprintf(tmp, "The job %i cannot be waited.\n", jobid);
        send_list_line(s, tmp);
        return;
    }

    if (p->state == FINISHED || p->state == SKIPPED) {
        send_waitjob_ok(s, p->result.errorlevel);
    } else
        add_to_notify_list(s, p->jobid);
}

void s_wait_running_job(int s, int jobid) {
    struct Job *p = 0;

    /* The job finding algorithm should be similar to that of
     * s_send_output, because this will be used by "-t" and "-c" */
    if (jobid == -1) {
        /* This means that we want the output info of the running task, or that
         * of the last job run */
        if (busy_slots > 0) {
            p = firstjob;
            if (p == 0)
                error("Internal state WAITING, but job not run."
                      "firstjob = %x", firstjob);
        } else {
            p = first_finished_job;
            if (p == 0) {
                send_list_line(s, "No jobs.\n");
                return;
            }
            while (p->next != 0)
                p = p->next;
        }
    } else {
        p = firstjob;
        while (p != 0 && p->jobid != jobid)
            p = p->next;

        /* Look in finished jobs if needed */
        if (p == 0) {
            p = first_finished_job;
            while (p != 0 && p->jobid != jobid)
                p = p->next;
        }
    }

    if (p == 0) {
        char tmp[50];
        if (jobid == -1)
            sprintf(tmp, "The last job cannot be waited.\n");
        else
            sprintf(tmp, "The job %i cannot be waited.\n", jobid);
        send_list_line(s, tmp);
        return;
    }

    if (p->state == FINISHED || p->state == SKIPPED) {
        send_waitjob_ok(s, p->result.errorlevel);
    } else
        add_to_notify_list(s, p->jobid);
}

void s_set_max_slots(int new_max_slots) {
    if (new_max_slots > 0)
        max_slots = new_max_slots;
    else
        warning("Received new_max_slots=%i", new_max_slots);
}

void s_get_max_slots(int s) {
    struct Msg m = default_msg();

    /* Message */
    m.type = GET_MAX_SLOTS_OK;
    m.u.max_slots = max_slots;

    send_msg(s, &m);
}

void s_move_urgent(int s, int jobid) {
    struct Job *p = 0;
    struct Job *tmp1;

    if (jobid == -1) {
        /* Find the last job added */
        p = firstjob;

        if (p != 0)
            while (p->next != 0)
                p = p->next;
    } else {
        p = firstjob;
        while (p != 0 && p->jobid != jobid)
            p = p->next;
    }

    if (p == 0 || firstjob->next == 0) {
        char tmp[50];
        if (jobid == -1)
            sprintf(tmp, "The last job cannot be urged.\n");
        else
            sprintf(tmp, "The job %i cannot be urged.\n", jobid);
        send_list_line(s, tmp);
        return;
    }

    /* Interchange the pointers */
    tmp1 = find_previous_job(p);
    tmp1->next = p->next;
    p->next = firstjob->next;
    firstjob->next = p;
    send_urgent_ok(s);
}

void s_swap_jobs(int s, int jobid1, int jobid2) {
    struct Job *p1, *p2;
    struct Job *prev1, *prev2;
    struct Job *tmp;

    p1 = findjob(jobid1);
    p2 = findjob(jobid2);

    if (p1 == 0 || p2 == 0 || p1 == firstjob || p2 == firstjob) {
        char prev[60];
        sprintf(prev, "The jobs %i and %i cannot be swapped.\n", jobid1, jobid2);
        send_list_line(s, prev);
        return;
    }

    /* Interchange the pointers */
    prev1 = find_previous_job(p1);
    prev2 = find_previous_job(p2);
    prev1->next = p2;
    prev2->next = p1;
    tmp = p1->next;
    p1->next = p2->next;
    p2->next = tmp;

    send_swap_jobs_ok(s);
}

static void send_state(int s, enum Jobstate state) {
    struct Msg m = default_msg();

    m.type = ANSWER_STATE;
    m.u.state = state;

    send_msg(s, &m);
}

void s_send_state(int s, int jobid) {
    struct Job *p = 0;

    if (jobid == -1) {
        /* Find the last job added */
        p = firstjob;

        if (p != 0)
            while (p->next != 0)
                p = p->next;

        /* Look in finished jobs if needed */
        if (p == 0) {
            p = first_finished_job;
            if (p != 0)
                while (p->next != 0)
                    p = p->next;
        }

    } else {
        p = get_job(jobid);
    }

    if (p == 0) {
        char tmp[50];
        if (jobid == -1)
            sprintf(tmp, "The last job cannot be stated.\n");
        else
            sprintf(tmp, "The job %i cannot be stated.\n", jobid);
        send_list_line(s, tmp);
        return;
    }

    /* Interchange the pointers */
    send_state(s, p->state);
}


static void dump_job_struct(FILE *out, const struct Job *p) {
    fprintf(out, "  new_job\n");
    fprintf(out, "    jobid %i\n", p->jobid);
    fprintf(out, "    command \"%s\"\n", p->command);
    fprintf(out, "    state %s\n",
            jstate2string(p->state));
    fprintf(out, "    result.errorlevel %i\n", p->result.errorlevel);
    fprintf(out, "    output_filename \"%s\"\n",
            p->output_filename ? p->output_filename : "NULL");
    fprintf(out, "    store_output %i\n", p->store_output);
    fprintf(out, "    pid %i\n", p->pid);
    fprintf(out, "    should_keep_finished %i\n", p->should_keep_finished);
}

void dump_jobs_struct(FILE *out) {
    const struct Job *p;

    fprintf(out, "New_jobs\n");

    p = firstjob;
    while (p != 0) {
        dump_job_struct(out, p);
        p = p->next;
    }

    p = first_finished_job;
    while (p != 0) {
        dump_job_struct(out, p);
        p = p->next;
    }
}

static void dump_notify_struct(FILE *out, const struct Notify *n) {
    fprintf(out, "  notify\n");
    fprintf(out, "    jobid %i\n", n->jobid);
    fprintf(out, "    socket \"%i\"\n", n->socket);
}

void dump_notifies_struct(FILE *out) {
    const struct Notify *n;

    fprintf(out, "New_notifies\n");

    n = first_notify;
    while (n != 0) {
        dump_notify_struct(out, n);
        n = n->next;
    }
}

void joblist_dump(int fd) {
    struct Job *p;
    char *buffer;

    buffer = joblistdump_headers();
    write(fd, buffer, strlen(buffer));
    free(buffer);

    /* We reuse the headers from the list */
    buffer = joblist_headers();
    write(fd, "# ", 2);
    write(fd, buffer, strlen(buffer));

    /* Show Finished jobs */
    p = first_finished_job;
    while (p != 0) {
        buffer = joblist_line(p);
        write(fd, "# ", 2);
        write(fd, buffer, strlen(buffer));
        free(buffer);
        p = p->next;
    }

    write(fd, "\n", 1);

    /* Show Queued or Running jobs */
    p = firstjob;
    while (p != 0) {
        buffer = joblistdump_torun(p);
        write(fd, buffer, strlen(buffer));
        free(buffer);
        p = p->next;
    }
}

void s_get_env(int s, int size) {
    char *var = malloc(size);
    int res = recv_bytes(s, var, size);
    if (res != size)
        error("Receiving environment variable name");

    char *val = getenv(var);
    struct Msg m = default_msg();
    m.type = LIST_LINE;
    m.u.size = val ? strlen(val) + 1 : 0;
    send_msg(s, &m);
    if (val)
        send_bytes(s, val, m.u.size);

    free(var);
}

void s_set_env(int s, int size) {
    char *var = malloc(size);
    int res = recv_bytes(s, var, size);
    if (res != size)
        error("Receiving environment variable name");

    /* get the var name */
    char *name = strtok(var, "=");

    /* get the var value */
    char *val = strtok(NULL, "=");
    setenv(name, val, 1);
    free(var);
}

void s_unset_env(int s, int size) {
    char *var = malloc(size);
    int res = recv_bytes(s, var, size);
    if (res != size)
        error("Receiving environment variable name");

    unsetenv(var);
    free(var);
}

#ifndef CPU
void s_set_free_percentage(int new_percentage) {
    if (new_percentage > 0)
        setFreePercentage(new_percentage);
    else
        warning("Received new_percentage=%i", new_percentage);
}

void s_get_free_percentage(int s) {
    struct Msg m = default_msg();
    m.type = GET_FREE_PERC;
    m.u.size = getFreePercentage();
    send_msg(s, &m);
}
#endif

void s_get_logdir(int s) {
    send_list_line(s, logdir);
}

void s_set_logdir(const char* path) {
    logdir = realloc(logdir, strlen(path) + 1);
    strcpy(logdir, path);
}

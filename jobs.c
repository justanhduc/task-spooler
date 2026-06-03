/*
    Task Spooler PLUS - a multi-user job scheduler like slurm.
    Copyright (C) 2007-2026  Kylin JIANG - Lluís Batlle i Rossell

    Please find the license in the provided COPYING file.
*/
#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>

#include "cjson/cJSON.h"

#include "defaults.h"
#include "main.h"
#include "user.h"
#include "vec.h"
#include "jobs.h"
#include "sqlite.h"
#include "runtime_limit.h"
#include "cgroups.h"
#include "info.h"
#include "list.h"
#include "utils.h"
#include "notify.h"
#include "execute.h"
#include "error.h"
#ifdef TS_CPU_BIND
#include "cpu_bind.h"
#endif

/* The list will access them */
int busy_slots = 0;
int max_slots = 1;

/* Server globals */
int jobsort_flag;
struct User *user_locker = NULL;
time_t locker_time;

/* Globals — dynamic arrays replacing linked lists */
vec_t active_jobs;    /* QUEUED, RUNNING, PAUSE, etc. */
vec_t finished_jobs;  /* FINISHED, SKIPPED */
int jobids = 1000;
/* This is used for dependencies from jobs
 * already out of the queue */
static int last_errorlevel = 0; /* Before the first job, let's consider
                                   a good previous result */
/* We need this to handle well "-d" after a "-nf" run */
static int last_finished_jobid;

char buff[256];
/* server will access them */
int max_jobs;

void init_jobs(void) {
    vec_init(&active_jobs);
    vec_init(&finished_jobs);
}

void destroy_jobs(void) {
    vec_destroy(&active_jobs);
    vec_destroy(&finished_jobs);
}

struct Job *get_job(int jobid);
int safe_pause_job(struct Job *p);

/* Return index in active_jobs, or -1 */
int findjob_idx(int jobid) {
    for (size_t i = 0; i < vec_size(&active_jobs); i++) {
        struct Job *p = (struct Job *)vec_get(&active_jobs, i);
        if (p->jobid == jobid) return (int)i;
    }
    return -1;
}

/* Return index in finished_jobs, or -1 */
int find_finished_idx(int jobid) {
    for (size_t i = 0; i < vec_size(&finished_jobs); i++) {
        struct Job *p = (struct Job *)vec_get(&finished_jobs, i);
        if (p->jobid == jobid) return (int)i;
    }
    return -1;
}

void notify_errorlevel(struct Job *p);

/* Startup init: validate against existing tables, write, set memory. */
void s_init_jobids(int i) {
    int actual = init_jobids_DB(i);
    jobids = (actual > 0) ? actual : ((i > 0) ? i : 1000);
}

/* Runtime: set jobid from client (e.g. ts -S), simple write. */
void s_set_jobids(int i) {
    jobids = i;
    set_jobids_DB(i);
}

void destroy_job(struct Job *p) {
    if (p != NULL) {
        free(p->notify_errorlevel_to);
        free(p->command);
        free(p->work_dir);
        free(p->output_filename);
        pinfo_free(&p->info);
        free(p->depend_on);
        free(p->label);
        free(p);
    }
}

void pause_job_config(struct Job* p) {
    if (p->info.pause_time == 0) {
        p->info.pause_time = get_monotonic_sec();
        // printf("set pause_time at %ld\n", p->info.pause_time);
        update_field_int64("Jobs", p->jobid, "pause_time", p->info.pause_time);
    }
}

void rerun_job_config(struct Job* p) {
    if (p->info.pause_time != 0) {
        p->info.pause_duration += get_monotonic_sec() - p->info.pause_time;
        update_field_int64("Jobs", p->jobid, "pause_duration", p->info.pause_duration);

        p->info.pause_time = 0;
        update_field_int64("Jobs", p->jobid, "pause_time", p->info.pause_time);
    }
}

void free_cores(struct Job *p) {
    if (p == NULL || p->num_allocated == 0) {
        return;
    }
    p->user->busy -= p->num_slots;
    busy_slots -= p->num_slots;
    p->num_allocated = 0;
    // p->user->queue--;
    p->user->jobs--;
}

int config_running(struct Job *p) {
    if (p == NULL || (p->state != PAUSE && p->state != QUEUED)) {
        return 1;
    }
    printf("config running %d, is_sleep %d\n", p->jobid, is_sleep(p));
    if (is_sleep(p) == 1) {
        cgroups_thaw_job(p);
    }

    // printf("Start job[%d]: PID: %d\n", p->jobid, p->pid);
    p->user->busy += p->num_slots;
    busy_slots += p->num_slots;
    p->num_allocated = p->num_slots;
    p->user->jobs++;
    p->state = RUNNING;
    rerun_job_config(p);
    return 0;
}

/* Serialize a job and add it to the JSON array. Returns 1 for success, 0 for
 * failure. */
void send_list_line(int s, char const *str) {
    struct Msg m = default_msg();

    /* Message */
    m.type = LIST_LINE;
    m.u.size = strlen(str) + 1;

    send_msg(s, &m);

    /* Send the line */
    send_bytes(s, str, m.u.size);
}

struct Job *findjob(int jobid) {
    int idx = findjob_idx(jobid);
    return (idx >= 0) ? (struct Job *)vec_get(&active_jobs, (size_t)idx) : NULL;
}

static int check_timeout(struct Job *p) {
    // printf("check job %d\n", p->jobid);
    if (p->pid == 0) {
        return 0;
    }

    time_t wall_time = i64abs(p->wall_time);
    time_t work_time = get_work_time_by_job(p);
    if (work_time <= wall_time || work_time < 3) {
        return 0;
    }

    printf("Job[%d|pid:%d] is time-out %ld sec (limit %ld)\n", p->jobid, p->pid, work_time, wall_time);
    if (safe_pause_job(p) == 0) {
        p->wall_time = -i64abs(p->wall_time) - 86400; // plus 24 hrs
        update_field_int64("Jobs", p->jobid, "wall_time", p->wall_time);
        p->state = PAUSE;
    }
    return 1; // time-out
}

static int s_check_timeout() {
    /* Collect timed-out jobs: iterate backwards so removals don't shift later indices */
    size_t n = vec_size(&active_jobs);
    vec_t timed_out;
    vec_init(&timed_out);

    for (size_t i = n; i > 0; i--) {
        size_t idx = i - 1;
        struct Job *p = (struct Job *)vec_get(&active_jobs, idx);
        if (p->state == RUNNING && check_timeout(p)) {
            vec_remove(&active_jobs, idx);
            vec_push(&timed_out, p);
        }
    }

    /* Push timed-out jobs to the back */
    size_t tn = vec_size(&timed_out);
    for (size_t i = 0; i < tn; i++)
        vec_push(&active_jobs, vec_get(&timed_out, i));

    int had_timeout = tn > 0;
    vec_destroy(&timed_out);
    return had_timeout;
}

int s_update_slots_usage() {
    int timeout_flag = s_check_timeout();

    int slots_usage = 0;
    for (int i = 0; i < vec_size(&users_vec); i++)
        USER(i)->busy = USER(i)->jobs = USER(i)->queue = 0;

    size_t n = vec_size(&active_jobs);
    for (size_t i = 0; i < n; i++) {
        struct Job *p = (struct Job *)vec_get(&active_jobs, i);
        if (p->state == RUNNING) {
            slots_usage += p->num_slots;
            p->user->busy += p->num_slots;
            p->user->jobs++;
        } else {
            p->user->queue++;
        }
    }

    if (slots_usage != busy_slots) {
        printf("Error: invalid slots: %d vs %d\n", slots_usage, busy_slots);
        busy_slots = slots_usage;
    }
    if (timeout_flag)
        next_run_job();
    return slots_usage;
}

/* Check if target_pid is a descendant (child, grandchild, etc.) of parent_pid.
   Reads /proc/<pid>/children recursively. Returns 1 if yes, 0 if no. */
static int is_descendant_pid(pid_t parent_pid, pid_t target_pid) {
    char path[256];
    char buf[4096];
    int fd;
    ssize_t n;

    snprintf(path, sizeof(path), "/proc/%d/task/%d/children", parent_pid, parent_pid);
    fd = open(path, O_RDONLY);
    if (fd == -1) {
        return 0;
    }

    n = read(fd, buf, sizeof(buf) - 1);
    close(fd);

    if (n <= 0) {
        return 0;
    }
    buf[n] = '\0';

    char *saveptr;
    char *token = strtok_r(buf, " ", &saveptr);
    while (token != NULL) {
        pid_t child_pid = (pid_t)atoi(token);
        if (child_pid == target_pid) {
            return 1;
        }
        if (is_descendant_pid(child_pid, target_pid)) {
            return 1;
        }
        token = strtok_r(NULL, " ", &saveptr);
    }

    return 0;
}

/* Find which running job a PID belongs to.
   If deep_search != 0, also checks child/grandchild processes.
   Returns jobid if found, -1 if not. */
int s_find_pid(pid_t target_pid, int deep_search) {
    if (target_pid <= 0) {
        return -1;
    }

    size_t n = vec_size(&active_jobs);
    for (size_t i = 0; i < n; i++) {
        struct Job *p = (struct Job *)vec_get(&active_jobs, i);
        if (p->state != RUNNING && p->state != PAUSE) {
            continue;
        }
        if (p->pid <= 0) {
            continue;
        }

        if (p->pid == target_pid) {
            return p->jobid;
        }

        if (deep_search && is_descendant_pid(p->pid, target_pid)) {
            return p->jobid;
        }
    }

    return -1;
}

// return 1 for running, other is dead
int s_check_running_pid(pid_t pid) {
    if (pid <= 0) {
        return 0;
    }

    // kill(pid, 0) 不真正发送信号
    if (kill(pid, 0) == 0) {
        return 1;
    }

    // EPERM 说明进程存在但无权限
    if (errno == EPERM) {
        return 1;
    }
    return 0;
}


/* s_check_relink removed — auto-reconnect branch handles this via RECONNECT protocol */

static struct Job *findjob_holding_client() {
    size_t n = vec_size(&active_jobs);
    for (size_t i = 0; i < n; i++) {
        struct Job *p = (struct Job *)vec_get(&active_jobs, i);
        if (p->state == HOLDING_CLIENT) return p;
    }
    return 0;
}

struct Job *find_finished_job(int jobid) {
    int idx = find_finished_idx(jobid);
    return (idx >= 0) ? (struct Job *)vec_get(&finished_jobs, (size_t)idx) : NULL;
}

static int count_not_finished_jobs() {
    return (int)vec_size(&active_jobs);
}

static void add_notify_errorlevel_to(struct Job *job, int jobid) {
    int *p;
    int newsize = (job->notify_errorlevel_to_size + 1) * sizeof(int);
    p = (int *)realloc(job->notify_errorlevel_to, newsize);

    if (p == 0) {
        error(
            "Cannot allocate more memory for notify_errorlist_to for jobid %i,"
            " having already %i elements",
            job->jobid, job->notify_errorlevel_to_size);
    }

    job->notify_errorlevel_to = p;
    job->notify_errorlevel_to_size += 1;
    job->notify_errorlevel_to[job->notify_errorlevel_to_size - 1] = jobid;
}

void s_kill_all_jobs(int s, struct User *user) {
    s_count_running_jobs(s, user);

    /* send running job PIDs */
    size_t n = vec_size(&active_jobs);
    for (size_t i = 0; i < n; i++) {
        struct Job *p = (struct Job *)vec_get(&active_jobs, i);
        if (p->state == RUNNING && (user == USER(0) || p->user == user)) {
            send(s, &p->pid, sizeof(int), 0);
        }
    }
}

void s_count_running_jobs(int s, struct User *user) {
    int count = 0;
    size_t n = vec_size(&active_jobs);
    struct Msg m = default_msg();

    for (size_t i = 0; i < n; i++) {
        struct Job *p = (struct Job *)vec_get(&active_jobs, i);
        if (p->state == RUNNING && (user == USER(0) || p->user == user))
            ++count;
    }

    m.type = COUNT_RUNNING;
    m.u.count_running = count;
    send_msg(s, &m);
}

void s_add_wtime(int s, int jobid, int64_t add_wtime) {
    if (jobid == 0) {
        snprintf(buff, 255, "Error: job ID is not specified. Use --job [jobid] or -J [jobid].\n");
        send_list_line(s, buff);
        return;
    }

    struct Job *p = findjob(jobid);
    if (p == NULL) {
        snprintf(buff, 255, "cannot find Job by id: %d\n", jobid);
        send_list_line(s, buff);
        return;
    }

    int64_t new_wtime = i64abs(p->wall_time) + add_wtime;
    if (new_wtime <= 0) {
        time_repr_t old_r = format_time(i64abs(p->wall_time));
        time_repr_t new_r = format_time(i64abs(new_wtime));
        snprintf(buff, 255,
                 "Error: [%d], negative wall-time after change %.3f%c => %.3f%c\n",
                 jobid, old_r.value, old_r.unit, new_r.value, new_r.unit);
        send_list_line(s, buff);
        return;
    }
    p->wall_time = (p->wall_time < 0) ? -new_wtime : new_wtime;

    insert_or_replace_DB(p, "Jobs");
    time_repr_t r = format_time(i64abs(p->wall_time));
    snprintf(buff, 255, "Set [%d] wall-time as %.3f %c\n", jobid,
             r.value, r.unit);
    send_list_line(s, buff);
    printf("s_add_wtime(): %s", buff);
}

static char *get_ofile_from_FD(int pid) {
    char path[256], buff[256] = "";
    snprintf(path, 255, "/proc/%d/fd/1", pid);
    ssize_t len = readlink(path, buff, sizeof(buff) - 1);
    if (len == -1 || len == 0) {
        return NULL;
    }
    buff[len] = '\0';
    int namesize = (int)len + 1;
    char *f = (char *)malloc(namesize);
    strncpy(f, buff, namesize);
    return f;
}

void s_mark_job_running(int jobid) {
    struct Job *p = findjob(jobid);
    if (!p) {
        error("Cannot mark the jobid %i RUNNING.", jobid);
    }
    /* RELINK removed in auto-reconnect branch */
    if (p->output_filename == NULL) {
        p->output_filename = get_ofile_from_FD(p->pid);
    }
    if (is_sleep(p) == 1) {
        p->state = PAUSE;
        return;
    }
    if (config_running(p)) {
        error("Err. in s_mark_job_running(): Cannot mark Job %d as RUNNING "
              "from state %i\n",
              jobid, p->state);
    }
}

/* -1 means nothing awaken, otherwise returns the jobid awaken */
int wake_hold_client() {
    struct Job *p;
    p = findjob_holding_client();
    if (p) {
        p->state = QUEUED;
        return p->jobid;
    }
    return -1;
}

char const *jstate2string(enum Jobstate s) {
    char const *jobstate;
    switch (s) {
    case QUEUED:         jobstate = "queued  "; break;
    case RUNNING:        jobstate = "running "; break;
    case FINISHED:       jobstate = "finished"; break;
    case SKIPPED:
    case HOLDING_CLIENT: jobstate = "skipped "; break;

    case WAIT:           jobstate = "wait    "; break;
    case DELINK:         jobstate = "delink  "; break;
    case LOCKED:         jobstate = "locked  "; break;
    case PAUSE:          jobstate = "pause   "; break;
    default:             jobstate = "UNKNOWN ";
    }
    return jobstate;
}

/*
void s_list_plain(int s) {
  struct Job *p;
  char *buffer;

  / Show Queued or Running jobs /
  p = firstjob.next;
  while (p != 0) {
    if (p->state != HOLDING_CLIENT) {
      buffer = joblist_line_plain(p);
      send_list_line(s, buffer);
      free(buffer);
    }
    p = p->next;
  }

  p = first_finished_job.next;

  / Show Finished jobs /
  while (p != 0) {
    buffer = joblist_line_plain(p);
    send_list_line(s, buffer);
    free(buffer);
    p = p->next;
  }
}
*/

static struct Job *newjobptr() {
    struct Job *p = (struct Job *)calloc(sizeof(struct Job), sizeof(char));
    if (p) vec_push(&active_jobs, p);
    return p;
}

/* Returns -1 if no last job id found */
static int find_last_jobid_in_queue(int neglect_jobid) {
    int last_jobid = -1;
    size_t n = vec_size(&active_jobs);
    for (size_t i = 0; i < n; i++) {
        struct Job *p = (struct Job *)vec_get(&active_jobs, i);
        if (p->jobid != neglect_jobid && p->jobid > last_jobid)
            last_jobid = p->jobid;
    }
    return last_jobid;
}

/* Returns -1 if no last job id found */
static int find_last_stored_jobid_finished() {
    int last_jobid = -1;
    size_t n = vec_size(&finished_jobs);
    for (size_t i = 0; i < n; i++) {
        struct Job *p = (struct Job *)vec_get(&finished_jobs, i);
        if (p->jobid > last_jobid) last_jobid = p->jobid;
    }
    return last_jobid;
}

/* Returns job id or -1 on error */
int s_newjob(int s, struct Msg *m, struct User *user) {
    s_update_slots_usage();

    struct Job *p = NULL;
    int res;
    // int waitjob_flag = 0; // 0 for newjob, 1 for WAIT and 2 for DELINK
    if (m->jobid != 0) {
        p = findjob(m->jobid);
        // if p == NULL => Manual Relink
        if (p != NULL) {
            // WAIT for restore queued tasks
            if (p->state == DELINK) {
                ; // waitjob_flag = 2;
            } else if (p->state == WAIT) {
                // jobDB_wait_num--;
                ; // waitjob_flag = 1;
            } else if (p->state == LOCKED) {
                // jobDB_wait_num--;
                ; // waitjob_flag = 1;
            } else {
                return -1;
            }
        }
    }

    if (p == NULL) {
        p = newjobptr();
        if (m->jobid != 0) {
            p->jobid = m->jobid;
            jobids = jobids > m->jobid ? jobids : m->jobid + 1;
        } else {
            p->jobid = jobids++;
        }
        if (count_not_finished_jobs() < max_jobs) {
            p->state = QUEUED;
        } else {
            p->state = HOLDING_CLIENT;
        }

        /* RELINK removed in auto-reconnect branch */
    }
    // save the user and record the number of waiting jobs
    p->user = user;
    p->client_socket = s;
    p->num_slots = m->u.newjob.num_slots;
    p->no_cpu_binding = m->u.newjob.no_cpu_binding;
    p->store_output = m->u.newjob.store_output;
    p->should_keep_finished = m->u.newjob.should_keep_finished;
    p->notify_errorlevel_to = 0;
    p->notify_errorlevel_to_size = 0;
    p->depend_on_size = m->u.newjob.depend_on_size;
    p->depend_on = 0;

    if (p->state != DELINK) {
        p->wall_time = get_max_wall_time();
        // printf("wall time = %d\n", m->u.newjob.wall_time);
        if (p->wall_time > m->u.newjob.wall_time) {
            p->wall_time = m->u.newjob.wall_time;
        }
    }
    /* this error level here is used internally to decide whether a job should
     * be run or not so it only matters whether the error level is 0 or not.
     * thus, summing the absolute error levels of all dependencies is
     * sufficient.*/
    if (p->state != WAIT && p->state != DELINK)
        p->schedule_time = m->u.newjob.schedule_time;
    p->dependency_errorlevel = 0;
    if (m->u.newjob.depend_on_size) {
        int *depend_on;
        int foo;
        depend_on = recv_ints(s, &foo);
        assert(p->depend_on_size == foo);

        /* Depend on the last queued job. */
        int idx = 0;
        for (int i = 0; i < p->depend_on_size; i++) {
            /* filter out dependencies that are current jobs */
            if (depend_on[i] >= p->jobid) {
                continue;
            }

            p->depend_on =
                (int *)realloc(p->depend_on, (idx + 1) * sizeof(int));
            /* As we already have 'p' in the queue,
             * neglect it during the find_last_jobid_in_queue() */
            if (depend_on[i] == -1) {
                p->depend_on[idx] = find_last_jobid_in_queue(p->jobid);

                /* We don't trust the last jobid in the queue (running or
                 * queued) if it's not the last added job. In that case, let the
                 * next control flow handle it as if it could not do_depend on
                 * any still queued job. */
                if (last_finished_jobid > p->depend_on[idx]) {
                    p->depend_on[idx] = -1;
                }

                /* If it's queued still without result, let it know
                 * its result to p when it finishes. */
                if (p->depend_on[idx] != -1) {
                    struct Job *depended_job;
                    depended_job = findjob(p->depend_on[idx]);
                    if (depended_job != 0) {
                        add_notify_errorlevel_to(depended_job, p->jobid);
                    } else {
                        warning("The jobid %i is queued to do_depend on the "
                                "jobid %i"
                                " suddenly non existent in the queue",
                                p->jobid, p->depend_on[idx]);
                    }
                } else /* Otherwise take the finished job, or the
                          last_errorlevel */
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
                            if (!parent) {
                                error("jobid %i suddenly disappeared from the "
                                      "finished list",
                                      ljobid);
                            }
                            p->dependency_errorlevel +=
                                abs(parent->result.errorlevel);
                        } else {
                            p->dependency_errorlevel += abs(last_errorlevel);
                        }
                    }
                }
            } else {
                /* The user decided what's the job this new job depends on */
                struct Job *depended_job;
                p->depend_on[idx] = depend_on[i];
                depended_job = findjob(p->depend_on[idx]);

                if (depended_job != 0) {
                    add_notify_errorlevel_to(depended_job, p->jobid);
                } else {
                    struct Job *parent;
                    parent = find_finished_job(p->depend_on[idx]);
                    if (parent) {
                        p->dependency_errorlevel +=
                            abs(parent->result.errorlevel);
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

    /* if dependency list is empty after removing invalid dependencies, make it
     * independent */
    if (p->depend_on_size == 0) {
        p->depend_on = 0;
    }

    if (p->state != DELINK && p->state != WAIT && p->state != LOCKED) {
        pinfo_init(&p->info);
        pinfo_set_enqueue_time(&p->info);
    }

    /* load the command */

    char *buff = malloc(m->u.newjob.command_size);
    if (buff == 0) {
        error("Cannot allocate memory in s_newjob command_size (%i)",
              m->u.newjob.command_size);
    }
    res = recv_bytes(s, buff, m->u.newjob.command_size);
    if (res == -1) {
        error("wrong bytes received");
    }

    p->command = buff;
    p->command_strip = m->u.newjob.command_size_strip;

    /* load the work dir */
    p->work_dir = 0;
    if (m->u.newjob.path_size > 0) {
        char *ptr;
        ptr = (char *)malloc(m->u.newjob.path_size);
        if (ptr == 0) {
            error("Cannot allocate memory in s_newjob path_size(%i)",
                  m->u.newjob.path_size);
        }
        res = recv_bytes(s, ptr, m->u.newjob.path_size);
        if (res == -1) {
            error("wrong bytes received");
        }
        p->work_dir = ptr;
    }

    /* load the label */
    p->label = NULL;
    if (m->u.newjob.label_size > 0) {
        char *ptr;
        ptr = (char *)malloc(m->u.newjob.label_size);
        if (ptr == 0) {
            error("Cannot allocate memory in s_newjob label_size(%i)",
                  m->u.newjob.label_size);
        }
        res = recv_bytes(s, ptr, m->u.newjob.label_size);
        if (res == -1) {
            error("wrong bytes received");
        }
        p->label = ptr;
    }


    /* load the info */
    if (m->u.newjob.env_size > 0) {
        char *ptr;
        ptr = (char *)malloc(m->u.newjob.env_size);
        if (ptr == 0) {
            error("Cannot allocate memory in s_newjob env_size(%i)",
                  m->u.newjob.env_size);
        }
        res = recv_bytes(s, ptr, m->u.newjob.env_size);
        if (res == -1) {
            error("wrong bytes received");
        }
        pinfo_addinfo(&p->info, m->u.newjob.env_size + 100, "Environment:\n%s",
                      ptr);
        free(ptr);
    }

    if (p->state == WAIT) {
        p->state = QUEUED;
        p->user->queue++;
    } else if (p->state == QUEUED) {
        insert_DB(p, "Jobs");
        p->user->queue++;
    } else if (p->state == LOCKED) {
        ;
    } else {
        insert_DB(p, "Jobs");
        p->user->queue++;
    }

    set_jobids_DB(jobids);
    return p->jobid;
}

/* This assumes the jobid exists */
void s_delete_job(int jobid) {
    int idx = findjob_idx(jobid);
    if (idx < 0)
        error("Job to be removed not found. jobid=%i", jobid);

    struct Job *p = (struct Job *)vec_remove(&active_jobs, (size_t)idx);
    destroy_job(p);
}

/* -1 if no one should be run. */
/*
    next_run_job()
    in `server.c`
    s_mark_job_running(newjob);
    s_runjob(newjob, conn);
*/
static void wake_all_held_clients(void) {
    int awaken_job;
    while ((awaken_job = wake_hold_client()) != -1) {
        struct Job *p = findjob(awaken_job);
        if (p) s_send_newjob_ok(p->client_socket, awaken_job);
    }
}

int next_run_job(void) {
    static int last_uid = 0;
    int dispatched = 0;

    while (1) {
        size_t n = vec_size(&active_jobs);
        if (n == 0) break;

        /* ---- Round-robin: one job per user per round (QUEUED or PAUSE-timeout) ---- */
        int found = 0;
        for (int i = 0; i < vec_size(&users_vec); i++) {
            int uid = (last_uid + i) % vec_size(&users_vec);
            if (USER(uid)->queue == 0) continue;

            int free_slots = max_slots - busy_slots;
            if (free_slots <= 0) break;

            for (size_t j = 0; j < n; j++) {
                struct Job *p = (struct Job *)vec_get(&active_jobs, j);
                if (p->user != USER(uid)) continue;

                if (p->state == QUEUED) {
                    if (p->depend_on_size) {
                        int ready = 1;
                        for (int k = 0; k < p->depend_on_size; k++) {
                            struct Job *dep = get_job(p->depend_on[k]);
                            if (dep != NULL &&
                                (dep->state == QUEUED || dep->state == RUNNING)) {
                                ready = 0;
                                break;
                            }
                        }
                        if (!ready) continue;
                    }

                    if (p->schedule_time > 0 && get_monotonic_sec() < p->schedule_time) continue;
                    if (p->client_socket <= 0) continue;  /* no client connected */

                    if (free_slots < p->num_slots) continue;
                    if (USER(uid)->max_slots - USER(uid)->busy < p->num_slots) continue;

                    USER(uid)->queue--;
                    s_mark_job_running(p->jobid);
                    s_send_runjob(p->client_socket, p->jobid);
                    last_uid = (uid + 1) % vec_size(&users_vec);
                    found = 1;
                    dispatched++;
                    break;
                }

                if (p->state == PAUSE && p->wall_time < 0) {
                    if (i64abs(p->wall_time) <= get_cpu_time_by_pid(p->pid)) continue;
                    if (free_slots < p->num_slots) continue;
                    if (USER(uid)->max_slots - USER(uid)->busy < p->num_slots) continue;

                    config_running(p);
                    s_send_runjob(p->client_socket, p->jobid);
                    last_uid = (uid + 1) % vec_size(&users_vec);
                    found = 1;
                    dispatched++;
                    break;
                }
            }
        }

        if (!found) break;
        wake_all_held_clients();
    }

    return dispatched;
}

/* Returns 1000 if no limit, The limit otherwise. */
static int get_max_finished_jobs() {
    char *limit;

    limit = getenv("TS_MAXFINISHED");
    if (limit == NULL) {
        return DEFAULT_MAXFINISHED;
    }
    int num = abs(atoi(limit));
    if (num < 1) {
        num = DEFAULT_MAXFINISHED;
    }
    return num;
}


/* Add the job to the finished queue. */
static void new_finished_job(struct Job *j) {
    int max = get_max_finished_jobs();

    /* If too many jobs, wipe out the first (oldest) */
    if ((int)vec_size(&finished_jobs) >= max) {
        struct Job *old = (struct Job *)vec_remove(&finished_jobs, 0);
        destroy_job(old);
    }
    vec_push(&finished_jobs, j);

    int err = insert_DB(j, "Finished");
    if (err == 0) {
        delete_DB(j->jobid, "Jobs");
        cgroups_clean_job(j);
#ifdef TS_CPU_BIND
        if (j->cpu_alloc) {
            // clean finished job   
            cpu_bind_free((struct CpuAlloc *)j->cpu_alloc);
            j->cpu_alloc = NULL;
        }
        cpu_bind_defrag();
#endif
    }
}

static int job_is_in_state(int jobid, enum Jobstate state) {
    struct Job *p;

    p = findjob(jobid);
    if (p == 0) {
        return 0;
    }
    if (p->state == state) {
        return 1;
    }
    return 0;
}

int job_is_running(int jobid) {
    return job_is_in_state(jobid, RUNNING);
}

int job_is_holding_client(int jobid) {
    return job_is_in_state(jobid, HOLDING_CLIENT);
}

/* job_finished from running to jobid */
void job_finished(const struct Result *result, int jobid) {
    if (busy_slots < 0) {
        error("Wrong state in the server. busy_slots = %i instead of greater "
              "than 0",
              busy_slots);
    }

    int idx = findjob_idx(jobid);
    if (idx < 0) error("on jobid %i finished, it doesn't exist", jobid);

    struct Job *p = (struct Job *)vec_get(&active_jobs, (size_t)idx);

    if (p->num_allocated != 0) free_cores(p);

    p->state = result->skipped ? SKIPPED : FINISHED;
    p->result = *result;
    last_finished_jobid = p->jobid;
    notify_errorlevel(p);

    pinfo_set_end_time(&p->info);
    if (result->real_sec == 0)
        p->info.start_time = p->info.enqueue_time = p->info.end_time;

    if (p->result.died_by_signal) {
        pinfo_addinfo(&p->info, 100, "Exit status: killed by signal %i\n",
                      p->result.signal);
    } else {
        pinfo_addinfo(&p->info, 100, "Exit status: died with exit code %i\n",
                      p->result.errorlevel);
    }

    /* Remove from active, optionally add to finished */
    vec_remove(&active_jobs, (size_t)idx);

    new_finished_job(p);
}


static void s_add_job(struct Job *j) {
    if (j->state == RUNNING) {
        if (j->pid > 0 && s_check_running_pid(j->pid) == 1) {
            /* Keep RUNNING — original client will reconnect via RECONNECT */
            j->client_socket = 0;
            vec_push(&active_jobs, j);
            jobids = jobids > j->jobid ? jobids : j->jobid + 1;
            return;
        } else {
            delete_DB(j->jobid, "Jobs");
        }
    } else if (j->state == QUEUED || j->state == LOCKED) {
        /* Don't fork — original client will reconnect via RECONNECT protocol */
        printf("queue job %d (waiting for client reconnect)\n", j->jobid);
        j->client_socket = 0;
        if (j->user) j->user->queue++;
        vec_push(&active_jobs, j);
        jobids = jobids > j->jobid ? jobids : j->jobid + 1;
        return;
    }

    destroy_job(j);
}

void s_read_sqlite() {
    int num_jobs, *jobs_DB = NULL;
    struct Job *job;
    num_jobs = read_jobid_DB(&(jobs_DB), "Jobs");
    printf("Jobs: #%d\n", num_jobs);
    for (int i = 0; i < num_jobs; i++) {
        job = read_DB(jobs_DB[i], "Jobs");
        if (job == NULL) {
            printf("Error in reading DB %d\n", jobs_DB[i]);
        } else {
            s_add_job(job);
        }
    }

    // finished jobs
    num_jobs = read_jobid_DB(&(jobs_DB), "Finished");
    printf("Finished:\n");
    for (int i = 0; i < num_jobs; i++) {
        job = read_DB(jobs_DB[i], "Finished");
        if (job == NULL) {
            printf("Error in reading DB %d\n", jobs_DB[i]);
        } else {
            printf("add job: %d from %d\n", job->jobid, jobs_DB[i]);
            vec_push(&finished_jobs, job);
        }
    }
    free(jobs_DB);
}

void s_clear_finished(struct User *user) {
    size_t n = vec_size(&finished_jobs);
    /* Iterate backwards so removals don't shift unvisited indices */
    for (size_t i = n; i > 0; i--) {
        size_t idx = i - 1;
        struct Job *p = (struct Job *)vec_get(&finished_jobs, idx);
        if (p->user == user || user == USER(0)) {
            delete_DB(p->jobid, "Finished");
            vec_remove(&finished_jobs, idx);
            destroy_job(p);
        }
    }
}

void s_check_holdon() {
    size_t n = vec_size(&active_jobs);
    for (size_t i = 0; i < n; i++) {
        struct Job *p = (struct Job *)vec_get(&active_jobs, i);
        if (p->pid != 0 && p->state == PAUSE) {
            if (is_sleep(p) == 0) cgroups_freeze_job(p);
        }
    }
}

// run the jobs
void s_process_runjob_ok(int jobid, char *oname, int pid) {
    struct Job *p = findjob(jobid);
    if (p == NULL) {
        error("Job %i already run not found on runjob_ok", jobid);
    }
    if (p->state == PAUSE) {
        return;
    }
    if (p->state != RUNNING) {
        error("Job %i not running, but %i on runjob_ok", jobid, p->state);
    }

    p->pid = pid;
#ifdef TS_CPU_BIND
    /* 先分配 CPU 并创建 cpuset cgroup（保证在 freezer 之前就绪） */
    if (cpu_bind_enabled() && !p->no_cpu_binding && p->num_allocated > 0) {
        p->cpu_alloc = cpu_bind_alloc_init(p->jobid, p->num_allocated);
        if (p->cpu_alloc) {
            cpu_bind_alloc((struct CpuAlloc *)p->cpu_alloc, p->num_allocated);
            cgroups_set_cpuset(p->jobid, p->pid, p->cpu_alloc);
        }
    }
#endif
    /* 再创建 cpu + freezer cgroup（freezer 是最后创建的） */
    cgroups_create_job(p);
    if (oname != NULL && strlen(oname) != 0) {
        p->output_filename = oname;
    }
    pinfo_set_start_time_check(&p->info);
    if (pid > 0) {
        // printf("s_process_runjob_ok = %d\n", jobid);
        write_logfile(p);
        // if (p->state == PAUSE) {
        //  config_running(p);
        // }
        insert_or_replace_DB(p, "Jobs");
    }
}

void s_send_runjob(int s, int jobid) {
    struct Msg m = default_msg();
    struct Job *p;

    p = findjob(jobid);
    if (p == 0) {
        error("Job %i was expected to run", jobid);
    }

    m.type = RUNJOB;

    /* TODO
     * We should make the dependencies update the jobids they're do_depending
     * on. Then, on finish, these could set the errorlevel to send to its
     * dependency childs. We cannot consider that the jobs will leave traces in
     * the finished job list
     * (-nf?) . */

    m.u.last_errorlevel = p->dependency_errorlevel;
    m.jobid = jobid;
    send_msg(s, &m);
}

void s_send_last_id(int s) {
    struct Msg m = default_msg();

    m.type = LAST_ID;
    m.jobid = jobids - 1;
    send_msg(s, &m);
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
static void s_send_removejob_nok(int s, char const *msg) {
    struct Msg m = default_msg();
    m.type = REMOVEJOB_NOK;
    m.u.size = (int)strlen(msg) + 1;
    send_msg(s, &m);
    send_bytes(s, msg, m.u.size);
}

int s_remove_job(int s, int *jobid, struct User *client) {
    struct Job *p = 0;
    struct Msg m = default_msg();
    int in_active = 1; /* 1 = active_jobs, 0 = finished_jobs */
    int remove_idx = -1;

    if (client == NULL) {
        snprintf(buff, 255, "invalid user in job removal.\n");
        s_send_removejob_nok(s, buff);
        return 0;
    }

    if (*jobid == -1) {
        /* Find the last job added */
        size_t an = vec_size(&active_jobs);
        if (an > 0) {
            remove_idx = (int)(an - 1);
            p = (struct Job *)vec_get(&active_jobs, (size_t)remove_idx);
            in_active = 1;
        } else {
            size_t fn = vec_size(&finished_jobs);
            if (fn > 0) {
                remove_idx = (int)(fn - 1);
                p = (struct Job *)vec_get(&finished_jobs, (size_t)remove_idx);
                in_active = 0;
            }
        }
    } else {
        remove_idx = findjob_idx(*jobid);
        if (remove_idx >= 0) {
            p = (struct Job *)vec_get(&active_jobs, (size_t)remove_idx);
            in_active = 1;
        } else {
            remove_idx = find_finished_idx(*jobid);
            if (remove_idx >= 0) {
                p = (struct Job *)vec_get(&finished_jobs, (size_t)remove_idx);
                in_active = 0;
            }
        }
    }

    if (p == NULL || (client != USER(0) && p->user != client)) {
        if (*jobid == -1) {
            snprintf(buff, 255, "The last job cannot be removed.\n");
        } else {
            snprintf(buff, 255, "The job %i is not in queue.\n", *jobid);
        }
        s_send_removejob_nok(s, buff);
        return 0;
    }

    if (p->state == RUNNING) {
        if (*jobid == -1)
            snprintf(buff, 255, "Running job of last job is removed.\n");
        else
            snprintf(buff, 255, "Running job [%i] PID: %d by `%s` is removed.\n",
                     *jobid, p->pid, p->user->name);
        s_send_removejob_nok(s, buff);
        return 0;
    }

    *jobid = p->jobid;
    delete_DB(p->jobid, "Jobs");
    p->state = FINISHED;
    p->result.errorlevel = -1;
    notify_errorlevel(p);
    check_notify_list(*jobid);

    /* Remove from the appropriate vec */
    if (in_active) {
        vec_remove(&active_jobs, (size_t)remove_idx);
        /* PAUSE jobs were running — preserve them in the finished queue.
           QUEUED/LOCKED jobs never ran, just free them. */
        if (p->pid != 0) {
            /* Finalize pause timing so work/pause duration display is correct */
            if (p->info.pause_time != 0) {
                p->info.pause_duration += get_monotonic_sec() - p->info.pause_time;
                p->info.pause_time = 0;
            }
            pinfo_set_end_time(&p->info);
            free_cores(p);
            cgroups_clean_job(p);
#ifdef TS_CPU_BIND
            if (p->cpu_alloc) {
                cpu_bind_free((struct CpuAlloc *)p->cpu_alloc);
                p->cpu_alloc = NULL;
            }
            cpu_bind_defrag();
#endif
            new_finished_job(p);
        } else {
            destroy_job(p);
        }
    } else {
        vec_remove(&finished_jobs, (size_t)remove_idx);
        delete_DB(p->jobid, "Finished");
        destroy_job(p);
    }

    m.type = REMOVEJOB_OK;
    send_msg(s, &m);
    return 1;
}

void s_send_newjob_ok(int socket, int jobid) {
    struct Msg m = default_msg();
    m.type = NEWJOB_OK;
    m.jobid = jobid;
    send_msg(socket, &m);
}

struct Job *get_job(int jobid) {
    struct Job *j;

    j = findjob(jobid);
    if (j != NULL) {
        return j;
    }

    j = find_finished_job(jobid);

    if (j != NULL) {
        return j;
    }

    return 0;
}

int safe_pause_job(struct Job *p) {
    cgroups_freeze_job(p);
    free_cores(p);
    pause_job_config(p);
    return 0;
    /*
    kill(p->pid, SIGSTOP);
    kill_pids(p->pid, SIGSTOP, NULL);
    usleep(10000);
    if (is_sleep(p) == 1) {
        free_cores(p);
        pause_job_config(p);
        return 0;
    } else {
        kill_pids(p->pid, SIGCONT, NULL);
        return 1;
    }
    */
}

/* Don't complain, if the socket doesn't exist */
void destroy_finished_job(struct Job *j) {
    int idx = find_finished_idx(j->jobid);
    if (idx < 0)
        error("Cannot destroy the expected job %i", j->jobid);
    vec_remove(&finished_jobs, (size_t)idx);
    destroy_job(j);
}

/* This is called when a job finishes */
void s_set_max_slots(int s, int new_max_slots) {
    if (new_max_slots > 0) {
        max_slots = new_max_slots;
    } else {
        warning("Received new_max_slots=%i", new_max_slots);
    }
    if (s > 0) {
        snprintf(buff, 255, "Reset the number of slots: %d\n", max_slots);
        send_list_line(s, buff);
    }
}

void s_get_max_slots(int s) {
    struct Msg m = default_msg();

    /* Message */
    m.type = GET_MAX_SLOTS_OK;
    m.u.max_slots = max_slots;

    send_msg(s, &m);
}

/* move jobid upto the top of list */
void s_move_urgent(int s, int jobid) {
    struct Job *p = 0;

    if (jobid == -1) {
        size_t an = vec_size(&active_jobs);
        if (an > 0) p = (struct Job *)vec_get(&active_jobs, an - 1);
    } else {
        int idx = findjob_idx(jobid);
        if (idx >= 0) p = (struct Job *)vec_get(&active_jobs, (size_t)idx);
    }

    if (p == 0 || vec_size(&active_jobs) == 0) {
        snprintf(buff, 255, "The job %i cannot be urged.\n", jobid);
        send_list_line(s, buff);
        return;
    }

    /* Move to front of vec */
    int idx = findjob_idx(jobid);
    if (idx < 0) {
        snprintf(buff, 255, "The job %i cannot be urged.\n", jobid);
        send_list_line(s, buff);
        return;
    }
    struct Job *p_moved = (struct Job *)vec_remove(&active_jobs, (size_t)idx);
    vec_insert(&active_jobs, 0, p_moved);
    movetop_DB(jobid);
    /* send_urgent_ok inline */
    {
        struct Msg m = default_msg();
        m.type = URGENT_OK;
        send_msg(s, &m);
    }
}

void s_swap_jobs(int s, int jobid1, int jobid2) {
    int idx1 = findjob_idx(jobid1);
    int idx2 = findjob_idx(jobid2);

    if (idx1 < 0 || idx2 < 0) {
        snprintf(buff, 255, "The jobs %i and %i cannot be swapped.\n", jobid1, jobid2);
        send_list_line(s, buff);
        return;
    }

    struct Job *p1 = (struct Job *)vec_get(&active_jobs, (size_t)idx1);
    struct Job *p2 = (struct Job *)vec_get(&active_jobs, (size_t)idx2);
    vec_set(&active_jobs, (size_t)idx1, p2);
    vec_set(&active_jobs, (size_t)idx2, p1);
    swap_DB(jobid1, jobid2);
    /* send_swap_jobs_ok inline */
    {
        struct Msg m = default_msg();
        m.type = SWAP_JOBS_OK;
        send_msg(s, &m);
    }
}

static void dump_job_struct(FILE *out, const struct Job *p) {
    fprintf(out, "  new_job\n");
    fprintf(out, "    jobid %i\n", p->jobid);
    fprintf(out, "    command \"%s\"\n", p->command);
    fprintf(out, "    state %s\n", jstate2string(p->state));
    fprintf(out, "    result.errorlevel %i\n", p->result.errorlevel);
    fprintf(out, "    output_filename \"%s\"\n",
            p->output_filename ? p->output_filename : "NULL");
    fprintf(out, "    store_output %i\n", p->store_output);
    fprintf(out, "    pid %i\n", p->pid);
    fprintf(out, "    should_keep_finished %i\n", p->should_keep_finished);
}

void dump_jobs_struct(FILE *out) {
    size_t an = vec_size(&active_jobs);
    size_t fn = vec_size(&finished_jobs);

    fprintf(out, "New_jobs\n");

    for (size_t i = 0; i < an; i++)
        dump_job_struct(out, (const struct Job *)vec_get(&active_jobs, i));
    for (size_t i = 0; i < fn; i++)
        dump_job_struct(out, (const struct Job *)vec_get(&finished_jobs, i));
}

void joblist_dump(int fd) {
    char *buffer;

    buffer = joblistdump_headers();
    write(fd, buffer, strlen(buffer));
    free(buffer);

    buffer = joblist_headers();
    write(fd, "# ", 2);
    write(fd, buffer, strlen(buffer));

    /* Show Finished jobs */
    size_t fn = vec_size(&finished_jobs);
    for (size_t i = 0; i < fn; i++) {
        struct Job *p = (struct Job *)vec_get(&finished_jobs, i);
        buffer = joblist_line(p);
        write(fd, "# ", 2);
        write(fd, buffer, strlen(buffer));
        free(buffer);
    }

    write(fd, "\n", 1);

    /* Show Queued or Running jobs */
    size_t an = vec_size(&active_jobs);
    for (size_t i = 0; i < an; i++) {
        struct Job *p = (struct Job *)vec_get(&active_jobs, i);
        buffer = joblistdump_torun(p);
        write(fd, buffer, strlen(buffer));
        free(buffer);
    }
}

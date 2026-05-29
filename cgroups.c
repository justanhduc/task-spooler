#include "main.h"
#include "cgroups.h"
#include "error.h"
#include "list.h"
#include "user.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* ---- path constants ---- */

#define SPOOL_PATTERN           "TASK_SPOOLER_%d_%d"

#ifdef CGROUP_V2

#define CGROUP_DIR              "/sys/fs/cgroup"
#define CGROUP_CONTROLLERS      "/sys/fs/cgroup/cgroup.controllers"
#define CGROUP_SUBTREE_CONTROL  "/sys/fs/cgroup/cgroup.subtree_control"
#define CGROUP_PROCS            "cgroup.procs"
#define CGROUP_CPU_MAX          "cpu.max"
#define CGROUP_FREEZE           "cgroup.freeze"
#define CGROUP_EVENTS           "cgroup.events"

#else

#define CGROUP_V1_CPU_DIR       "/sys/fs/cgroup/cpu"
#define CGROUP_V1_FREEZE_DIR    "/sys/fs/cgroup/freezer"

#endif /* CGROUP_V2 */

/* ---- shared helpers ---- */

static inline int cg_write(char const *path, char const *fmt, ...) {
    char buf[256];
    va_list ap;

    va_start(ap, fmt);
    int len = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    if (len < 0) {
        fprintf(stderr, "Error: vsnprintf failed\n");
        return -1;
    }

    int fd = open(path, O_WRONLY);
    if (fd < 0) {
        fprintf(stderr, "Error: open %s failed: %s\n", path, strerror(errno));
        return -1;
    }

    ssize_t ret = write(fd, buf, len);
    if (ret != len) {
        fprintf(stderr, "Error: write %s failed: %s\n", path, strerror(errno));
        close(fd);
        return -1;
    }

    close(fd);
    return 0;
}

static inline int cg_mkdir(char const *path) {
    if (mkdir(path, 0755) != 0 && errno != EEXIST) {
        fprintf(stderr, "Error: failed to create %s: %s\n in cg_mkdir()", path,
                strerror(errno));
        return -1;
    }
    return 0;
}

static inline void cg_group_name(int jobid, pid_t pid, char *buf, size_t size) {
    snprintf(buf, size, SPOOL_PATTERN, jobid, pid);
}

/* ================================================================
 *  Internal implementations — v1 or v2, selected at compile time
 * ================================================================ */
#ifdef CGROUP_V2

/* ---- v2 internal ---- */

void cgroups_v2_init(void) {
    FILE *fp = fopen(CGROUP_CONTROLLERS, "r");
    if (!fp) {
        fprintf(stderr, "Warning: cannot open %s\n", CGROUP_CONTROLLERS);
        return;
    }

    char line[256];
    int has_cpu = 0;
    if (fgets(line, sizeof(line), fp)) {
        if (strstr(line, "cpu")) {
            has_cpu = 1;
        }
    }
    fclose(fp);

    if (has_cpu) {
        cg_write(CGROUP_SUBTREE_CONTROL, "+cpu");
    }
}

static int cgroups_v2_cpu(int jobid, pid_t pid, int cpus) {
    char path[512];
    char group[64];

    cg_group_name(jobid, pid, group, sizeof(group));
    snprintf(path, sizeof(path), CGROUP_DIR "/%s", group);
    printf("Create cgroups v2 folder at %s\n", path);

    long period = 100000;
    long quota  = (long)cpus * period;

    if (cg_mkdir(path) != 0) {
        return -1;
    }

    snprintf(path, sizeof(path), CGROUP_DIR "/%s/" CGROUP_CPU_MAX, group);
    if (cg_write(path, "%ld %ld", quota, period) != 0) {
        return -1;
    }

    snprintf(path, sizeof(path), CGROUP_DIR "/%s/" CGROUP_PROCS, group);
    if (cg_write(path, "%d", pid) != 0) {
        return -1;
    }

    return 0;
}

static int cgroups_v2_wait_frozen(int jobid, pid_t pid) {
    char path[512];
    char group[64];
    cg_group_name(jobid, pid, group, sizeof(group));
    snprintf(path, sizeof(path), CGROUP_DIR "/%s/" CGROUP_EVENTS, group);

    for (int i = 0; i < 200; i++) {
        FILE *fp = fopen(path, "r");
        if (!fp) {
            return -1;
        }
        char line[256];
        int found = 0;
        while (fgets(line, sizeof(line), fp)) {
            if (strncmp(line, "frozen ", 7) == 0 && atoi(line + 7) == 1) {
                found = 1;
                break;
            }
        }
        fclose(fp);
        if (found) {
            return 0;
        }
        usleep(50000);
    }
    fprintf(stderr, "Timeout waiting for cgroup freeze on job %d, pid %d\n",
            jobid, pid);
    return -1;
}

static int cgroups_v2_wait_thawed(int jobid, pid_t pid) {
    char path[512];
    char group[64];
    cg_group_name(jobid, pid, group, sizeof(group));
    snprintf(path, sizeof(path), CGROUP_DIR "/%s/" CGROUP_EVENTS, group);

    for (int i = 0; i < 200; i++) {
        FILE *fp = fopen(path, "r");
        if (!fp) {
            return 0;
        }
        char line[256];
        int thawed = 0;
        while (fgets(line, sizeof(line), fp)) {
            if (strncmp(line, "frozen ", 7) == 0 && atoi(line + 7) == 0) {
                thawed = 1;
                break;
            }
        }
        fclose(fp);
        if (thawed) {
            return 0;
        }
        usleep(50000);
    }
    fprintf(stderr, "Timeout waiting for cgroup thaw on job %d, pid %d\n",
            jobid, pid);
    return -1;
}

static int cgroups_v2_freeze(int jobid, pid_t pid) {
    char path[512];
    char group[64];
    cg_group_name(jobid, pid, group, sizeof(group));
    snprintf(path, sizeof(path), CGROUP_DIR "/%s/" CGROUP_FREEZE, group);
    if (cg_write(path, "1") != 0) {
        return -1;
    }
    return cgroups_v2_wait_frozen(jobid, pid);
}

static int cgroups_v2_thaw(int jobid, pid_t pid) {
    char path[512];
    char group[64];
    cg_group_name(jobid, pid, group, sizeof(group));
    snprintf(path, sizeof(path), CGROUP_DIR "/%s/" CGROUP_FREEZE, group);
    if (cg_write(path, "0") != 0) {
        return -1;
    }
    return cgroups_v2_wait_thawed(jobid, pid);
}

static int cgroups_v2_check_frozen(int jobid, pid_t pid) {
    char path[512];
    char group[64];
    cg_group_name(jobid, pid, group, sizeof(group));
    snprintf(path, sizeof(path), CGROUP_DIR "/%s/" CGROUP_EVENTS, group);

    FILE *fp = fopen(path, "r");
    if (!fp) {
        return -1;
    }

    char line[256];
    int frozen = -1;
    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, "frozen ", 7) == 0) {
            frozen = atoi(line + 7);
            break;
        }
    }
    fclose(fp);
    return frozen;
}

static int cgroups_v2_cleanup(const char *group) {
    char path[512];
    char procs_path[512];

    snprintf(path, sizeof(path), CGROUP_DIR "/%s", group);
    snprintf(procs_path, sizeof(procs_path), "%s/" CGROUP_PROCS, path);

    /* Thaw first */
    {
        char freeze_path[512];
        snprintf(freeze_path, sizeof(freeze_path), "%s/" CGROUP_FREEZE, path);
        FILE *fp = fopen(freeze_path, "w");
        if (fp) {
            fprintf(fp, "0");
            fclose(fp);
        }
    }

    /* Kill remaining processes */
    for (int retry = 0; retry < 100; retry++) {
        FILE *fp = fopen(procs_path, "r");
        if (!fp) {
            break;
        }

        char line[64];
        while (fgets(line, sizeof(line), fp)) {
            pid_t p = (pid_t)atoi(line);
            if (p > 0) {
                if (kill(p, SIGKILL) != 0 && errno != ESRCH) {
                    fprintf(stderr, "Error: kill(%d) failed: %s\n", p,
                            strerror(errno));
                }
            }
        }
        fclose(fp);

        fp = fopen(procs_path, "r");
        if (!fp) break;
        int empty = (fgets(line, sizeof(line), fp) == NULL);
        fclose(fp);
        if (empty) break;

        usleep(10000);
    }

    /* Remove directory */
    for (int i = 0; i < 100; i++) {
        if (rmdir(path) == 0 || errno == ENOENT) {
            return 0;
        }
        if (errno != EBUSY) {
            fprintf(stderr, "Error: rmdir %s failed: %s\n", path, strerror(errno));
            return -1;
        }
        usleep(50000);
    }

    fprintf(stderr, "Failed to remove cgroup %s after 100 retries\n", path);
    return -1;
}

static void cgroups_v2_clean_dir(const char *spool_dir) {
    DIR *dir = opendir(spool_dir);
    if (!dir) {
        fprintf(stderr, "Error: cannot open dir %s: %s\n", spool_dir, strerror(errno));
        return;
    }

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        int jobid;
        pid_t pid;
        if (sscanf(ent->d_name, "TASK_SPOOLER_%d_%d", &jobid, &pid) != 2) {
            continue;
        }
        if (s_check_running_pid(pid) == 0) {
            printf("[CLEAN] dead task: %s/%s\n", spool_dir, ent->d_name);
            cgroups_v2_cleanup(ent->d_name);
        }
    }
    closedir(dir);
}

static int cgroups_v2_freeze_ok(int jobid, pid_t pid) {
    char path[512];
    char group[64];
    cg_group_name(jobid, pid, group, sizeof(group));
    snprintf(path, sizeof(path), CGROUP_DIR "/%s/" CGROUP_PROCS, group);

    FILE *fp = fopen(path, "r");
    if (!fp) {
        return 0;
    }

    char line[64];
    int found = 0;
    while (fgets(line, sizeof(line), fp)) {
        if ((pid_t)atoi(line) == pid) {
            found = 1;
            break;
        }
    }
    fclose(fp);
    return found;
}

#else /* CGROUP_V1 */

/* ---- v1 internal ---- */

static int cgroups_v1_cpu(int jobid, pid_t pid, int cpus) {
    char buf[256];
    char group[64];
    cg_group_name(jobid, pid, group, sizeof(group));
    snprintf(buf, sizeof(buf), CGROUP_V1_CPU_DIR "/%s", group);
    printf("Create cgroups folder at %s\n", buf);

    char path[512];
    long period = 100000;
    long quota = (long)cpus * period;

    if (cg_mkdir(buf) != 0) {
        return -1;
    }

    snprintf(path, sizeof(path), "%s/cpu.cfs_period_us", buf);
    if (cg_write(path, "%ld", period) != 0) {
        return -1;
    }

    snprintf(path, sizeof(path), "%s/cpu.cfs_quota_us", buf);
    if (cg_write(path, "%ld", quota) != 0) {
        return -1;
    }

    snprintf(path, sizeof(path), "%s/cgroup.procs", buf);
    if (cg_write(path, "%d", pid) != 0) {
        return -1;
    }
    return 0;
}

static int cgroups_v1_freeze(int jobid, pid_t pid) {
    char buf[256];
    char group[64];
    cg_group_name(jobid, pid, group, sizeof(group));
    snprintf(buf, sizeof(buf), CGROUP_V1_FREEZE_DIR "/%s/freezer.state", group);
    if (cg_write(buf, "FROZEN") != 0) {
        return -1;
    }
    return 0;
}

static int cgroups_v1_thaw(int jobid, pid_t pid) {
    char buf[256];
    char group[64];
    cg_group_name(jobid, pid, group, sizeof(group));
    snprintf(buf, sizeof(buf), CGROUP_V1_FREEZE_DIR "/%s/freezer.state", group);
    if (cg_write(buf, "THAWED") != 0) {
        return -1;
    }
    return 0;
}

static int cgroups_v1_mkdir_freeze(int jobid, pid_t pid) {
    char buf[256];
    char group[64];
    cg_group_name(jobid, pid, group, sizeof(group));
    snprintf(buf, sizeof(buf), CGROUP_V1_FREEZE_DIR "/%s", group);
    printf("Create cgroups freeze folder at %s\n", buf);

    char path[512];

    if (cg_mkdir(buf) != 0) {
        return -1;
    }

    snprintf(path, sizeof(path), "%s/cgroup.procs", buf);
    if (cg_write(path, "%d", pid) != 0) {
        return -1;
    }
    cgroups_v1_thaw(jobid, pid);
    return 0;
}

int cgroups_v1_freeze_ok(int jobid, pid_t pid) {
    char buf[256];
    char group[64];
    cg_group_name(jobid, pid, group, sizeof(group));
    snprintf(buf, sizeof(buf), CGROUP_V1_FREEZE_DIR "/%s/freezer.state", group);
    return access(buf, F_OK) == 0;
}

static int cgroups_v1_check_frozen(int jobid, pid_t pid) {
    char buf[256];
    char state[32];
    char group[64];
    cg_group_name(jobid, pid, group, sizeof(group));
    snprintf(buf, sizeof(buf), CGROUP_V1_FREEZE_DIR "/%s", group);

    char path[512];
    snprintf(path, sizeof(path), "%s/freezer.state", buf);

    if (access(path, F_OK) != 0) {
        return -1;
    }

    FILE *fp = fopen(path, "r");
    if (!fp) {
        printf("Cannot open %s\n", path);
        return -1;
    }

    if (fgets(state, sizeof(state), fp) == NULL) {
        fclose(fp);
        return -1;
    }
    fclose(fp);

    state[strcspn(state, "\n")] = '\0';

    if (strcmp(state, "FROZEN") == 0) {
        return 1;
    } else if (strcmp(state, "FREEZING") == 0) {
        return 0;
    } else {
        return -1;
    }
}

static int cgroups_v1_cleanup_freeze(const char *group) {
    char buf[256];
    snprintf(buf, sizeof(buf), CGROUP_V1_FREEZE_DIR "/%s", group);

    char freeze_state[512];
    snprintf(freeze_state, sizeof(freeze_state), "%s/freezer.state", buf);

    FILE *fp = fopen(freeze_state, "w");
    if (fp) {
        fprintf(fp, "THAWED");
        fclose(fp);
    }

    for (int i = 0; i < 100; i++) {
        if (rmdir(buf) == 0) {
            return 0;
        }

        if (errno == ENOENT) {
            return 0;
        }

        if (errno == EBUSY) {
            usleep(50000);
            continue;
        }

        fprintf(stderr, "Cannot remove cgroup %s: %s\n", buf, strerror(errno));
        return -1;
    }

    fprintf(stderr, "Failed to remove cgroup %s after 100 retries\n", buf);
    return -1;
}

static int cgroups_v1_cleanup_cpu(const char *group) {
    char path[512];
    char procs_path[512];

    snprintf(path, sizeof(path), CGROUP_V1_CPU_DIR "/%s", group);

    if (access(path, F_OK) != 0) {
        return 0;
    }

    snprintf(procs_path, sizeof(procs_path), "%s/cgroup.procs", path);

    for (int retry = 0; retry < 100; retry++) {
        FILE *fp = fopen(procs_path, "r");

        if (!fp) {
            fprintf(stderr, "Error: open %s failed: %s\n", procs_path,
                    strerror(errno));
            break;
        }

        char line[64];

        while (fgets(line, sizeof(line), fp)) {
            pid_t pid = (pid_t)atoi(line);

            if (pid <= 0) {
                continue;
            }

            if (kill(pid, SIGKILL) != 0) {
                if (errno != ESRCH) {
                    fprintf(stderr, "Error: kill(%d) failed: %s\n", pid,
                            strerror(errno));
                }
            }
        }

        fclose(fp);

        fp = fopen(procs_path, "r");

        if (!fp) {
            break;
        }

        int empty = (fgets(line, sizeof(line), fp) == NULL);

        fclose(fp);

        if (empty) {
            break;
        }

        usleep(10000);
    }

    /* Remove directory with EBUSY retry */
    for (int i = 0; i < 100; i++) {
        if (rmdir(path) == 0 || errno == ENOENT) {
            return 0;
        }
        if (errno != EBUSY) {
            fprintf(stderr, "Error: rmdir %s failed: %s\n", path, strerror(errno));
            return -1;
        }
        usleep(50000);
    }

    fprintf(stderr, "Failed to remove cgroup %s after 100 retries\n", path);
    return -1;
}

typedef int (*cleanup_func_t)(const char *);

static void cgroups_v1_clean_dir(const char *spool_dir, cleanup_func_t cleanup_func) {
    DIR *dir = opendir(spool_dir);
    if (!dir) {
        fprintf(stderr, "Error: cannot open dir %s: %s\n", spool_dir, strerror(errno));
        return;
    }

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        int jobid;
        pid_t pid;

        if (sscanf(ent->d_name, "TASK_SPOOLER_%d_%d", &jobid, &pid) != 2) {
            continue;
        }
        if (s_check_running_pid(pid) == 0) {
            printf("[CLEAN] dead task: %s%s\n", spool_dir, ent->d_name);
            cleanup_func(ent->d_name);
        }
    }

    closedir(dir);
}

#endif /* CGROUP_V2 */

/* ================================================================
 *  Public interface — dispatches to v1 or v2 via #ifdef
 * ================================================================ */

int cgroups_is_frozen(const struct Job *p) {
#ifdef CGROUP_V2
    return cgroups_v2_check_frozen(p->jobid, p->pid) == 1;
#else
    return cgroups_v1_check_frozen(p->jobid, p->pid) != -1;
#endif
}

int cgroups_freeze_job(const struct Job *p) {
#ifdef CGROUP_V2
    return cgroups_v2_freeze(p->jobid, p->pid);
#else
    if (is_sleep(p) == 0) {
        kill(p->pid, SIGCONT);
        kill_pids(p->pid, SIGCONT, NULL);
        usleep(20000);
    }
    return cgroups_v1_freeze(p->jobid, p->pid);
#endif
}

int cgroups_thaw_job(const struct Job *p) {
#ifdef CGROUP_V2
    int ret = cgroups_v2_thaw(p->jobid, p->pid);
#else
    int ret = cgroups_v1_thaw(p->jobid, p->pid);
#endif
    kill(p->pid, SIGCONT);
    kill_pids(p->pid, SIGCONT, NULL);
    return ret;
}

void cgroups_create_job(const struct Job *p) {
    if (p->pid == 0) {
        printf("cannot set cgroups for group: missing PID\n");
        return;
    }
#ifdef CGROUP_V2
    cgroups_v2_cpu(p->jobid, p->pid, p->num_allocated);
#else
    cgroups_v1_cpu(p->jobid, p->pid, p->num_allocated);
    cgroups_v1_mkdir_freeze(p->jobid, p->pid);
#endif
}

void cgroups_clean_job(const struct Job *p) {
    int jobid = p->jobid;
    pid_t pid = p->pid;
    if (pid == 0) {
        printf("cannot set cgroups for group: missing PID\n");
        return;
    }
    /* Only refuse cleanup for actively RUNNING jobs.
       PAUSE (frozen) jobs are safe — the cleanup functions thaw first. */
    if (p->state == RUNNING) {
        printf("Cannot clear the cgroups for a RUNNING job[%d](PID: %d)\n",
               jobid, pid);
        return;
    }

    char group[64];
    cg_group_name(jobid, pid, group, sizeof(group));
#ifdef CGROUP_V2
    printf("clear cgroups v2: %s\n", group);
    cgroups_v2_cleanup(group);
#else
    printf("clear cgroups: %s\n", group);
    cgroups_v1_cleanup_freeze(group);
    cgroups_v1_cleanup_cpu(group);
#endif
}

void cgroups_clean_all_finished(void) {
#ifdef CGROUP_V2
    cgroups_v2_clean_dir(CGROUP_DIR);
#else
    cgroups_v1_clean_dir(CGROUP_V1_CPU_DIR, cgroups_v1_cleanup_cpu);
    cgroups_v1_clean_dir(CGROUP_V1_FREEZE_DIR, cgroups_v1_cleanup_freeze);
#endif
}

int cgroups_freeze_ok(int jobid, pid_t pid) {
#ifdef CGROUP_V2
    return cgroups_v2_freeze_ok(jobid, pid);
#else
    return cgroups_v1_freeze_ok(jobid, pid);
#endif
}

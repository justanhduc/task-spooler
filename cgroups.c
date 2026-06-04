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
#ifdef TS_CPU_BIND
#include "cpu_bind.h"
#endif

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
    int has_cpuset = 0;
    if (fgets(line, sizeof(line), fp)) {
        if (strstr(line, "cpu")) {
            has_cpu = 1;
        }
#ifdef TS_CPU_BIND
        if (strstr(line, "cpuset")) {
            has_cpuset = 1;
        }
#endif
    }
    fclose(fp);

    if (has_cpu) {
        cg_write(CGROUP_SUBTREE_CONTROL, "+cpu");
    }
#ifdef TS_CPU_BIND
    if (has_cpuset) {
        cg_write(CGROUP_SUBTREE_CONTROL, "+cpuset");
    }
#endif
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

#ifdef TS_CPU_BIND
static int cgroups_v1_cleanup_cpuset(const char *group) {
    char path[512];
    snprintf(path, sizeof(path), "/sys/fs/cgroup/cpuset/%s", group);

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
    fprintf(stderr, "Failed to remove cpuset cgroup %s after 100 retries\n", path);
    return -1;
}
#endif /* TS_CPU_BIND */

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
#ifdef TS_CPU_BIND
    cgroup_io_wait_if_busy();
#endif
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
#ifdef TS_CPU_BIND
    cgroup_io_wait_if_busy();
#endif
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
    int cpus = p->num_slots;
    if (cpus < 1) cpus = 1;
#ifdef CGROUP_V2
    cgroups_v2_cpu(p->jobid, p->pid, cpus);
#else
    cgroups_v1_cpu(p->jobid, p->pid, cpus);
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
#ifdef TS_CPU_BIND
    cgroups_v1_cleanup_cpuset(group);
#endif
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

/* ================================================================
 *  cgroups_set_cpuset — 将 cpu_bind 分配结果写入 cgroup cpuset
 *  v1: /sys/fs/cgroup/cpuset/TASK_SPOOLER_<jobid>_<pid>/cpuset.cpus
 *  v2: /sys/fs/cgroup/TASK_SPOOLER_<jobid>_<pid>/cpuset.cpus (统一层级)
 * ================================================================ */
#ifdef TS_CPU_BIND
void cgroups_set_cpuset(int jobid, pid_t pid, const void *valloc)
{
    cgroup_io_wait_if_busy();

    const struct CpuAlloc *alloc = (const struct CpuAlloc *)valloc;
    char *cpus_str = cpu_bind_format_cpus(alloc);
    char *mems_str = cpu_bind_format_mems(alloc);
    char path[512];
    char group[64];

    if (!cpus_str) return;

    cg_group_name(jobid, pid, group, sizeof(group));

#ifdef CGROUP_V2
    /* v2: 统一层级，需要先确保目录存在 */
    snprintf(path, sizeof(path), CGROUP_DIR "/%s", group);
    cg_mkdir(path);

    snprintf(path, sizeof(path), CGROUP_DIR "/%s/cpuset.cpus", group);
    cg_write(path, "%s", cpus_str);

    if (mems_str) {
        snprintf(path, sizeof(path), CGROUP_DIR "/%s/cpuset.mems", group);
        cg_write(path, "%s", mems_str);
    }
#else
    /* v1: 独立的 cpuset 层级，需要创建目录 */
    snprintf(path, sizeof(path), "/sys/fs/cgroup/cpuset/%s", group);
    cg_mkdir(path);

    snprintf(path, sizeof(path), "/sys/fs/cgroup/cpuset/%s/cpuset.cpus", group);
    cg_write(path, "%s", cpus_str);

    if (mems_str) {
        snprintf(path, sizeof(path), "/sys/fs/cgroup/cpuset/%s/cpuset.mems", group);
        cg_write(path, "%s", mems_str);
    }

    /* 将 PID 写入 cgroup.procs */
    snprintf(path, sizeof(path), "/sys/fs/cgroup/cpuset/%s/cgroup.procs", group);
    cg_write(path, "%d", (int)pid);
#endif

    free(cpus_str);
    free(mems_str);
}

/* 读取 cgroup 文件内容到静态缓冲区，返回指针或 NULL */
static const char *cg_read_file(const char *path)
{
    static char buf[4096];
    int fd = open(path, O_RDONLY);
    if (fd < 0) return NULL;
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return NULL;
    buf[n] = '\0';
    /* 去掉尾部换行 */
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == ' '))
        buf[--n] = '\0';
    return buf;
}

/* 解析 "0,1" 格式的 mems 字符串 → primary_node + mem_nodes 位图 */
static void parse_mems(const char *str, int *primary_node, int *mem_nodes)
{
    *primary_node = -1;
    *mem_nodes = 0;
    if (!str || !*str) return;

    char copy[128];
    strncpy(copy, str, sizeof(copy) - 1);
    copy[sizeof(copy) - 1] = '\0';

    char *tok = strtok(copy, ",");
    while (tok) {
        int n = atoi(tok);
        if (n >= 0 && n < NUM_NODES) {
            *mem_nodes |= (1 << n);
            if (*primary_node < 0) *primary_node = n;
        }
        tok = strtok(NULL, ",");
    }
}

/* 清除孤儿 cgroup：将进程移回根 cgroup 后删除目录 */
static void cgroup_remove_orphan(const char *scan_dir, const char *group)
{
    char path[512];
    char procs_path[512];

    /* 读取所有 PID 并写回根 cgroup.procs（释放约束） */
    snprintf(procs_path, sizeof(procs_path), "%s/%s/cgroup.procs", scan_dir, group);
    snprintf(path, sizeof(path), "%s/cgroup.procs", scan_dir);

    FILE *fp = fopen(procs_path, "r");
    if (fp) {
        /* 把还在里面的进程全部移到父级 */
        char lprocs[512];
        snprintf(lprocs, sizeof(lprocs), "%s/cgroup.procs", scan_dir);
        char line[64];
        while (fgets(line, sizeof(line), fp)) {
            pid_t p = (pid_t)atoi(line);
            if (p > 0)
                cg_write(lprocs, "%d", (int)p);
        }
        fclose(fp);
    }

    /* 删目录 */
    snprintf(path, sizeof(path), "%s/%s", scan_dir, group);
    for (int i = 0; i < 100; i++) {
        if (rmdir(path) == 0 || errno == ENOENT) return;
        if (errno != EBUSY) {
            fprintf(stderr, "Error: rmdir %s: %s\n", path, strerror(errno));
            return;
        }
        usleep(50000);
    }
    fprintf(stderr, "Failed to remove cgroup %s after 100 retries\n", path);
}

/* 重启恢复：扫描 cgroup 目录重建 cpu_bind 状态
 *
 * 流程：
 *   1. 扫描 cgroups 下的 TASK_SPOOLER_<jobid>_<pid> 目录
 *   2. 对每个目录：
 *      a. pid 不存活 → 清理残留 cgroup
 *      b. pid 存活但 ts 无此 job → 孤儿，删 cgroup，让系统自由调度
 *      c. pid 存活且 ts 有此 job → 恢复 alloc
 *   3. 遍历 active_jobs，对有 alloc 的 job 统一写 cpuset.cpus（确保一致）
 */
void cgroups_restore_all_cpu_bind(void)
{
    /* 重启恢复不受 --no-bind 影响：已有 cgroup 中的 binding 需要重建 */

    const char *scan_dir;
#ifdef CGROUP_V2
    scan_dir = CGROUP_DIR;           /* /sys/fs/cgroup/ — 统一层级 */
#else
    scan_dir = "/sys/fs/cgroup/cpuset";  /* v1 独立 cpuset 层级 */
#endif

    DIR *dir = opendir(scan_dir);
    if (!dir) {
        fprintf(stderr, "Warning: cannot open %s for cpu_bind restore\n", scan_dir);
        return;
    }

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        int jobid;
        pid_t pid;

        if (sscanf(ent->d_name, "TASK_SPOOLER_%d_%d", &jobid, &pid) != 2)
            continue;
        if (pid <= 0) continue;

        /* 检查 pid 是否存活 */
        int pid_alive = (kill(pid, 0) == 0 || errno == EPERM);

        /* 查找 ts 中是否有该 job */
        struct Job *p = findjob(jobid);

        if (!pid_alive) {
            /* 进程已死 → 清理残留 cgroup */
            printf("[RESTORE] cleanup stale cgroup %s (PID %d dead)\n",
                   ent->d_name, pid);
            cgroup_remove_orphan(scan_dir, ent->d_name);

        } else if (!p) {
            /* pid 存活但 ts 不管理此 job → 孤儿，删 cgroup 让系统接管 */
            printf("[RESTORE] orphan cgroup %s (no ts job %d), releasing PID %d\n",
                   ent->d_name, jobid, pid);
            cgroup_remove_orphan(scan_dir, ent->d_name);

        } else if (p->state == RUNNING || p->state == PAUSE) {
            /* ts 管理的活跃 job → 恢复 alloc */
            char path[512];
            const char *content;

            snprintf(path, sizeof(path), "%s/%s/cpuset.cpus", scan_dir, ent->d_name);
            content = cg_read_file(path);
            if (!content) {
                printf("[RESTORE] %s: no cpuset.cpus, skip\n", ent->d_name);
                continue;
            }

            int cpus[MAX_OS_CPU];
            int count = cpu_bind_parse_cpuset(content, cpus, MAX_OS_CPU);
            if (count <= 0) continue;

            /* 读取 mems */
            int mem_nodes = 0, primary_node = -1;
            snprintf(path, sizeof(path), "%s/%s/cpuset.mems", scan_dir, ent->d_name);
            content = cg_read_file(path);
            if (content)
                parse_mems(content, &primary_node, &mem_nodes);

            p->cpu_alloc = cpu_bind_claim(p->jobid, cpus, count,
                                           mem_nodes, primary_node);
            if (p->cpu_alloc) {
                printf("[RESTORE] job %d: restored %d CPUs, mem=0x%x\n",
                       p->jobid, count, mem_nodes);
            }
        }
        /* 其他状态（QUEUED/FINISHED 等）— cgroup 不应该存在，忽略 */
    }
    closedir(dir);

    /* 统一写回 cpuset.cpus：遍历所有 active_jobs，确保 cgroup 一致 */
    size_t n = vec_size(&active_jobs);
    for (size_t i = 0; i < n; i++) {
        struct Job *p = (struct Job *)vec_get(&active_jobs, i);
        if ((p->state != RUNNING && p->state != PAUSE) || !p->cpu_alloc)
            continue;
        if (p->pid <= 0) continue;

        printf("[RESTORE] set cpu for job %d (PID %d)\n", p->jobid, p->pid);
        cgroups_set_cpuset(p->jobid, p->pid, p->cpu_alloc);
    }
}
#endif /* TS_CPU_BIND */

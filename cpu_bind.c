/* cpu_bind.c — CPU binding allocator
 *
 * 独立实现，完全按照 docs/cpu-bind-spec.md 规格编写。
 * 编译: gcc -std=c11 -Wall -Wextra -c cpu_bind.c -o cpu_bind.o
 */

#include "cpu_bind.h"
#include "vec.h"
#include "main.h"
#include "cgroups.h"
#include "list.h"

#ifdef TS_CPU_BIND
#include <pthread.h>
#include <time.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <stdarg.h>
#include <signal.h>
#endif

/* 拓扑实例 — 由 gen_topology.py 生成 */
struct Topology sys_topology = TOPOLOGY_INIT;

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ================================================================
 *  内部变量
 * ================================================================ */

int cpu_owner[MAX_OS_CPU];          /* 0=空闲, >0=jobid */
static int cpu_bind_disabled;
static int cpu_bind_defrag_disabled;
static int cpu_to_group[MAX_OS_CPU];  /* cpu→group 索引表 */
vec_t cpu_allocs;                    /* 活跃 alloc 列表（供 defrag） */

#ifdef TS_CPU_BIND
static pthread_mutex_t cgroup_io_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  cgroup_io_cond  = PTHREAD_COND_INITIALIZER;
static int             cgroup_io_busy;
#endif

/* ================================================================
 *  cpu→group 索引表在 cpu_bind_init 中构建，替代遍历查找
 * ================================================================
 *  take_from_group — 从单个 group 取走 N 个空闲核心（不要求连续）
 * ================================================================ */

static int take_from_group(struct CoreGroup *group, int N,
                            struct CpuAlloc *alloc)
{
    int taken = 0;
    for (int i = 0; i < group->num_cores && taken < N; i++) {
        int cpu = group->os_cpus[i];
        if (cpu_owner[cpu] == 0) {
            alloc->os_cpus[alloc->count++] = cpu;
            cpu_owner[cpu] = alloc->jobid;
            taken++;
        }
    }

    group->free_count -= taken;
    sys_topology.nodes[group->node_id].free_count -= taken;

    /* 取完后 group 还有空闲 → 碎片 group */
    if (group->free_count > 0)
        alloc->quality++;

    return taken;
}

/* ================================================================
 *  alloc_from_groups — 从一组 group 中分配 N 个核心
 * ================================================================ */

static void alloc_from_groups(int N, int *group_ids,
                             int num_groups, struct CpuAlloc *alloc)
{
    int remaining = N;

    while (remaining > 0) {
        /* best-fit: 找 free_count ≥ remaining 且 excess 最小的 group */
        struct CoreGroup *best = NULL;
        int best_excess = INT_MAX;
        int best_idx = -1;

        for (int i = 0; i < num_groups; i++) {
            struct CoreGroup *g = &sys_topology.groups[group_ids[i]];
            if (g->free_count >= remaining) {
                int excess = g->free_count - remaining;
                if (excess < best_excess) {
                    best_excess = excess;
                    best = g;
                    best_idx = i;
                }
            }
        }

        if (best) {
            /* 找到了一个能一次满足剩余需求的 group */
            int got = take_from_group(best, remaining, alloc);
            remaining -= got;
            continue;
        }

        /* 没有单组能装下 → 取 free_count 最大的 */
        int max_free = -1;
        best_idx = -1;

        for (int i = 0; i < num_groups; i++) {
            struct CoreGroup *g = &sys_topology.groups[group_ids[i]];
            if (g->free_count > 0 && g->free_count > max_free) {
                max_free = g->free_count;
                best_idx = i;
            }
        }

        if (best_idx < 0) {
            fprintf(stderr, "ERROR: alloc_from_groups: no groups have free cores, "
                    "remaining=%d\n", remaining);
            alloc->error = 1;
            break;
        }

        int got = take_from_group(&sys_topology.groups[group_ids[best_idx]], remaining, alloc);
        remaining -= got;
    }

    if (remaining > 0) {
        fprintf(stderr, "ERROR: alloc_from_groups: shortfall, got %d of %d\n",
                N - remaining, N);
        alloc->error = 1;
    }
}



/* 给 qsort 用的 int 比较器（CPU 编号 0~255，直接减安全） */
static int int_cmp(const void *a, const void *b)
{
    return *(const int *)a - *(const int *)b;
}

/* 分配后处理：排序 + 校验（#define CPU_BIND_NO_POST_CHECK 可关闭所有检查） */
static void cpu_bind_post_alloc(struct CpuAlloc *alloc)
{
    qsort(alloc->os_cpus, (size_t)alloc->count, sizeof(int), int_cmp);

#ifndef CPU_BIND_NO_POST_CHECK
    /* ① os_cpus[] 中的 CPU 确实被本 job 占用 */
    for (int i = 0; i < alloc->count; i++) {
        int cpu = alloc->os_cpus[i];
        if (cpu_owner[cpu] != alloc->jobid) {
            fprintf(stderr, "ERROR: cpu %d owner=%d, expected jobid=%d\n",
                    cpu, cpu_owner[cpu], alloc->jobid);
            alloc->error = 1;
        }
    }

    /* ② cpu_owner 中本 job 的计数与 alloc->count 一致 */
    int owner_cnt = 0;
    for (int cpu = 0; cpu < MAX_OS_CPU; cpu++) {
        if (cpu_owner[cpu] == alloc->jobid)
            owner_cnt++;
    }
    if (owner_cnt != alloc->count) {
        fprintf(stderr, "ERROR: owner count %d != alloc count %d\n",
                owner_cnt, alloc->count);
        alloc->error = 1;
    }

    /* ③ quality 已在 take_from_group 中写入，此处不再重复计算 */
#endif /* CPU_BIND_NO_POST_CHECK */
}

/* 给 qsort 用的节点排序比较器：按 free_count 降序 */
static int node_free_desc(const void *a, const void *b)
{
    int ia = *(const int *)a;
    int ib = *(const int *)b;
    int fa = sys_topology.nodes[ia].free_count;
    int fb = sys_topology.nodes[ib].free_count;
    return (fb > fa) - (fb < fa);  /* 降序 */
}

/* ================================================================
 *  cross_node_merge — 跨节点合并分配
 *  按 free_count 降序选取节点，合并它们的 group 一次分配
 * ================================================================ */

static void cross_node_merge(int N, struct CpuAlloc *alloc)
{
    int order[NUM_NODES];
    for (int i = 0; i < sys_topology.num_nodes; i++)
        order[i] = i;

    qsort(order, (size_t)sys_topology.num_nodes, sizeof(int),
          node_free_desc);

    /* 确定主节点 */
    if (N > MAX_CORES_PER_GROUP) {
        alloc->primary_node = -1;
    } else {
        /* 前几个节点可能 free_count 相同（平局） */
        int nt = 0;
        int best_free = sys_topology.nodes[order[0]].free_count;
        while (nt < sys_topology.num_nodes &&
            sys_topology.nodes[order[nt]].free_count == best_free)
            nt++;

        if (nt > 1)
            alloc->primary_node = order[alloc->jobid % nt];
        else
            alloc->primary_node = order[0];
    }

    /* 从大到小取节点，合并它们的 group，直到够 N */
    int groups_collected[NUM_GROUPS];
    int ng = 0;
    int total = 0;
    int bitmap = 0;

    for (int i = 0; i < sys_topology.num_nodes && total < N; i++) {
        int ni = order[i];
        struct NodeInfo *node = &sys_topology.nodes[ni];

        for (int j = 0; j < node->num_groups; j++)
            groups_collected[ng++] = node->group_ids[j];

        total  += node->free_count;
        bitmap |= (1 << ni);
    }

    alloc->mem_nodes = bitmap;
    if (ng > 0) {
        alloc_from_groups(N, groups_collected, ng, alloc);
    }
}

/* ================================================================
 *  公共 API
 * ================================================================ */

void cpu_bind_init(void)
{
    cpu_bind_disabled = 0;
    cpu_bind_defrag_disabled = 0;
    vec_init(&cpu_allocs);

    for (int i = 0; i < MAX_OS_CPU; i++) {
        cpu_owner[i] = 0;
        cpu_to_group[i] = -1;   /* 标记无效 */
    }

    /* 构建 cpu→group 反向索引表 */
    for (int i = 0; i < sys_topology.num_groups; i++) {
        struct CoreGroup *g = &sys_topology.groups[i];
        for (int j = 0; j < g->num_cores; j++)
            cpu_to_group[g->os_cpus[j]] = i;
    }

    for (int i = 0; i < sys_topology.num_groups; i++)
        sys_topology.groups[i].free_count =
            sys_topology.groups[i].num_cores;

    for (int i = 0; i < sys_topology.num_nodes; i++)
        sys_topology.nodes[i].free_count =
            sys_topology.nodes[i].num_cores;
}

int cpu_bind_enabled(void)
{
    return !cpu_bind_disabled;
}

void cpu_bind_set_disabled(int disabled)
{
    cpu_bind_disabled = disabled;
}

int cpu_bind_defrag_enabled(void)
{
    return !cpu_bind_defrag_disabled;
}

void cpu_bind_set_defrag_disabled(int disabled)
{
    cpu_bind_defrag_disabled = disabled;
}

void cpu_bind_alloc(struct CpuAlloc *alloc, int N)
{
    if (cpu_bind_disabled || N <= 0) {
        alloc->error = 1;
        return;
    }

    /* 检查总空闲 */
    int total_free = 0;
    for (int i = 0; i < sys_topology.num_nodes; i++)
        total_free += sys_topology.nodes[i].free_count;

    if (total_free < N) {
        alloc->error = 1;
        return;
    }

    alloc->count = 0;

    /* 选择分配路径 */
    int best_node = -1;
    int max_free  = -1;
    for (int i = 0; i < sys_topology.num_nodes; i++) {
        int fc = sys_topology.nodes[i].free_count;
        if (fc >= N && fc > max_free) {
            max_free  = fc;
            best_node = i;
        }
    }

    if (best_node >= 0) {
        /* 单节点分配 — NUMA 亲和路径 */
        struct NodeInfo *node = &sys_topology.nodes[best_node];
        alloc_from_groups(N, node->group_ids, node->num_groups, alloc);
        alloc->primary_node = node->node_id;
        alloc->mem_nodes    = 1 << node->node_id;
    } else {
        cross_node_merge(N, alloc);
    }

    cpu_bind_post_alloc(alloc);
}

void cpu_bind_free(struct CpuAlloc *alloc)
{
    if (!alloc)
        return;

    for (int i = 0; i < alloc->count; i++) {
        int cpu = alloc->os_cpus[i];
        cpu_owner[cpu] = 0;

        int gi = cpu_to_group[cpu];
        sys_topology.groups[gi].free_count++;
        sys_topology.nodes[sys_topology.groups[gi].node_id].free_count++;
    }

    /* 从 vec 中移除 */
    for (size_t i = 0; i < vec_size(&cpu_allocs); i++) {
        if (vec_get(&cpu_allocs, i) == alloc) {
            vec_remove(&cpu_allocs, i);
            break;
        }
    }

    free(alloc);
}

struct CpuAlloc *cpu_bind_alloc_init(int jobid, int max_cpus)
{
    struct CpuAlloc *alloc = calloc(1,
        sizeof(struct CpuAlloc) + (size_t)max_cpus * sizeof(int));
    if (!alloc)
        return NULL;

    alloc->slots     = max_cpus;
    alloc->jobid     = jobid;

    vec_push(&cpu_allocs, alloc);
    return alloc;
}

/* defrag 排序：(quality ASC, N DESC) */
static int alloc_defrag_cmp(const void *a, const void *b)
{
    struct CpuAlloc *pa = *(struct CpuAlloc **)a;
    struct CpuAlloc *pb = *(struct CpuAlloc **)b;
    if (pa->quality != pb->quality)
        return pa->quality - pb->quality;
    return pb->count - pa->count;
}

#ifdef TS_CPU_BIND
/* ---- sync defrag work (cpu_bind_defrag_run uses main-thread globals) ---- */
struct defrag_work {
    int n;                    /* first quality>0 alloc index */
    int count;                /* total alloc count (== vec_size(&cpu_allocs)) */
    struct Job **jobs;        /* pre-computed job pointers (size count) */
};

/* ---- async I/O work: self-contained copy, no main-thread pointers ---- */
struct defrag_io_job {
    int    jobid;
    pid_t  pid;
    int    state;          /* RUNNING or PAUSE */
    int    is_sleep;       /* v1: need SIGCONT before freeze if !sleep */
    char  *cpus_str;       /* pre-formatted cpuset.cpus */
    char  *mems_str;       /* pre-formatted cpuset.mems */
};

struct defrag_io_work {
    int count;
    struct defrag_io_job *jobs;   /* self-contained array */
};

/* ---- cgroup I/O wait: blocks main thread until defrag I/O finishes ---- */
void cgroup_io_wait_if_busy(void)
{
    pthread_mutex_lock(&cgroup_io_mutex);
    while (cgroup_io_busy) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += 30;
        int rc = pthread_cond_timedwait(&cgroup_io_cond, &cgroup_io_mutex, &ts);
        if (rc == ETIMEDOUT) {
            fprintf(stderr, "WARNING: defrag I/O thread stalled 30s, clearing lock\n");
            cgroup_io_busy = 0;
            break;
        }
    }
    pthread_mutex_unlock(&cgroup_io_mutex);
}

/* ---- sync defrag: freeze → rebuild → realloc → cpuset → thaw (all inline) ---- */
static void cpu_bind_defrag_run(struct defrag_work *work)
{
    int n = work->n;
    int count = work->count;
    struct Job **jobs = work->jobs;
    struct CpuAlloc **allocs = (struct CpuAlloc **)cpu_allocs.data;
    int *skipped = (int *)calloc((size_t)count, sizeof(int));

    if (n >= count) {
        free(skipped);
        return;
    }

    /* step 1: freeze quality>0 jobs */
    for (int i = n; i < count; i++) {
        struct Job *p = jobs[i];
        if (p && p->pid > 0 && (p->state == RUNNING || p->state == PAUSE))
            cgroups_freeze_job(p);
    }

    /* step 2: rebuild allocation state */
    memset(cpu_owner, 0, sizeof(cpu_owner));
    for (int i = 0; i < sys_topology.num_groups; i++)
        sys_topology.groups[i].free_count = sys_topology.groups[i].num_cores;

    for (int i = 0; i < sys_topology.num_nodes; i++)
        sys_topology.nodes[i].free_count = sys_topology.nodes[i].num_cores;

    /* restore quality=0 allocs */
    for (int i = 0; i < n; i++) {
        struct CpuAlloc *a = allocs[i];
        for (int j = 0; j < a->count; j++) {
            int cpu = a->os_cpus[j];
            cpu_owner[cpu] = a->jobid;
            int gi = cpu_to_group[cpu];
            sys_topology.groups[gi].free_count--;
            sys_topology.nodes[sys_topology.groups[gi].node_id].free_count--;
        }
    }

    /* step 3: same-node reallocation */
    for (int i = n; i < count; i++) {
        struct CpuAlloc *a = allocs[i];
        int N = a->count;
        int pn = a->primary_node;

        if (pn >= 0 && pn < sys_topology.num_nodes &&
            sys_topology.nodes[pn].free_count >= N) {
            a->count   = 0;
            a->error   = 0;
            a->quality = 0;
            alloc_from_groups(N, sys_topology.nodes[pn].group_ids,
                              sys_topology.nodes[pn].num_groups, a);
            cpu_bind_post_alloc(a);
            if (jobs[i]) {
                cgroups_set_cpuset(a->jobid, jobs[i]->pid, a);
                cgroups_thaw_job(jobs[i]);
            }
        } else {
            skipped[i] = 1;
        }
    }

    /* step 4: cross-node merge for remaining */
    for (int i = n; i < count; i++) {
        if (skipped[i]) {
            struct CpuAlloc *a = allocs[i];
            int N = a->count;
            int pn = a->primary_node;
            int mn = a->mem_nodes;
            a->count   = 0;
            a->error   = 0;
            a->quality = 0;
            cross_node_merge(N, a);
            a->primary_node = pn;
            a->mem_nodes = mn;
            cpu_bind_post_alloc(a);
            if (jobs[i]) {
                cgroups_set_cpuset(a->jobid, jobs[i]->pid, a);
                cgroups_thaw_job(jobs[i]);
            }
        }
    }
    free(skipped);
}

/* ---- helpers for raw cgroup writes used by the I/O thread ---- */

static void io_cg_group_name(int jobid, pid_t pid, char *buf, size_t size)
{
    snprintf(buf, size, "TASK_SPOOLER_%d_%d", jobid, pid);
}

static int io_cg_write(const char *path, const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    int len = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (len < 0) return -1;

    int fd = open(path, O_WRONLY);
    if (fd < 0) return -1;
    ssize_t ret = write(fd, buf, len);
    close(fd);
    return (ret == len) ? 0 : -1;
}

#ifdef CGROUP_V2
static int io_freeze_v2(int jobid, pid_t pid)
{
    char path[512], group[64];
    io_cg_group_name(jobid, pid, group, sizeof(group));
    snprintf(path, sizeof(path), "/sys/fs/cgroup/%s/cgroup.freeze", group);
    if (io_cg_write(path, "1") != 0) return -1;

    /* wait for frozen */
    char events[512];
    snprintf(events, sizeof(events), "/sys/fs/cgroup/%s/cgroup.events", group);
    for (int i = 0; i < 200; i++) {
        FILE *fp = fopen(events, "r");
        if (!fp) return -1;
        char line[256];
        int found = 0;
        while (fgets(line, sizeof(line), fp)) {
            if (strncmp(line, "frozen ", 7) == 0 && atoi(line + 7) == 1) {
                found = 1; break;
            }
        }
        fclose(fp);
        if (found) return 0;
        usleep(50000);
    }
    fprintf(stderr, "Timeout waiting for cgroup freeze on job %d, pid %d\n", jobid, pid);
    return -1;
}

static int io_thaw_v2(int jobid, pid_t pid)
{
    char path[512], group[64];
    io_cg_group_name(jobid, pid, group, sizeof(group));
    snprintf(path, sizeof(path), "/sys/fs/cgroup/%s/cgroup.freeze", group);
    io_cg_write(path, "0");

    /* wait for thawed */
    char events[512];
    snprintf(events, sizeof(events), "/sys/fs/cgroup/%s/cgroup.events", group);
    for (int i = 0; i < 200; i++) {
        FILE *fp = fopen(events, "r");
        if (!fp) return 0;
        char line[256];
        int thawed = 0;
        while (fgets(line, sizeof(line), fp)) {
            if (strncmp(line, "frozen ", 7) == 0 && atoi(line + 7) == 0) {
                thawed = 1; break;
            }
        }
        fclose(fp);
        if (thawed) return 0;
        usleep(50000);
    }
    fprintf(stderr, "Timeout waiting for cgroup thaw on job %d, pid %d\n", jobid, pid);
    return -1;
}
#else /* CGROUP_V1 */
static int io_freeze_v1(int jobid, pid_t pid, int is_sleep)
{
    if (!is_sleep) {
        kill(pid, SIGCONT);
        usleep(20000);
    }
    char path[512], group[64];
    io_cg_group_name(jobid, pid, group, sizeof(group));
    snprintf(path, sizeof(path), "/sys/fs/cgroup/freezer/%s/freezer.state", group);
    return io_cg_write(path, "FROZEN");
}

static int io_thaw_v1(int jobid, pid_t pid)
{
    char path[512], group[64];
    io_cg_group_name(jobid, pid, group, sizeof(group));
    snprintf(path, sizeof(path), "/sys/fs/cgroup/freezer/%s/freezer.state", group);
    int ret = io_cg_write(path, "THAWED");
    kill(pid, SIGCONT);
    return ret;
}
#endif /* CGROUP_V2 */

static void io_write_cpuset(int jobid, pid_t pid, const char *cpus_str, const char *mems_str)
{
    char path[512], group[64];
    io_cg_group_name(jobid, pid, group, sizeof(group));

#ifdef CGROUP_V2
    snprintf(path, sizeof(path), "/sys/fs/cgroup/%s", group);
    mkdir(path, 0755);
    snprintf(path, sizeof(path), "/sys/fs/cgroup/%s/cpuset.cpus", group);
    {
        int fd = open(path, O_WRONLY);
        if (fd >= 0) { write(fd, cpus_str, strlen(cpus_str)); close(fd); }
    }
    if (mems_str && mems_str[0]) {
        snprintf(path, sizeof(path), "/sys/fs/cgroup/%s/cpuset.mems", group);
        int fd = open(path, O_WRONLY);
        if (fd >= 0) { write(fd, mems_str, strlen(mems_str)); close(fd); }
    }
#else
    snprintf(path, sizeof(path), "/sys/fs/cgroup/cpuset/%s", group);
    mkdir(path, 0755);
    snprintf(path, sizeof(path), "/sys/fs/cgroup/cpuset/%s/cpuset.cpus", group);
    {
        int fd = open(path, O_WRONLY);
        if (fd >= 0) { write(fd, cpus_str, strlen(cpus_str)); close(fd); }
    }
    if (mems_str && mems_str[0]) {
        snprintf(path, sizeof(path), "/sys/fs/cgroup/cpuset/%s/cpuset.mems", group);
        int fd = open(path, O_WRONLY);
        if (fd >= 0) { write(fd, mems_str, strlen(mems_str)); close(fd); }
    }
    snprintf(path, sizeof(path), "/sys/fs/cgroup/cpuset/%s/cgroup.procs", group);
    {
        char pbuf[32];
        int len = snprintf(pbuf, sizeof(pbuf), "%d", (int)pid);
        int fd = open(path, O_WRONLY);
        if (fd >= 0) { write(fd, pbuf, len); close(fd); }
    }
#endif
}

/* ---- I/O-only thread: self-contained data, no main-thread refs ---- */
static void *defrag_io_thread(void *arg)
{
    struct defrag_io_work *work = (struct defrag_io_work *)arg;
    struct timespec t0, t1;

    clock_gettime(CLOCK_MONOTONIC, &t0);
    printf("[defrag I/O] start (jobs=%d)\n", work->count);

    pthread_mutex_lock(&cgroup_io_mutex);
    /* cgroup_io_busy is already 1 (set by main thread before spawn) */

    /* Phase 1: freeze */
    for (int i = 0; i < work->count; i++) {
        struct defrag_io_job *e = &work->jobs[i];
        if (e->pid > 0 && (e->state == RUNNING || e->state == PAUSE)) {
#ifdef CGROUP_V2
            io_freeze_v2(e->jobid, e->pid);
#else
            io_freeze_v1(e->jobid, e->pid, e->is_sleep);
#endif
        }
    }

    /* Phase 2: write cpusets */
    for (int i = 0; i < work->count; i++) {
        struct defrag_io_job *e = &work->jobs[i];
        if (e->cpus_str)
            io_write_cpuset(e->jobid, e->pid, e->cpus_str, e->mems_str);
    }

    /* Phase 3: thaw */
    for (int i = 0; i < work->count; i++) {
        struct defrag_io_job *e = &work->jobs[i];
        if (e->pid > 0) {
#ifdef CGROUP_V2
            io_thaw_v2(e->jobid, e->pid);
#else
            io_thaw_v1(e->jobid, e->pid);
#endif
        }
    }

    cgroup_io_busy = 0;
    pthread_cond_broadcast(&cgroup_io_cond);
    pthread_mutex_unlock(&cgroup_io_mutex);

    clock_gettime(CLOCK_MONOTONIC, &t1);
    {
        double elapsed = (t1.tv_sec - t0.tv_sec)
                       + (t1.tv_nsec - t0.tv_nsec) / 1e9;
        printf("[defrag I/O] done in %.3f s\n", elapsed);
    }

    /* cleanup: thread owns all data, frees everything */
    for (int i = 0; i < work->count; i++) {
        free(work->jobs[i].cpus_str);
        free(work->jobs[i].mems_str);
    }
    free(work->jobs);
    free(work);
    return NULL;
}
#endif /* TS_CPU_BIND */

/* ---- sync defrag: kept for API backward compat, unused internally ---- */
int cpu_bind_defrag(void)
{
#ifdef TS_CPU_BIND
    int count = (int)vec_size(&cpu_allocs);
    if (count <= 0) return 0;

    struct CpuAlloc **allocs = (struct CpuAlloc **)cpu_allocs.data;
    struct Job **jobs = (struct Job **)calloc((size_t)count, sizeof(struct Job *));
    if (!jobs) return -1;

    qsort(allocs, (size_t)count, sizeof(struct CpuAlloc *), alloc_defrag_cmp);

    int n = 0;
    while (n < count && allocs[n]->quality == 0) n++;

    if (n >= count) { free(jobs); return 0; }

    for (int i = n; i < count; i++) {
        struct Job *p = findjob(allocs[i]->jobid);
        if (p && p->pid > 0 && (p->state == RUNNING || p->state == PAUSE))
            jobs[i] = p;
    }

    struct defrag_work work = { .n = n, .count = count, .jobs = jobs };
    cpu_bind_defrag_run(&work);
    free(jobs);
    return 0;
#else
    return 0;
#endif
}

#ifdef TS_CPU_BIND
/* ================================================================
 *  Async defrag: computation in main thread, I/O in background thread
 * ================================================================ */

int cpu_bind_defrag_start(void)
{
    if (!cpu_bind_defrag_enabled()) return -1;

    int count = (int)vec_size(&cpu_allocs);
    if (count <= 0) return 0;

    struct CpuAlloc **allocs = (struct CpuAlloc **)cpu_allocs.data;

    /* ---- main thread: sort allocs ---- */
    qsort(allocs, (size_t)count, sizeof(struct CpuAlloc *), alloc_defrag_cmp);

    /* ---- main thread: find split point ---- */
    int n = 0;
    while (n < count && allocs[n]->quality == 0)
        n++;

    if (n >= count) return 0;  /* all quality=0, nothing to defrag */

    /* ---- main thread: build jobs[] array (safe access to active_jobs) ---- */
    struct Job **jobs = (struct Job **)calloc((size_t)count, sizeof(struct Job *));
    if (!jobs) return -1;

    for (int i = n; i < count; i++) {
        struct Job *p = findjob(allocs[i]->jobid);
        if (p && p->pid > 0 && (p->state == RUNNING || p->state == PAUSE))
            jobs[i] = p;
    }

    /* ---- main thread: rebuild allocation state (computation only) ---- */
    memset(cpu_owner, 0, sizeof(cpu_owner));
    for (int i = 0; i < sys_topology.num_groups; i++)
        sys_topology.groups[i].free_count = sys_topology.groups[i].num_cores;
    for (int i = 0; i < sys_topology.num_nodes; i++)
        sys_topology.nodes[i].free_count = sys_topology.nodes[i].num_cores;

    /* restore quality=0 allocs */
    for (int i = 0; i < n; i++) {
        struct CpuAlloc *a = allocs[i];
        for (int j = 0; j < a->count; j++) {
            int cpu = a->os_cpus[j];
            cpu_owner[cpu] = a->jobid;
            int gi = cpu_to_group[cpu];
            sys_topology.groups[gi].free_count--;
            sys_topology.nodes[sys_topology.groups[gi].node_id].free_count--;
        }
    }

    /* same-node reallocation for quality>0 */
    int *skipped = (int *)calloc((size_t)count, sizeof(int));
    for (int i = n; i < count; i++) {
        struct CpuAlloc *a = allocs[i];
        int N = a->count;
        int pn = a->primary_node;
        if (pn >= 0 && pn < sys_topology.num_nodes &&
            sys_topology.nodes[pn].free_count >= N) {
            a->count   = 0;
            a->error   = 0;
            a->quality = 0;
            alloc_from_groups(N, sys_topology.nodes[pn].group_ids,
                              sys_topology.nodes[pn].num_groups, a);
            cpu_bind_post_alloc(a);
        } else {
            skipped[i] = 1;
        }
    }
    /* cross-node merge for remaining */
    for (int i = n; i < count; i++) {
        if (skipped[i]) {
            struct CpuAlloc *a = allocs[i];
            int N = a->count;
            int pn = a->primary_node;
            int mn = a->mem_nodes;
            a->count   = 0;
            a->error   = 0;
            a->quality = 0;
            cross_node_merge(N, a);
            a->primary_node = pn;
            a->mem_nodes = mn;
            cpu_bind_post_alloc(a);
        }
    }
    free(skipped);

    /* ---- main thread: build self-contained I/O work for thread ---- */
    int n_io = 0;
    for (int i = n; i < count; i++) {
        if (jobs[i]) n_io++;
    }

    struct defrag_io_work *work = malloc(sizeof(struct defrag_io_work));
    if (!work) { free(jobs); return -1; }
    work->count = n_io;
    work->jobs = NULL;

    if (n_io > 0) {
        work->jobs = calloc((size_t)n_io, sizeof(struct defrag_io_job));
        if (!work->jobs) { free(jobs); free(work); return -1; }
        int idx = 0;
        for (int i = n; i < count; i++) {
            if (!jobs[i]) continue;
            struct defrag_io_job *e = &work->jobs[idx++];
            e->jobid    = allocs[i]->jobid;
            e->pid      = jobs[i]->pid;
            e->state    = jobs[i]->state;
            e->is_sleep = is_sleep(jobs[i]);
            e->cpus_str = cpu_bind_format_cpus(allocs[i]);
            e->mems_str = cpu_bind_format_mems(allocs[i]);
        }
    }
    free(jobs);  /* all needed data copied, no more Job* refs */

    /* wait for in-flight defrag I/O, then mark busy before spawn */
    cgroup_io_wait_if_busy();
    pthread_mutex_lock(&cgroup_io_mutex);
    cgroup_io_busy = 1;
    pthread_mutex_unlock(&cgroup_io_mutex);

    pthread_t thr;
    int rc = pthread_create(&thr, NULL, defrag_io_thread, work);
    if (rc != 0) {
        pthread_mutex_lock(&cgroup_io_mutex);
        cgroup_io_busy = 0;
        pthread_cond_broadcast(&cgroup_io_cond);
        pthread_mutex_unlock(&cgroup_io_mutex);
        for (int i = 0; i < n_io; i++) {
            free(work->jobs[i].cpus_str);
            free(work->jobs[i].mems_str);
        }
        free(work->jobs);
        free(work);
        return -1;
    }
    pthread_detach(thr);
    return 0;
}
#endif /* TS_CPU_BIND */

/* ================================================================
 *  cpu_bind_parse_cpuset — 解析 "0-3,6,7-8" 格式为 int 数组
 *  返回解析出的 CPU 个数，-1 表示出错
 * ================================================================ */
int cpu_bind_parse_cpuset(const char *str, int *cpus, int max_cpus)
{
    if (!str || !*str) return -1;

    int count = 0;
    const char *p = str;

    while (*p) {
        /* 跳过空白和逗号 */
        while (*p == ' ' || *p == ',') p++;
        if (!*p) break;

        /* 解析起始值 */
        int start = 0;
        while (*p >= '0' && *p <= '9') {
            start = start * 10 + (*p - '0');
            p++;
        }
        if (start < 0 || start >= MAX_OS_CPU)
            return -1;

        if (*p == '-') {
            p++; /* 跳过 '-' */
            int end = 0;
            while (*p >= '0' && *p <= '9') {
                end = end * 10 + (*p - '0');
                p++;
            }
            if (end >= MAX_OS_CPU || end < start)
                return -1;
            for (int i = start; i <= end && count < max_cpus; i++)
                cpus[count++] = i;
        } else {
            /* 单个 CPU */
            if (count < max_cpus)
                cpus[count++] = start;
        }
    }

    return count;
}

/* ================================================================
 *  cpu_bind_claim — 重启恢复用：直接认领指定 CPU，不经过分配器
 *  从 cpuset 解析结果重建 CpuAlloc，更新 cpu_owner / free_count
 * ================================================================ */
struct CpuAlloc *cpu_bind_claim(int jobid, const int *cpus, int count,
                                int mem_nodes, int primary_node)
{
    if (count <= 0) return NULL;

    struct CpuAlloc *alloc = calloc(1,
        sizeof(struct CpuAlloc) + (size_t)count * sizeof(int));
    if (!alloc) return NULL;

    alloc->jobid       = jobid;
    alloc->count       = count;
    alloc->slots       = count;
    alloc->mem_nodes   = mem_nodes;
    alloc->primary_node = primary_node;

    memcpy(alloc->os_cpus, cpus, (size_t)count * sizeof(int));
    qsort(alloc->os_cpus, (size_t)count, sizeof(int), int_cmp);

    /* 写入 cpu_owner，更新 free_count */
    for (int i = 0; i < count; i++) {
        int cpu = alloc->os_cpus[i];
        cpu_owner[cpu] = jobid;

        int gi = cpu_to_group[cpu];
        if (gi >= 0) {
            sys_topology.groups[gi].free_count--;
            sys_topology.nodes[sys_topology.groups[gi].node_id].free_count--;
        }
    }

    vec_push(&cpu_allocs, alloc);
    return alloc;
}

char *cpu_bind_format_cpus(const struct CpuAlloc *alloc)
{
    if (!alloc)
        return strdup("");

    size_t sz = (size_t)alloc->count * 12 + 1;
    char *buf = malloc(sz);
    if (!buf) return NULL;

    int pos = 0;
    for (int i = 0; i < alloc->count && pos < (int)sz - 1; i++) {
        int n = snprintf(buf + pos, sz - (size_t)pos,
                         "%s%d", i > 0 ? "," : "", alloc->os_cpus[i]);
        if (n < 0 || pos + n >= (int)sz - 1)
            break;
        pos += n;
    }
    buf[pos] = '\0';
    return buf;
}

char *cpu_bind_format_mems(const struct CpuAlloc *alloc)
{
    if (!alloc)
        return strdup("");

    char *buf = malloc(128);
    if (!buf) return NULL;

    if (alloc->primary_node >= 0) {
        snprintf(buf, 128, "%d", alloc->primary_node);
        return buf;
    }

    /* primary_node == -1: 位图展开 */
    int pos = 0;
    int first = 1;
    for (int b = 0; b < NUM_NODES && pos < 126; b++) {
        if (alloc->mem_nodes & (1 << b)) {
            if (!first)
                buf[pos++] = ',';
            first = 0;
            int n = snprintf(buf + pos, (size_t)(128 - pos), "%d", b);
            if (n < 0 || pos + n >= 126)
                break;
            pos += n;
        }
    }
    buf[pos] = '\0';
    return buf;
}

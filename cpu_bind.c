/* cpu_bind.c — CPU binding allocator
 *
 * 独立实现，完全按照 docs/cpu-bind-spec.md 规格编写。
 * 编译: gcc -std=c11 -Wall -Wextra -c cpu_bind.c -o cpu_bind.o
 */

#include "cpu_bind.h"
#include "vec.h"
#include "main.h"
#include "cgroups.h"
#include "server_user.h"

#ifdef TS_CPU_BIND
#include <pthread.h>
#endif

/* 拓扑实例 — 由 gen_topology.py 生成 */
struct Topology sys_topology = TOPOLOGY_INIT;

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ================================================================
 *  内部变量
 * ================================================================ */

int cpu_owner[MAX_OS_CPU];          /* 0=空闲, >0=jobid */
static int cpu_bind_disabled;
static int cpu_bind_defrag_disabled;
static int cpu_to_group[MAX_OS_CPU];  /* cpu→group 索引表 */
vec_t cpu_allocs;                    /* 活跃 alloc 列表（供 defrag） */

#ifdef TS_CPU_BIND
static pthread_t      defrag_thread;
static pthread_mutex_t defrag_mutex = PTHREAD_MUTEX_INITIALIZER;
static int             defrag_in_progress;  /* set before mutex, cleared after */

/* ---- deferred operations queue — push during defrag, drain after ---- */
enum deferred_kind {
    DEFER_PAUSE,       /* s_hold_job */
    DEFER_CONTINUE,    /* s_cont_job */
    DEFER_SUSPEND,     /* s_suspend_user */
    DEFER_RESUME,      /* s_resume_user */
    DEFER_BIND_FREE,   /* cpu_bind_free */
    DEFER_BIND_ALLOC,  /* cpu_bind_alloc + cgroups_set_cpuset */
};

struct deferred_op {
    enum deferred_kind kind;
    int socket;
    int jobid;
    struct User *user;
    struct CpuAlloc *alloc;    /* BIND_FREE */
    int num_allocated;         /* BIND_ALLOC */
    pid_t pid;                 /* BIND_ALLOC */
};

static vec_t deferred_ops;     /* struct deferred_op* */
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
/* ---- defrag work struct: pre-computed by main thread, consumed by worker ---- */
struct defrag_work {
    int n;                    /* first quality>0 alloc index */
    int count;                /* total alloc count (== vec_size(&cpu_allocs)) */
    struct Job **jobs;        /* pre-computed job pointers (size count) */
};

/* ---- thread-side defrag: freeze → rebuild → realloc → cpuset → thaw ---- */
static void cpu_bind_defrag_run(struct defrag_work *work)
{
    int n = work->n;
    int count = work->count;
    struct Job **jobs = work->jobs;
    struct CpuAlloc **allocs = (struct CpuAlloc **)cpu_allocs.data;
    int *skipped = (int *)calloc((size_t)count, sizeof(int));

    /* no quality>0 entries → early return (shouldn't happen, _start guards this) */
    if (n >= count) {
        free(skipped);
        return;
    }

    /* ---- 第一步：暂停所有 quality>0 的 job ---- */
    for (int i = n; i < count; i++) {
        struct Job *p = jobs[i];
        if (p && p->pid > 0 && (p->state == RUNNING || p->state == PAUSE))
            cgroups_freeze_job(p);
    }

    /* ---- 第二步：重建分配状态 ---- */
    memset(cpu_owner, 0, sizeof(cpu_owner));
    for (int i = 0; i < sys_topology.num_groups; i++)
        sys_topology.groups[i].free_count = sys_topology.groups[i].num_cores;

    for (int i = 0; i < sys_topology.num_nodes; i++)
        sys_topology.nodes[i].free_count = sys_topology.nodes[i].num_cores;

    /* 恢复 quality=0 的 allocs（直接写回 cpu_owner） */
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

    /* ---- 第三步：优先在原节点上重分配 ---- */
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

    /* ---- 第四步：剩余 job 跨节点合并 ---- */
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

/* ---- thread worker: holds mutex, calls run, cleans up ---- */
static void *cpu_bind_defrag_thread(void *arg)
{
    struct defrag_work *work = (struct defrag_work *)arg;

    pthread_mutex_lock(&defrag_mutex);
    cpu_bind_defrag_run(work);
    pthread_mutex_unlock(&defrag_mutex);

    defrag_in_progress = 0;
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
 *  Async defrag thread API
 * ================================================================ */

int cpu_bind_defrag_start(void)
{
    if (defrag_in_progress) return 0;
    if (!cpu_bind_defrag_enabled()) return -1;

    int count = (int)vec_size(&cpu_allocs);
    if (count <= 0) return 0;

    struct CpuAlloc **allocs = (struct CpuAlloc **)cpu_allocs.data;

    qsort(allocs, (size_t)count, sizeof(struct CpuAlloc *), alloc_defrag_cmp);

    int n = 0;
    while (n < count && allocs[n]->quality == 0)
        n++;

    if (n >= count) return 0;

    struct Job **jobs = (struct Job **)calloc((size_t)count, sizeof(struct Job *));
    if (!jobs) return -1;

    for (int i = n; i < count; i++) {
        struct Job *p = findjob(allocs[i]->jobid);
        if (p && p->pid > 0 && (p->state == RUNNING || p->state == PAUSE))
            jobs[i] = p;
    }

    struct defrag_work *work = malloc(sizeof(struct defrag_work));
    if (!work) { free(jobs); return -1; }
    work->n = n;
    work->count = count;
    work->jobs = jobs;

    defrag_in_progress = 1;
    int rc = pthread_create(&defrag_thread, NULL, cpu_bind_defrag_thread, work);
    if (rc != 0) {
        defrag_in_progress = 0;
        free(jobs);
        free(work);
        return -1;
    }
    pthread_detach(defrag_thread);
    return 0;
}

/* ---- deferred operations: push ---- */

static void defer_push(struct deferred_op *op)
{
    vec_push(&deferred_ops, op);
}

void cpu_bind_defer_pause(int s, int jobid, struct User *u)
{
    struct deferred_op *op = calloc(1, sizeof(*op));
    op->kind = DEFER_PAUSE;
    op->socket = s;
    op->jobid = jobid;
    op->user = u;
    defer_push(op);
}

void cpu_bind_defer_continue(int s, int jobid, struct User *u)
{
    struct deferred_op *op = calloc(1, sizeof(*op));
    op->kind = DEFER_CONTINUE;
    op->socket = s;
    op->jobid = jobid;
    op->user = u;
    defer_push(op);
}

void cpu_bind_defer_suspend(int s, struct User *u)
{
    struct deferred_op *op = calloc(1, sizeof(*op));
    op->kind = DEFER_SUSPEND;
    op->socket = s;
    op->jobid = 0;
    op->user = u;
    defer_push(op);
}

void cpu_bind_defer_resume(int s, struct User *u)
{
    struct deferred_op *op = calloc(1, sizeof(*op));
    op->kind = DEFER_RESUME;
    op->socket = s;
    op->jobid = 0;
    op->user = u;
    defer_push(op);
}

void cpu_bind_defer_bind_free(int jobid, struct CpuAlloc *alloc)
{
    struct deferred_op *op = calloc(1, sizeof(*op));
    op->kind = DEFER_BIND_FREE;
    op->socket = -1;
    op->jobid = jobid;
    op->alloc = alloc;
    defer_push(op);
}

void cpu_bind_defer_bind_alloc(int jobid, int num_allocated, int pid)
{
    struct deferred_op *op = calloc(1, sizeof(*op));
    op->kind = DEFER_BIND_ALLOC;
    op->socket = -1;
    op->jobid = jobid;
    op->num_allocated = num_allocated;
    op->pid = (pid_t)pid;
    defer_push(op);
}

/* ---- deferred operations: drain ---- */

static void drain_one(struct deferred_op *op)
{
    switch (op->kind) {
    case DEFER_PAUSE:
        s_hold_job(op->socket, op->jobid, op->user);
        break;
    case DEFER_CONTINUE:
        s_cont_job(op->socket, op->jobid, op->user);
        break;
    case DEFER_SUSPEND:
        s_suspend_user(op->socket, op->user);
        s_user_status(op->socket, op->user);
        break;
    case DEFER_RESUME:
        s_resume_user(op->socket, op->user);
        s_user_status(op->socket, op->user);
        break;
    case DEFER_BIND_FREE:
        if (op->alloc)
            cpu_bind_free(op->alloc);
        return;  /* no socket to close */
    case DEFER_BIND_ALLOC: {
        struct Job *p = findjob(op->jobid);
        if (p && p->pid > 0 && p->state == RUNNING && !p->cpu_alloc
            && cpu_bind_enabled() && !p->no_cpu_binding
            && op->num_allocated > 0) {
            p->cpu_alloc = cpu_bind_alloc_init(op->jobid, op->num_allocated);
            if (p->cpu_alloc) {
                cpu_bind_alloc((struct CpuAlloc *)p->cpu_alloc,
                               op->num_allocated);
                cgroups_set_cpuset(op->jobid, p->pid, p->cpu_alloc);
            }
        }
        return;  /* no socket to close */
    }
    }

    /* Category-1 ops: close socket and let server loop detect the
       disconnect (matches normal client_read path which always calls
       close(s) + remove_connection(index) after these handlers). */
    if (op->socket >= 0)
        close(op->socket);
}

static void cpu_bind_drain_deferred(void)
{
    int need_defrag = 0;

    while (vec_size(&deferred_ops) > 0) {
        struct deferred_op *op = (struct deferred_op *)vec_get(&deferred_ops, 0);
        vec_remove(&deferred_ops, 0);

        if (op->kind == DEFER_BIND_FREE || op->kind == DEFER_BIND_ALLOC)
            need_defrag = 1;

        drain_one(op);
        free(op);
    }

    /* If any CPU bind free/alloc was deferred, trigger a fresh defrag
       to re-optimise the now-updated allocation state. */
    if (need_defrag && cpu_bind_defrag_enabled())
        cpu_bind_defrag_start();
}

int cpu_bind_defrag_poll(void)
{
    if (!defrag_in_progress) {
        cpu_bind_drain_deferred();
        return 1;
    }
    return 0;
}

int cpu_bind_alloc_is_locked(void)
{
    return defrag_in_progress;
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

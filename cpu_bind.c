/* cpu_bind.c — CPU binding allocator
 *
 * 独立实现，完全按照 docs/cpu-bind-spec.md 规格编写。
 * 编译: gcc -std=c11 -Wall -Wextra -c cpu_bind.c -o cpu_bind.o
 */

#define TOPOLOGY_IMPLEMENTATION
#include "cpu_bind.h"
#include "vec.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ================================================================
 *  内部变量
 * ================================================================ */

int cpu_owner[MAX_OS_CPU];          /* 0=空闲, >0=jobid */
static int cpu_bind_disabled;
static int cpu_to_group[MAX_OS_CPU];  /* cpu→group 索引表 */
vec_t cpu_allocs;                    /* 活跃 alloc 列表（供 defrag） */

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
    /* 64 = MAX_CPU_ON_NODE，N > 64 必然跨节点 */
    if (N > 64) {
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
    if (ng > 0)
        alloc_from_groups(N, groups_collected, ng, alloc);
}

/* ================================================================
 *  公共 API
 * ================================================================ */

void cpu_bind_init(void)
{
    /* 由编译开关 TS_CPU_BIND 控制，编译即启用 */
    cpu_bind_disabled = 0;
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

    alloc->jobid     = jobid;
    alloc->slots     = max_cpus;

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

int cpu_bind_defrag(void)
{
    int count = (int)vec_size(&cpu_allocs);
    if (count <= 0)
        return 0;

    struct CpuAlloc **allocs = (struct CpuAlloc **)cpu_allocs.data;

    /* 排序：quality 小的在前，N 大的在前 */
    qsort(allocs, (size_t)count, sizeof(struct CpuAlloc *), alloc_defrag_cmp);

    /* quality=0 的都在前部 */
    int n = 0;
    while (n < count && allocs[n]->quality == 0)
        n++;

    /* 全部清空 */
    memset(cpu_owner, 0, sizeof(cpu_owner));
    for (int i = 0; i < sys_topology.num_groups; i++)
        sys_topology.groups[i].free_count =
            sys_topology.groups[i].num_cores;
    for (int i = 0; i < sys_topology.num_nodes; i++)
        sys_topology.nodes[i].free_count =
            sys_topology.nodes[i].num_cores;

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

    /* 重分 quality>0 的 allocs（手动重置，不调 malloc/vec_push） */
    for (int i = n; i < count; i++) {
        struct CpuAlloc *a = allocs[i];
        int N = a->count;
        a->count     = 0;
        a->error     = 0;
        a->quality   = 0;
        a->mem_nodes = 0;
        cpu_bind_alloc(a, N);
    }

    return 0;
}

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

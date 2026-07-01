/* test_cpu_bind.c — CPU 绑定分配器完整测试套件
 *
 * 编译: gcc -std=c11 -Wall -Wextra -o test_cpu_bind test_cpu_bind.c cpu_bind.o
 * 运行: ./test_cpu_bind
 *
 * 拓扑数据硬编码在 topology.h 中：
 *   2 NUMA 节点 x 4 groups x 4 核 = 32 核
 *   Node0: 0-15, Node1: 16-31
 */

#include "cpu_bind.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ---- 测试辅助宏 ---- */
static int tests_run  = 0;
static int tests_pass = 0;

#define TEST(name)  do { \
    printf("  %-36s ", name); \
    fflush(stdout); \
    tests_run++; \
} while (0)

#define PASS()      do { \
    printf("\033[32mPASS\033[0m\n"); \
    tests_pass++; \
} while (0)

#define FAIL(msg)   do { \
    printf("\033[31mFAIL\033[0m  %s:%d: %s\n", __FILE__, __LINE__, msg); \
    return; \
} while (0)

#define ASSERT(cond, msg) do { \
    if (!(cond)) { FAIL(msg); } \
} while (0)

/* 分配辅助：init + alloc */
static struct CpuAlloc *alloc_cpu(int N, int jobid)
{
    struct CpuAlloc *a = cpu_bind_alloc_init(jobid, N);
    cpu_bind_alloc(a, N);
    return a;
}

/* ================================================================
 *  测试用例 — 拓扑已在 topology.h 中硬编码
 * ================================================================ */

static void test_init(void)
{
    TEST("init — enables & zeros everything");
    cpu_bind_init();
    ASSERT(cpu_bind_enabled() == 1, "should be enabled");
    for (int i = 0; i < 32; i++)
        ASSERT(cpu_owner[i] == 0, "all cpus free");
    ASSERT(sys_topology.nodes[0].free_count == 16, "node0 free=16");
    ASSERT(sys_topology.nodes[1].free_count == 16, "node1 free=16");
    for (int i = 0; i < 8; i++)
        ASSERT(sys_topology.groups[i].free_count == 4,
               "each group free=4");
    PASS();
}

static void test_init_disabled(void)
{
    TEST("init — TS_NO_CPU_BIND disables");
    setenv("TS_NO_CPU_BIND", "1", 1);
    cpu_bind_init();
    ASSERT(cpu_bind_enabled() == 0, "should be disabled");
    unsetenv("TS_NO_CPU_BIND");
    PASS();
}

static void test_alloc_N0_returns_null(void)
{
    TEST("alloc — N=0 returns error");
    cpu_bind_init();
    struct CpuAlloc *a = alloc_cpu(0, 1001);
    ASSERT(a->error != 0, "error set for N=0");
    cpu_bind_free(a);
    PASS();
}

static void test_alloc_over_capacity(void)
{
    TEST("alloc — N > total free returns error");
    cpu_bind_init();
    struct CpuAlloc *a = alloc_cpu(33, 1002);
    ASSERT(a->error != 0, "error set for N=33 > 32");
    cpu_bind_free(a);
    PASS();
}

static void test_alloc_single_cpu(void)
{
    TEST("alloc — 1 CPU from node0");
    cpu_bind_init();
    struct CpuAlloc *a = alloc_cpu(1, 2001);
    ASSERT(a->error == 0, "alloc succeeded");
    ASSERT(a->count == 1, "got 1 cpu");
    ASSERT(a->primary_node == 0, "primary node 0");
    ASSERT(cpu_owner[a->os_cpus[0]] == 2001, "owner=2001");
    cpu_bind_free(a);
    PASS();
}

static void test_alloc_whole_group(void)
{
    TEST("alloc — full group (4 CPUs)");
    cpu_bind_init();
    struct CpuAlloc *a = alloc_cpu(4, 2002);
    ASSERT(a->error == 0, "alloc succeeded");
    ASSERT(a->count == 4, "got 4 cpus");
    ASSERT(a->primary_node == 0, "primary node 0");
    int cpu0_group = a->os_cpus[0] / 4;
    for (int i = 0; i < 4; i++) {
        ASSERT(cpu_owner[a->os_cpus[i]] == 2002, "owner=2002");
        ASSERT(a->os_cpus[i] / 4 == cpu0_group, "all from same group");
    }
    cpu_bind_free(a);
    PASS();
}

static void test_alloc_cross_group_same_node(void)
{
    TEST("alloc — 6 CPUs (cross-group, same node)");
    cpu_bind_init();
    struct CpuAlloc *a = alloc_cpu(6, 2003);
    ASSERT(a->error == 0, "alloc succeeded");
    ASSERT(a->count == 6, "got 6 cpus");
    ASSERT(a->primary_node == 0, "primary node 0");
    for (int i = 0; i < 6; i++) {
        ASSERT(cpu_owner[a->os_cpus[i]] == 2003, "owner=2003");
        ASSERT(a->os_cpus[i] < 16, "all from node0");
    }
    cpu_bind_free(a);
    PASS();
}

static void test_alloc_cross_node(void)
{
    TEST("alloc — 24 CPUs (forced cross-node)");
    cpu_bind_init();
    struct CpuAlloc *a = alloc_cpu(24, 2004);
    ASSERT(a->error == 0, "alloc succeeded");
    ASSERT(a->count == 24, "got 24 cpus");
    ASSERT(a->primary_node >= 0, "primary node set");
    int node_count = 0;
    for (int b = 0; b < NUM_NODES; b++) {
        if (a->mem_nodes & (1 << b))
            node_count++;
    }
    ASSERT(node_count > 1, "spans multiple nodes");
    for (int i = 0; i < 24; i++)
        ASSERT(cpu_owner[a->os_cpus[i]] == 2004, "owner=2004");
    cpu_bind_free(a);
    PASS();
}

static void test_alloc_exhaust_and_free(void)
{
    TEST("alloc — exhaust all CPUs then free+re-alloc");
    cpu_bind_init();

    struct CpuAlloc *a1 = alloc_cpu(16, 3001);
    struct CpuAlloc *a2 = alloc_cpu(16, 3002);
    ASSERT(a1->error == 0 && a2->error == 0, "both allocs succeeded");

    struct CpuAlloc *d = alloc_cpu(1, 3003);
    ASSERT(d->error != 0, "no more free cpus");
    cpu_bind_free(d);

    cpu_bind_free(a1);
    struct CpuAlloc *a3 = alloc_cpu(16, 3004);
    ASSERT(a3->error == 0, "re-alloc after free");
    ASSERT(a3->count == 16, "got 16 cpus");

    cpu_bind_free(a2);
    cpu_bind_free(a3);
    PASS();
}

static void test_alloc_free_twice(void)
{
    TEST("alloc-free — double idle check");
    cpu_bind_init();

    struct CpuAlloc *a = alloc_cpu(4, 5001);
    ASSERT(a->error == 0, "alloc");
    int cpu = a->os_cpus[0];
    cpu_bind_free(a);
    ASSERT(cpu_owner[cpu] == 0, "freed");

    struct CpuAlloc *b = alloc_cpu(4, 5002);
    ASSERT(b->error == 0, "re-alloc");
    cpu_bind_free(b);
    PASS();
}

static void test_format_cpus(void)
{
    TEST("format_cpus — range merging");
    cpu_bind_init();

    struct CpuAlloc *a = alloc_cpu(8, 6001);
    ASSERT(a->error == 0, "alloc");

    char *s = cpu_bind_format_cpus(a);
    ASSERT(s && strlen(s) > 0, "formatted");
    printf(" [cpus=%s] ", s);
    free(s);

    cpu_bind_free(a);
    PASS();
}

static void test_format_mems(void)
{
    TEST("format_mems — single node vs bitmap");
    cpu_bind_init();

    struct CpuAlloc *a = alloc_cpu(4, 7001);
    ASSERT(a->error == 0, "alloc");
    char *s = cpu_bind_format_mems(a);
    ASSERT(s && strcmp(s, "0") == 0, "primary node 0");
    free(s);
    cpu_bind_free(a);

    struct CpuAlloc *b = alloc_cpu(24, 7002);
    if (b->error == 0 && b->primary_node == -1) {
        char *s2 = cpu_bind_format_mems(b);
        ASSERT(s2 && strlen(s2) > 1, "bitmap like '0,1'");
        free(s2);
    }
    cpu_bind_free(b);
    PASS();
}

static void test_alloc_defrag(void)
{
    TEST("defrag — empty vec returns 0");
    ASSERT(cpu_bind_defrag(NULL, NULL, NULL) == 0, "no allocs in vec");
    PASS();
}

static void test_alloc_large_jobid(void)
{
    TEST("alloc — large jobid");
    cpu_bind_init();
    struct CpuAlloc *a = alloc_cpu(2, 999999);
    ASSERT(a->error == 0, "alloc");
    ASSERT(cpu_owner[a->os_cpus[0]] == 999999, "owner=999999");
    cpu_bind_free(a);
    PASS();
}

static void test_alloc_multiple_concurrent(void)
{
    TEST("alloc — 4 concurrent jobs");
    cpu_bind_init();

    struct CpuAlloc *j1 = alloc_cpu(4, 101);
    struct CpuAlloc *j2 = alloc_cpu(4, 102);
    struct CpuAlloc *j3 = alloc_cpu(4, 103);
    struct CpuAlloc *j4 = alloc_cpu(4, 104);

    ASSERT(j1->error == 0 && j2->error == 0 &&
           j3->error == 0 && j4->error == 0, "all allocs");
    ASSERT(j1->count == 4 && j2->count == 4 &&
           j3->count == 4 && j4->count == 4, "each got 4");

    int seen[32] = {0};
    struct CpuAlloc *all[] = {j1, j2, j3, j4};
    for (int j = 0; j < 4; j++) {
        for (int i = 0; i < 4; i++) {
            int cpu = all[j]->os_cpus[i];
            ASSERT(seen[cpu] == 0, "no overlap");
            seen[cpu] = 1;
        }
    }

    for (int j = 0; j < 4; j++)
        cpu_bind_free(all[j]);
    PASS();
}

static void test_free_null(void)
{
    TEST("free — NULL is safe");
    cpu_bind_free(NULL);
    PASS();
}

/* ================================================================
 *  主入口
 * ================================================================ */

int main(void)
{
    printf("═══ CPU Bind Allocator Test Suite ═══\n");
    printf("     Topology: 2 nodes x 4 groups x 4 cores = 32 cores\n\n");

    test_init();
    test_init_disabled();
    test_alloc_N0_returns_null();
    test_alloc_over_capacity();
    test_alloc_single_cpu();
    test_alloc_whole_group();
    test_alloc_cross_group_same_node();
    test_alloc_cross_node();
    test_alloc_exhaust_and_free();
    test_alloc_free_twice();
    test_free_null();
    test_format_cpus();
    test_format_mems();
    test_alloc_defrag();
    test_alloc_large_jobid();
    test_alloc_multiple_concurrent();

    printf("\n═══ %d / %d tests passed ═══\n",
           tests_pass, tests_run);

    return (tests_pass == tests_run) ? 0 : 1;
}

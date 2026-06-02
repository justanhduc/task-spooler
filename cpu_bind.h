/* cpu_bind.h — CPU binding allocator public API */

#ifndef CPU_BIND_H
#define CPU_BIND_H

#include "topology.h"
#include "vec.h"

/* ---- CPU topology types (merged from topology_types.h) ---- */
struct CoreGroup {
    int group_id;
    int node_id;
    int num_cores;
    int free_count;
    int os_cpus[MAX_CORES_PER_GROUP];
};

struct NodeInfo {
    int node_id;
    int num_groups;
    int num_cores;
    int free_count;
    int group_ids[16];
};

struct Topology {
    int num_nodes;
    int num_groups;
    struct CoreGroup groups[NUM_GROUPS];
    struct NodeInfo nodes[NUM_NODES];
};

/* ---- 全局 CPU→jobid 映射 ---- */
/* 0 = 空闲, >0 = 被该 jobid 占用 */
extern int cpu_owner[MAX_OS_CPU];

/* ---- 分配结果 ---- */
struct CpuAlloc {
    int primary_node;           /* -1=跨节点不限制, ≥0=主节点 */
    int mem_nodes;              /* 涉及节点位图 */
    int count;                  /* os_cpus 长度 */
    int slots;                  /* 分配的容量（os_cpus[] 最大元素数） */
    int jobid;
    int error;                  /* 0=成功, 1=分配异常（部分失败） */
    int quality;                /* 0=完美 1=1个碎片group 2=2个碎片group */
    int os_cpus[];              /* 柔性数组 */
};

extern struct Topology sys_topology;
extern vec_t      cpu_allocs;

void              cpu_bind_init(void);
int               cpu_bind_enabled(void);
void              cpu_bind_set_disabled(int disabled);

struct CpuAlloc *cpu_bind_alloc_init(int jobid, int max_cpus);
void              cpu_bind_alloc(struct CpuAlloc *alloc, int N);
void              cpu_bind_free(struct CpuAlloc *alloc);

/* defrag 回调类型 — 传 NULL 表示跳过对应步骤 */
typedef void (*cpu_bind_pause_fn)(int jobid);
typedef void (*cpu_bind_update_fn)(int jobid, const struct CpuAlloc *alloc);
typedef void (*cpu_bind_resume_fn)(int jobid);

/* 碎片整理：先暂停 quality>0 的 job，重分配后更新 cpuset，再恢复 */
int               cpu_bind_defrag(cpu_bind_pause_fn pause,
                                 cpu_bind_update_fn update_cpuset,
                                 cpu_bind_resume_fn resume);

char             *cpu_bind_format_cpus(const struct CpuAlloc *alloc);
char             *cpu_bind_format_mems(const struct CpuAlloc *alloc);

/* 重启恢复 — 从 cpuset 字符串认领已分配的 CPU */
int               cpu_bind_parse_cpuset(const char *str, int *cpus, int max_cpus);
struct CpuAlloc  *cpu_bind_claim(int jobid, const int *cpus, int count,
                                 int mem_nodes, int primary_node);

#endif /* CPU_BIND_H */

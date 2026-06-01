/* cpu_bind.h — CPU binding allocator public API */

#ifndef CPU_BIND_H
#define CPU_BIND_H

#include "topology.h"
#include "vec.h"

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

struct CpuAlloc *cpu_bind_alloc_init(int jobid, int max_cpus);
void              cpu_bind_alloc(struct CpuAlloc *alloc, int N);
void              cpu_bind_free(struct CpuAlloc *alloc);

int               cpu_bind_defrag(void);

char             *cpu_bind_format_cpus(const struct CpuAlloc *alloc);
char             *cpu_bind_format_mems(const struct CpuAlloc *alloc);

/* 重启恢复 — 从 cpuset 字符串认领已分配的 CPU */
int               cpu_bind_parse_cpuset(const char *str, int *cpus, int max_cpus);
struct CpuAlloc  *cpu_bind_claim(int jobid, const int *cpus, int count,
                                 int mem_nodes, int primary_node);

#endif /* CPU_BIND_H */

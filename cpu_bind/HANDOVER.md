# CPU 绑定分配器 — 交接文档

## 文件结构

```
cpu_bind/
├── topology.h          # 硬编码系统拓扑（2节点 x 4组 x 4核 = 32核）
├── cpu_bind.h          # 公共 API 声明
├── cpu_bind.c          # 完整实现
├── test_cpu_bind.c     # 16 个测试用例
└── Makefile            # 独立编译（依赖 ../vec.h ../vec.o）
```

## 公共 API

```c
void              cpu_bind_init(void);                      // 初始化
int               cpu_bind_enabled(void);                   // 是否启用
struct CpuAlloc  *cpu_bind_alloc_init(int jobid, int max_cpus); // 创建空 alloc
void              cpu_bind_alloc(struct CpuAlloc *alloc, int N); // 分配 N 个 CPU
void              cpu_bind_free(struct CpuAlloc *alloc);    // 释放
int               cpu_bind_defrag(void);                    // 碎片整理
char             *cpu_bind_format_cpus(const struct CpuAlloc *alloc); // "0,1,2,3"
char             *cpu_bind_format_mems(const struct CpuAlloc *alloc); // "0" 或 "0,1"
```

## struct CpuAlloc

```c
struct CpuAlloc {
    int primary_node;           // 主节点（-1=不限制）
    int mem_nodes;              // 涉及节点位图
    int count;                  // 已分配的 CPU 数
    int slots;                  // 容量（os_cpus[] 最大元素数）
    int jobid;
    int error;                  // 0=成功 1=异常
    int quality;                // 碎片 group 数（0=完美 1=1个碎片 2=2个碎片）
    int os_cpus[];              // 柔性数组
};
```

## 全局变量

- `cpu_owner[MAX_OS_CPU]` — CPU→jobid 映射（0=空闲）
- `cpu_to_group[MAX_OS_CPU]` — CPU→group 索引表（`cpu_bind_init` 中构建）
- `cpu_bind_disabled` — `TS_NO_CPU_BIND` 环境变量禁用
- `cpu_allocs` — `vec_t` 全局活跃 alloc 列表（供 defrag 使用）

## 内部函数调用链

```
cpu_bind_alloc()
  ├─ single_ok? → alloc_from_groups(N, node->group_ids, ...)
  └─ 否则 → cross_node_merge()
              └─ alloc_from_groups(N, collected_ids, ...)

alloc_from_groups(N, group_ids, ...)
  while remaining > 0:
    ① best-fit 单组（free_count ≥ remaining，excess 最小）
    ② 无 → 最大空闲组
    └─ take_from_group(group, take, alloc)

take_from_group()
  → 在 group→os_cpus[] 中取 N 个空闲 CPU（不要求连续）
  → 写 cpu_owner，更新 free_count
  → free_count > 0 则 alloc->quality++
  → 返回实际取的个数

cpu_bind_post_alloc()
  → qsort(os_cpus)
  → 校验①：每个 CPU owner == jobid
  → 校验②：owner 总数 == count
  → 可 #define CPU_BIND_NO_POST_CHECK 关闭
```

## defrag 流程

```
cpu_bind_defrag()
  ① 按 (quality ASC, slots DESC) 排序 cpu_allocs
  ② 找 quality=0 的边界
  ③ 清空 cpu_owner，重置 free_count（用 num_cores 直接赋值）
  ④ 恢复 quality=0 allocs 的 cpu_owner
  ⑤ 重分 quality>0 allocs（手动重置字段，re-alloc）
```

## 测试

```bash
cd cpu_bind && make test
# 16/16 passed, 零警告
```

`test_cpu_bind.c` — 16 个测试覆盖：初始化、禁用、空/超容量分配、单核→整组→跨组→跨节点、耗尽+重分配、双释放安全、格式化、并发不重叠、defrag。

## 待办 / 后续

- [ ] defrag 的质量排序和重分逻辑需要实际验证（目前只有空 vec 测试）
- [ ] `cpu_bind_free` 中的 vec 扫描删除是 O(n)，数据量大时可优化
- [ ] `cpu_bind_post_alloc` 的校验目前硬编码，可通过编译开关关闭
- [ ] 与 ts 主项目的集成（`cpu_bind.c` 需要接入 server 的 job 生命周期）

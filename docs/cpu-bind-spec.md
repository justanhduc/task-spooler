# CPU 绑定分配器规格

> 独立 `.c` 文件，review 通过后合并到 ts。

---

## 一、输入：topology.h（外部生成）

```c
// topology.h — gen_topology.py 生成的硬编码

#define NUM_NODES            2
#define NUM_GROUPS           32
#define MAX_CORES_PER_GROUP  16
#define MAX_OS_CPU           256

struct CoreGroup {
    int group_id;
    int node_id;
    int num_cores;                     // 总核心数（静态）
    int free_count;                    // 空闲核心数（运行时，初始=num_cores）
    int os_cpus[MAX_CORES_PER_GROUP];
};

struct NodeInfo {
    int node_id;
    int num_groups;
    int free_count;                    // 运行时，=该节点所有组的 free_count 之和
    int group_ids[16];
};

extern struct Topology {
    int num_nodes;
    int num_groups;
    struct CoreGroup groups[NUM_GROUPS];
    struct NodeInfo nodes[NUM_NODES];
} sys_topology;
```

---

## 二、运行时结构（cpu_bind.h）

```c
/* ---- 全局 CPU→jobid 映射 ---- */
// 0 = 空闲, >0 = 被该 jobid 占用
extern int cpu_owner[MAX_OS_CPU];

/* ---- 分配结果 ---- */
struct CpuAlloc {
    int primary_node;           // -1=跨节点不限制, ≥0=主节点
    int mem_nodes;              // 涉及节点位图
    int count;                  // os_cpus 长度
    int jobid;
    int os_cpus[];              // 柔性数组
};
```

---

## 三、公共 API

```c
void cpu_bind_init(void);
int  cpu_bind_enabled(void);

struct CpuAlloc *cpu_bind_alloc(int N, int jobid);
void             cpu_bind_free(struct CpuAlloc *alloc);

int  cpu_bind_who_owns(int os_cpu);
int  cpu_bind_defrag(void);

void cpu_bind_format_cpus(const struct CpuAlloc *alloc, char *buf, int size);
void cpu_bind_format_mems(const struct CpuAlloc *alloc, char *buf, int size);
```

---

## 四、内部变量

```c
int cpu_owner[MAX_OS_CPU];      // 0=空闲, >0=jobid
int cpu_bind_disabled;
```

- `cpu_owner` 是唯一真相源
- 组/节点的 `free_count` 存在 `sys_topology.groups[]` 和 `sys_topology.nodes[]` 里

---

## 五、核心算法

### 5.1 `cpu_bind_init(void)`

```
if getenv("TS_NO_CPU_BIND"): cpu_bind_disabled = 1; return

for i in 0..MAX_OS_CPU-1: cpu_owner[i] = 0

for i in 0..NUM_GROUPS-1:
  sys_topology.groups[i].free_count = sys_topology.groups[i].num_cores

for i in 0..NUM_NODES-1:
  该节点 free_count = sum(所属各组 free_count)
```

### 5.2 `cpu_bind_alloc(N, jobid) → struct CpuAlloc *`

```
if cpu_bind_disabled || N <= 0: return NULL

total_free = sum(nodes[].free_count)
if total_free < N: return NULL

alloc = calloc(1, sizeof(struct CpuAlloc) + N * sizeof(int))
alloc->jobid = jobid

有节点 free_count ≥ N → node_selection(N, alloc)
否则                → cross_node_merge(N, alloc)
```

### 5.3 `node_selection(N, alloc)` — 单节点能装下

```
候选: 所有节点中 free_count ≥ N 的
选中: free_count 最大的（并列随机）

groups = 选中节点的所有组（遍历 group_ids，按 group_id 升序）
alloc_from_vnode(N, groups, alloc)

alloc->primary_node = 选中节点
alloc->mem_nodes    = 1 << primary_node
```

### 5.4 `cross_node_merge(N, alloc)` — 跨节点

```
remaining = N
groups_collected[] = 空
bitmap = 0

while remaining > 0:
  在未选节点中 best-fit:
    找 free_count ≥ remaining 且 excess 最小的
    找不到 → free_count 最大的
    并列 → 随机
  记录该节点贡献 = min(node.free_count, remaining)
  groups_collected += 该节点所有组
  bitmap |= (1 << node_id)
  remaining -= 贡献

alloc_from_vnode(N, groups_collected, alloc)
alloc->mem_nodes = bitmap

if N > 64:
  alloc->primary_node = -1
else:
  alloc->primary_node = bitmap 中本 job 占核心最多的 node_id（并列随机）
```

### 5.5 `alloc_from_vnode(N, groups[], alloc)`

```
groups 按 group_id 升序

1. best-fit 单组:
   for g in groups:
     if g->free_count >= N:
       excess = g->free_count - N, 记录最小
   if 找到: take_from_group(g, N, alloc); return

2. 最大空闲组:
   g = groups 中 free_count 最大的（并列随机）
   if g->free_count >= N: take_from_group(g, N, alloc); return

3. 多组凑:
   for g in groups:
     take = min(g->free_count, remaining)
     if take > 0:
       take_from_group(g, take, alloc)
       if (remaining -= take) == 0: break
```

### 5.6 `take_from_group(group, N, alloc)`

```
// 在 group->os_cpus[0..num_cores-1] 中找连续空闲段
// 空闲判据: cpu_owner[cpu] == 0
// 找长度 ≥ N 的最短连续空闲段

对段内每个 cpu:
  alloc->os_cpus[alloc->count++] = cpu
  cpu_owner[cpu] = alloc->jobid
  group->free_count--

// 同步更新所属节点的 free_count
group 所属 node->free_count -= N
```

---

## 六、辅助函数

### 6.1 `cpu_bind_free(alloc)`

```
for i in 0..alloc->count-1:
  cpu = alloc->os_cpus[i]
  cpu_owner[cpu] = 0

  找到 cpu 属于哪个 group（遍历 sys_topology.groups 的 os_cpus）
  该 group->free_count++
  该 group 所属 node->free_count++

free(alloc)
```

### 6.2 `cpu_bind_who_owns(os_cpu)`

```
if os_cpu < 0 || os_cpu >= MAX_OS_CPU: return -1
return cpu_owner[os_cpu]
```

### 6.3 `cpu_bind_format_cpus(alloc, buf, size)`

```
排序 alloc->os_cpus → 连续段合并 → "0-3,6-7"
```

### 6.4 `cpu_bind_format_mems(alloc, buf, size)`

```
primary_node == -1 → mem_nodes 位图展开 → "0,1"
primary_node ≥ 0   → sprintf(buf, "%d", primary_node)
```

### 6.5 `cpu_bind_defrag()`

```
// 占位
return 0;
```

---

## 七、编译

```bash
gcc -std=c11 -Wall -Wextra -c cpu_bind.c -o cpu_bind.o
```

---

## 八、要点

- 只有 `cpu_owner[]` 一个运行时数组和 `sys_topology` 里的 `free_count`，没有其他结构体
- `cpu_owner[cpu] == 0` 即空闲
- 组和节点的 `free_count` 在每次分配/释放时同步更新

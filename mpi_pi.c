/*
 * mpi_pi.c — MPI + CPU 绑定测试程序
 *
 * 用数值积分计算 π，定期从 rank 0 输出 FLOPS。
 * 配合 ts + cpu_bind 测试绑核效果。
 *
 * 编译:
 *   mpicc -O2 -o mpi_pi mpi_pi.c
 *
 * 运行:
 *   mpirun -np 4 ./mpi_pi 10        # 4进程跑10秒
 *   ts -N 4 mpirun -np 4 ./mpi_pi 10   # 绑4核跑
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <mpi.h>
#include <sched.h>

/* ---- 获取绑定的 CPU 个数 ---- */
static int get_cpu_count(void)
{
    cpu_set_t mask;
    CPU_ZERO(&mask);
    if (sched_getaffinity(0, sizeof(mask), &mask) == 0) {
        int count = 0;
        for (int i = 0; i < CPU_SETSIZE; i++)
            if (CPU_ISSET(i, &mask)) count++;
        return count;
    }
    return 1;
}

/* ---- 获取绑定的 CPU 列表字符串（仅 rank 0 使用） ---- */
static const char *get_cpu_list(void)
{
    static char buf[256];
    cpu_set_t mask;
    CPU_ZERO(&mask);
    if (sched_getaffinity(0, sizeof(mask), &mask) != 0) {
        snprintf(buf, sizeof(buf), "(unknown)");
        return buf;
    }
    int pos = 0, first = 1;
    for (int i = 0; i < CPU_SETSIZE; i++) {
        if (CPU_ISSET(i, &mask)) {
            int n = snprintf(buf + pos, sizeof(buf) - (size_t)pos,
                             "%s%d", first ? "" : ",", i);
            if (n > 0) pos += n;
            first = 0;
        }
    }
    if (first) snprintf(buf, sizeof(buf), "(none)");
    return buf;
}

int main(int argc, char **argv)
{
    int rank, nprocs;

    MPI_Init(&argc, &argv);
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &nprocs);

    if (argc < 2) {
        if (rank == 0)
            fprintf(stderr, "用法: mpirun -np N %s <运行秒数>\n", argv[0]);
        MPI_Finalize();
        return 1;
    }

    int runtime_sec = atoi(argv[1]);
    if (runtime_sec <= 0) runtime_sec = 10;

    /* rank 0 显示启动信息 */
    if (rank == 0) {
        printf("========================================\n");
        printf(" 命令:");
        for (int i = 0; i < argc; i++)
            printf(" %s", argv[i]);
        printf("\n");
        printf(" 进程: %d\n", nprocs);
        printf(" 运行: %d 秒\n", runtime_sec);
        printf(" CPU:  %s (%d 核)\n", get_cpu_list(), get_cpu_count());
        printf("========================================\n");
        fflush(stdout);
    }

    /* π = ∫₀¹ 4/(1+x²) dx   — 每个进程算自己的分片 */
    double h = 1.0 / (double)nprocs;      /* 每个进程的区间宽度 */
    int steps_per_sec = 20000000;         /* 每秒每进程的步数 */

    double global_pi = 0.0;
    double flop_count = 0.0;             /* 每进程浮点运算计数 */
    double prev_flops = 0.0;             /* 上一秒的 flop_count 快照 */
    int    step = 0;
    double start_wall = MPI_Wtime();

    while (1) {
        /* 当前区间: [rank*h, (rank+1)*h) */
        double local = 0.0;
        double x0 = (double)rank * h;

        for (int i = 0; i < steps_per_sec; i++) {
            double x = x0 + h * ((double)i + 0.5) / (double)steps_per_sec;
            local += 4.0 / (1.0 + x * x);
            flop_count += 4.0;            /* +, *, /, + 约4次浮点 */
        }
        local *= h / (double)steps_per_sec;

        step++;
        double elapsed = MPI_Wtime() - start_wall;

        /* 规约 π 和 flop 总数 */
        double total_pi, total_flops;
        MPI_Allreduce(&local, &total_pi, 1, MPI_DOUBLE, MPI_SUM, MPI_COMM_WORLD);
        MPI_Allreduce(&flop_count, &total_flops, 1, MPI_DOUBLE, MPI_SUM, MPI_COMM_WORLD);
        global_pi += total_pi;

        if (rank == 0) {
            double delta = total_flops - prev_flops;
            prev_flops = total_flops;
            printf(" [%d/%ds] π=%.10f  %.2e FLOPS  (累计 %.2e)\n",
                   step, runtime_sec, global_pi, delta, total_flops);
            fflush(stdout);
        }

        if (step >= runtime_sec) break;
    }

    double elapsed = MPI_Wtime() - start_wall;
    double total_flops_sum;
    MPI_Reduce(&flop_count, &total_flops_sum, 1, MPI_DOUBLE, MPI_SUM, 0, MPI_COMM_WORLD);

    if (rank == 0) {
        printf("========================================\n");
        printf(" π ≈ %.10f  (误差 %.2e)\n", global_pi, global_pi - 3.14159265358979323846);
        printf(" 耗时: %.2f 秒\n", elapsed);
        printf(" 总计: %.2e 次浮点运算\n", total_flops_sum);
        printf(" 均值: %.2e FLOPS\n", total_flops_sum / elapsed);
        printf("========================================\n");
    }

    MPI_Finalize();
    return 0;
}

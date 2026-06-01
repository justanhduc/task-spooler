/*
 * mpi_bench.c — MPI + CPU 绑定测试程序
 *
 * 每个进程做矩阵乘法（GEMM 风格）负载，
 * 每秒 rank 0 显示绑核范围 + 所有进程当前运行的 CPU。
 *
 * 配合 ts + cpu_bind 测试绑核效果。
 *
 * 编译:
 *   mpicc -O2 -o mpi_bench mpi_bench.c
 *
 * 运行:
 *   mpirun -np 4 ./mpi_bench 10        # 4进程跑10秒
 *   ts -N 4 mpirun -np 4 ./mpi_bench 10   # 绑4核
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <mpi.h>
#include <pthread.h>
#include <sched.h>

/* ---- 矩阵大小 ---- */
#define MAT_SIZE  512

/* ---- 矩阵乘法 C = A * B (方阵, 全尺寸) ---- */
static void mat_mul(double *C, const double *A, const double *B, int n)
{
    for (int i = 0; i < n; i++) {
        for (int k = 0; k < n; k++) {
            double aik = A[i * n + k];
            for (int j = 0; j < n; j++)
                C[i * n + j] += aik * B[k * n + j];
        }
    }
}

/* ---- int 比较器给 qsort 用 ---- */
static int int_cmp(const void *a, const void *b)
{
    return *(const int *)a - *(const int *)b;
}

/* ---- 获取绑核范围字符串 (pthread_getaffinity_np) ---- */
static const char *get_affinity_range(void)
{
    static char buf[256];
    cpu_set_t mask;
    CPU_ZERO(&mask);

    if (pthread_getaffinity_np(pthread_self(), sizeof(mask), &mask) != 0) {
        snprintf(buf, sizeof(buf), "(unknown)");
        return buf;
    }

    /* 收集所有允许的 CPU */
    int cpus[CPU_SETSIZE], n = 0;
    for (int i = 0; i < CPU_SETSIZE; i++)
        if (CPU_ISSET(i, &mask)) cpus[n++] = i;

    if (n == 0) { snprintf(buf, sizeof(buf), "(none)"); return buf; }

    /* 合并连续段为范围, e.g. "0-3,6,8-11" */
    int pos = 0, first = 1, start = cpus[0], end = cpus[0];
    for (int i = 1; i <= n; i++) {
        if (i < n && cpus[i] == end + 1) {
            end = cpus[i];
        } else {
            int written;
            if (start == end)
                written = snprintf(buf + pos, sizeof(buf) - (size_t)pos,
                                   "%s%d", first ? "" : ",", start);
            else
                written = snprintf(buf + pos, sizeof(buf) - (size_t)pos,
                                   "%s%d-%d", first ? "" : ",", start, end);
            if (written > 0) pos += written;
            first = 0;
            if (i < n) { start = end = cpus[i]; }
        }
    }
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

    /* ---- rank 0 显示启动信息 ---- */
    if (rank == 0) {
        printf("========================================\n");
        printf(" 命令:");
        for (int i = 0; i < argc; i++) printf(" %s", argv[i]);
        printf("\n");
        printf(" MPI:  %d 进程\n", nprocs);
        printf(" 运行: %d 秒\n", runtime_sec);
        printf(" 绑核: %s\n", get_affinity_range());
        printf("========================================\n");
        fflush(stdout);
    }

    /* ---- 每个进程分配自己的矩阵 ---- */
    int n = MAT_SIZE;
    double *A = malloc((size_t)n * n * sizeof(double));
    double *B = malloc((size_t)n * n * sizeof(double));
    double *C = calloc((size_t)n * n, sizeof(double));
    if (!A || !B || !C) {
        fprintf(stderr, "[%d] malloc failed\n", rank);
        MPI_Abort(MPI_COMM_WORLD, 1);
    }

    /* 初始化 A, B */
    for (int i = 0; i < n * n; i++) {
        A[i] = 1.0 / (double)(i + 1);
        B[i] = (double)(i % 100) + 0.5;
    }

    /* ---- 主循环 ---- */
    const double FLOPS_PER_MUL = (double)n * n * (double)n * 2.0; /* 2n³ */
    double flop_count = 0.0, prev_flops = 0.0;
    double start_wall = MPI_Wtime();
    double end_wall = start_wall + (double)runtime_sec + 1.0;
    double next_report = start_wall + 1.0;
    int step = 0;

    long long iter_count = 0;
    double checksum = 0.0;

    while (MPI_Wtime() < end_wall) {
        /* 矩阵乘法 */
        memset(C, 0, (size_t)n * n * sizeof(double));
        mat_mul(C, A, B, n);
        flop_count += FLOPS_PER_MUL;
        iter_count++;

        /* 结果校验：累计 C 矩阵元素和 */
        double row_sum = 0.0;
        for (int i = 0; i < n; i++)
            row_sum += C[i * n + i];   /* 对角线求和 */
        checksum += row_sum;

        /* 每秒报告 */
        double now = MPI_Wtime();
        if (now >= next_report) {
            step++;
            if (step <= runtime_sec) {
                /* 当前运行 CPU */
                int my_cpu = sched_getcpu();
                int *all_cpus = NULL;
                if (rank == 0)
                    all_cpus = malloc((size_t)nprocs * sizeof(int));

                MPI_Gather(&my_cpu, 1, MPI_INT,
                           all_cpus, 1, MPI_INT, 0, MPI_COMM_WORLD);

                if (rank == 0) {
                    qsort(all_cpus, (size_t)nprocs, sizeof(int), int_cmp);

                    double total_flops = (double)nprocs * flop_count;
                    double delta = total_flops - prev_flops;
                    prev_flops = total_flops;

                    /* 去重显示 CPU 列表 */
                    char cpu_buf[256] = "";
                    int pos = 0, first = 1;
                    for (int i = 0; i < nprocs; i++) {
                        if (i > 0 && all_cpus[i] == all_cpus[i - 1]) continue;
                        int written = snprintf(cpu_buf + pos,
                                    sizeof(cpu_buf) - (size_t)pos,
                                    "%s%d", first ? "" : ",", all_cpus[i]);
                        if (written > 0) pos += written;
                        first = 0;
                    }

                    printf(" [%d/%ds] CPU[%s]  %.2e FLOPS  (%lld iter)\n",
                           step, runtime_sec, cpu_buf, delta,
                           (long long)nprocs * iter_count);
                    fflush(stdout);
                    free(all_cpus);
                }
            }
            next_report = now + 1.0;
        }
    }

    /* ---- 最终汇总 ---- */
    double total_flops_sum, total_checksum;
    MPI_Reduce(&flop_count, &total_flops_sum, 1, MPI_DOUBLE, MPI_SUM, 0,
               MPI_COMM_WORLD);
    MPI_Reduce(&checksum, &total_checksum, 1, MPI_DOUBLE, MPI_SUM, 0,
               MPI_COMM_WORLD);

    if (rank == 0) {
        printf("========================================\n");
        printf(" 每进程: %lld 次矩阵乘法 (512×512)\n", (long long)iter_count);
        printf(" 总计:   %.2e 次浮点运算\n", total_flops_sum);
        printf(" 均值:   %.2e FLOPS\n", total_flops_sum / (double)runtime_sec);
        printf(" 校验:   checksum=%.10e\n", total_checksum);
        printf("========================================\n");
    }

    free(A); free(B); free(C);
    MPI_Finalize();
    return 0;
}

/*
 * mpi_bench.c — MPI CPU binding benchmark
 *
 * Numerical integration of π = ∫₀¹ 4/(1+x²) dx with fine-grained
 * steps for dense FP workload. Every second rank 0 reports the
 * cgroup CPU affinity and the actual CPUs each process runs on.
 *
 * Use with ts --cpu-bind to verify binding effectiveness.
 *
 * Compile:
 *   mpicc -O2 -o mpi_bench mpi_bench.c
 *
 * Run:
 *   mpirun -np 4 ./mpi_bench 10              # 4 procs, 10 seconds
 *   mpirun -np 4 ./mpi_bench 10 4000000000   # custom steps/sec/proc
 *   ts -N 4 mpirun -np 4 ./mpi_bench 10      # bind 4 CPUs
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <mpi.h>
#include <pthread.h>
#include <sched.h>

/* ---- 默认每进程每秒步数 ---- */
#define STEPS_PER_SEC_DEFAULT  2000000000  /* 20亿步/秒/进程 */

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

    int cpus[CPU_SETSIZE], n = 0;
    for (int i = 0; i < CPU_SETSIZE; i++)
        if (CPU_ISSET(i, &mask)) cpus[n++] = i;

    if (n == 0) { snprintf(buf, sizeof(buf), "(none)"); return buf; }

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
            fprintf(stderr, "Usage: mpirun -np N %s <seconds> [steps/sec/proc]\n",
                    argv[0]);
        MPI_Finalize();
        return 1;
    }

    int runtime_sec = atoi(argv[1]);
    if (runtime_sec <= 0) runtime_sec = 10;

    long long steps_per_sec = STEPS_PER_SEC_DEFAULT;
    if (argc >= 3) {
        steps_per_sec = atoll(argv[2]);
        if (steps_per_sec < 1000000) steps_per_sec = 1000000;
    }

    /* ---- rank 0 显示启动信息 ---- */
    if (rank == 0) {
        printf("========================================\n");
        printf(" Cmd:");
        for (int i = 0; i < argc; i++) printf(" %s", argv[i]);
        printf("\n");
        printf(" MPI procs: %d\n", nprocs);
        printf(" Steps:     %lld /sec/proc\n", steps_per_sec);
        printf(" Duration:  %d sec\n", runtime_sec);
        printf(" Affinity:  %s\n", get_affinity_range());
        printf("========================================\n");
        fflush(stdout);
    }

    /* ---- π 积分参数 ---- */
    double h = 1.0 / (double)nprocs;          /* 每进程区间宽度 */
    double x_start = (double)rank * h;        /* 本进程起始 x */

    /* ---- 主循环 ---- */
    const double FLOPS_PER_STEP = 4.0;        /* +, *, /, + */
    double flop_count = 0.0, prev_flops = 0.0;
    double pi_sum = 0.0;
    long long total_steps = 0;

    /* 小块大小：约 1/10 秒的计算量 */
    long long chunk = steps_per_sec / 10;
    if (chunk < 1) chunk = 1;

    double start_wall = MPI_Wtime();
    double end_wall = start_wall + (double)runtime_sec + 1.0;
    double next_report = start_wall + 1.0;
    int step = 0;

    while (MPI_Wtime() < end_wall) {
        /* 用矩形法算一个 CHUNK */
        double local_pi = 0.0;
        for (long long i = 0; i < chunk; i++) {
            long long idx = total_steps + i;
            double x = x_start + h * ((double)idx + 0.5) / (double)steps_per_sec;
            local_pi += 4.0 / (1.0 + x * x);
            flop_count += FLOPS_PER_STEP;
        }
        local_pi *= h / (double)steps_per_sec;

        /* 规约 π */
        double total_pi;
        MPI_Allreduce(&local_pi, &total_pi, 1, MPI_DOUBLE, MPI_SUM,
                       MPI_COMM_WORLD);
        pi_sum += total_pi;
        total_steps += chunk;

        /* 每秒报告 */
        double now = MPI_Wtime();
        if (now >= next_report) {
            step++;
            if (step <= runtime_sec) {
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

                    char cpu_buf[256] = "";
                    int pos = 0;
                    for (int i = 0; i < nprocs; i++) {
                        int written = snprintf(cpu_buf + pos,
                                    sizeof(cpu_buf) - (size_t)pos,
                                    "%s%d", i > 0 ? "," : "", all_cpus[i]);
                        if (written > 0) pos += written;
                    }

                    /* π 归一化：已完成 steps_per_sec 步才算一轮完整积分 */
                    double pi_now = total_steps > 0
                        ? pi_sum * (double)steps_per_sec / (double)total_steps
                        : 0.0;
                    printf(" [%d/%ds] affinity[%s]  cpu[%s]  π=%.10f  %.2e FLOPS  (%lld steps)\n",
                           step, runtime_sec,
                           get_affinity_range(), cpu_buf,
                           pi_now, delta,
                           (long long)nprocs * total_steps);
                    fflush(stdout);
                    free(all_cpus);
                }
            }
            next_report = now + 1.0;
        }
    }

    /* ---- 最终汇总 ---- */
    double total_flops_sum;
    MPI_Reduce(&flop_count, &total_flops_sum, 1, MPI_DOUBLE, MPI_SUM, 0,
               MPI_COMM_WORLD);

    if (rank == 0) {
        double pi_final = total_steps > 0
            ? pi_sum * (double)steps_per_sec / (double)total_steps
            : 0.0;
        double err = pi_final - 3.14159265358979323846;
        printf("========================================\n");
        printf(" π ≈ %.10f  (error %+.2e)\n", pi_final, err);
        printf(" Steps/proc: %lld\n", (long long)total_steps);
        printf(" Total FP:   %.2e ops\n", total_flops_sum);
        printf(" Avg:        %.2e FLOPS\n", total_flops_sum / (double)runtime_sec);
        printf("========================================\n");
    }

    MPI_Finalize();
    return 0;
}

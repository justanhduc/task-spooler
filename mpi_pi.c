/*
 * mpi_pi.c — Hybrid MPI + OpenMP π benchmark
 *
 * Numerical integration of π = ∫₀¹ 4/(1+x²) dx.
 * Each MPI rank uses OpenMP threads to parallelize its interval.
 *
 * Compile:
 *   mpicc -O2 -fopenmp -o mpi_pi mpi_pi.c
 *
 * Run:
 *   mpirun -np 4 ./mpi_pi 10                # 4 MPI, 1 thread each
 *   mpirun -np 4 ./mpi_pi 10 -nt 4          # 4 MPI, 4 OMP threads each
 *   ts -N 4 mpirun -np 4 ./mpi_pi 10 -nt 4  # with ts cpu_bind
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <mpi.h>
#include <pthread.h>
#include <sched.h>
#include <omp.h>

/* ---- qsort helper ---- */
static int int_cmp(const void *a, const void *b)
{
    return *(const int *)a - *(const int *)b;
}

/* ---- get cgroup CPU affinity as range string ---- */
static const char *get_affinity(void)
{
    static char buf[256];
    cpu_set_t mask;
    CPU_ZERO(&mask);
    if (pthread_getaffinity_np(pthread_self(), sizeof(mask), &mask)) {
        snprintf(buf, sizeof(buf), "?");
        return buf;
    }
    int cpus[CPU_SETSIZE], n = 0;
    for (int i = 0; i < CPU_SETSIZE; i++)
        if (CPU_ISSET(i, &mask)) cpus[n++] = i;
    if (n == 0) { snprintf(buf, sizeof(buf), "none"); return buf; }
    int pos = 0, first = 1, start = cpus[0], end = cpus[0];
    for (int i = 1; i <= n; i++) {
        if (i < n && cpus[i] == end + 1) { end = cpus[i]; continue; }
        pos += snprintf(buf + pos, sizeof(buf) - (size_t)pos,
                        "%s%d", first ? "" : ",", start);
        if (start != end)
            pos += snprintf(buf + pos, sizeof(buf) - (size_t)pos, "-%d", end);
        first = 0;
        if (i < n) start = end = cpus[i];
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
            fprintf(stderr, "Usage: mpirun -np N %s <seconds> [-nt <threads>]\n", argv[0]);
        MPI_Finalize();
        return 1;
    }

    int secs = atoi(argv[1]);
    if (secs <= 0) secs = 10;

    /* Parse -nt nthread (default 1) */
    int omp_threads = 1;
    for (int i = 2; i < argc - 1; i++) {
        if (strcmp(argv[i], "-nt") == 0) {
            omp_threads = atoi(argv[i + 1]);
            if (omp_threads < 1) omp_threads = 1;
            break;
        }
    }
    omp_set_num_threads(omp_threads);

    /* header */
    if (rank == 0) {
        printf("========================================\n");
        printf(" Cmd:");
        for (int i = 0; i < argc; i++) printf(" %s", argv[i]);
        printf("\n");
        printf(" MPI procs: %d\n", nprocs);
        printf(" OMP threads: %d\n", omp_threads);
        printf(" Total workers: %d\n", nprocs * omp_threads);
        printf(" Time:  %d sec\n", secs);
        printf(" Affinity: %s\n", get_affinity());
        printf("========================================\n");
        fflush(stdout);
    }

    /* pi: each rank integrates [rank/nprocs, (rank+1)/nprocs) */
    double h = 1.0 / nprocs;
    double x0 = rank * h;

double start = MPI_Wtime();
    double last_report_time = start; // 记录上一次打印的精准时间
    double next_out = start + 1.0;
    int step = 0;
    
    double pi_acc = 0.0;
    long long step_acc = 0;
    long long prev_step_acc = 0; // 上一次打印时的总步数

    const long long SLICE = 200000000;
    // 每一个循环周期，单 rank 执行的真实浮点操作数
    // 循环内固定 6 次：2次乘法(*)、1次加法(+)、1次除法(/)、2次分支/权重运算
    // 加上循环结束后的 local *= dx / 3.0 (2次)
    const double FLOPS_PER_CYCLE = (double)(SLICE + 1) * 6.0 + 2.0;

    while (MPI_Wtime() < start + secs + 1.0) {
        double local = 0.0;
        double dx = h / SLICE;

        #pragma omp parallel for reduction(+:local) if(omp_threads > 1)
        for (long long i = 0; i <= SLICE; i++) {
            double x = x0 + i * dx;
            double f = 4.0 / (1.0 + x * x);
            if (i == 0 || i == SLICE)
                local += f;
            else if (i % 2 == 1)
                local += 4.0 * f;
            else
                local += 2.0 * f;
        }
        local *= dx / 3.0;

        double total;
        MPI_Allreduce(&local, &total, 1, MPI_DOUBLE, MPI_SUM, MPI_COMM_WORLD);
        pi_acc += total;
        step_acc += SLICE; // 所有 rank 同步增加步数

        /* --- 每一秒汇报 --- */
        double now = MPI_Wtime();
        if (now >= next_out) {
            step++;
            if (step <= secs) {
                int mycpu = sched_getcpu();
                int *cpus = NULL;
                if (rank == 0) cpus = malloc(nprocs * sizeof(int));
                MPI_Gather(&mycpu, 1, MPI_INT, cpus, 1, MPI_INT, 0, MPI_COMM_WORLD);

                if (rank == 0) {
                    qsort(cpus, nprocs, sizeof(int), int_cmp);

                    // 1. 精准计算当前周期的总时间差
                    double delta_time = now - last_report_time;
                    
                    // 2. 精准计算当前周期内所有 rank 完成的总浮点数
                    double delta_steps = (double)(step_acc - prev_step_acc);
                    double delta_flops = delta_steps * FLOPS_PER_CYCLE * nprocs / SLICE;
                    
                    char cpubuf[256] = "";
                    int pos = 0;
                    for (int i = 0; i < nprocs; i++)
                        pos += snprintf(cpubuf + pos, sizeof(cpubuf) - pos,
                                        "%s%d", i ? "," : "", cpus[i]);

                    double pi_val = pi_acc * (double)SLICE / (double)step_acc;
                    
                    // 核心修改：真正的 GFLOPS = 浮点数 / 时间差 / 1e9
                    printf(" [%d/%d] affinity[%s]  cpu[%s]  π=%.10f  %.2f GLOPS\n",
                           step, secs, get_affinity(), cpubuf, pi_val, (delta_flops / delta_time) / 1e9);
                    fflush(stdout);
                    free(cpus);
                }
                // 所有 rank 统一更新时间步长基准
                prev_step_acc = step_acc;
                last_report_time = now;
            }
            next_out = now + 1.0;
        }
    }

    // ================= SUMMARY =================
    // 所有的 rank 都能正确拿到自己的 step_acc，在 rank 0 汇总即可
    if (rank == 0) {
        double total_elapsed_time = last_report_time - start;
        double total_flop = (double)nprocs * ((double)step_acc / SLICE) * FLOPS_PER_CYCLE;
        double pi_final = pi_acc * (double)SLICE / (double)(step_acc > 0 ? step_acc : 1);
        double err = pi_final - 3.14159265358979323846;
        
        printf("========================================\n");
        printf(" π ≈ %.10f  (error %+.2e)\n", pi_final, err);
        printf(" Steps/proc: %.2f B\n", (double)step_acc / 1e9);
        printf(" Total FP:   %.2f G ops\n", total_flop / 1e9);
        printf(" Avg:        %.2f GLOPS\n", (total_flop / total_elapsed_time) / 1e9);
        printf("========================================\n");
    }
    MPI_Finalize();
    return 0;
}

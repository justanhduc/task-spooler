/*
 * mpi_pi.c — Hybrid MPI + OpenMP π benchmark
 *
 * Numerical integration of π = ∫₀¹ 4/(1+x²) dx.
 * Each MPI rank uses OpenMP threads to parallelize its interval.
 *
 * Compile:
 * mpicc -O2 -fopenmp -o mpi_pi mpi_pi.c
 *
 * Run:
 * mpirun -np 4 ./mpi_pi 10                 # 运行 10 秒
 * mpirun -np 4 ./mpi_pi 10 -nt 4          # 运行 10 秒，每个 Rank 4 线程
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

    double h = 1.0 / nprocs;
    double x0 = rank * h;

    double start = MPI_Wtime();
    double last_report_time = start; 
    double next_out = start + 1.0;
    int step = 0;
    
    double pi_acc = 0.0;
    long long step_acc = 0;
    long long prev_step_acc = 0; 

    const long long SLICE = 200000000;
    /* Simpson inner loop: per-point 2(x)+3(f)+1|2(weight) + final *=dx/3.0(2) */
    const double FLOPS_PER_CYCLE = 7.0 * (double)SLICE + 7.0;

    // 状态控制变量（用数组统一广播：[0]表示是否继续循环，[1]表示本轮是否执行打印）
    int ctrl[2] = {1, 0}; 

    while (1) {
        // 1. 只有 Rank 0 负责算时间、下达指令
        if (rank == 0) {
            double now = MPI_Wtime();
            // 判断是否超时（多预留1秒，保证满额跑完指定的 secs）
            if (now >= start + secs + 0.9) {
                ctrl[0] = 0; // 通知所有人：退出循环
            } else {
                ctrl[0] = 1; // 继续跑
            }

            // 判断是否到了 1 秒的打印周期
            if (now >= next_out && step < secs) {
                ctrl[1] = 1; // 通知所有人：这一轮我们需要打印
                step++;
                next_out = now + 0.9; // 顺延下一次打印时间
            } else {
                ctrl[1] = 0; // 这一轮不打印
            }
        }

        // 2. 关键点：Rank 0 把决定广播给所有人，步调达成绝对一致
        MPI_Bcast(ctrl, 2, MPI_INT, 0, MPI_COMM_WORLD);

        // 如果 Rank 0 下令退出，所有人一起退出
        if (ctrl[0] == 0) {
            break;
        }

        /* ---- 核心计算（所有 Rank 共同执行） ---- */
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
        step_acc += SLICE; 

        /* ---- 统一判定是否进行 1 秒汇报 ---- */
        if (ctrl[1] == 1) {
            int mycpu = sched_getcpu();
            int *cpus = NULL;
            if (rank == 0) cpus = malloc(nprocs * sizeof(int));
            
            // 因为 ctrl[1] 在所有 Rank 里都等于 1，所有人都会安全调用，绝不发生死锁
            MPI_Gather(&mycpu, 1, MPI_INT, cpus, 1, MPI_INT, 0, MPI_COMM_WORLD);

            double now = MPI_Wtime();

            if (rank == 0) {
                qsort(cpus, nprocs, sizeof(int), int_cmp);

                double delta_time = now - last_report_time;
                double delta_steps = (double)(step_acc - prev_step_acc);
                double delta_flops = delta_steps * FLOPS_PER_CYCLE * nprocs / SLICE;
                
                char cpubuf[256] = "";
                int pos = 0;
                for (int i = 0; i < nprocs; i++)
                    pos += snprintf(cpubuf + pos, sizeof(cpubuf) - pos,
                                    "%s%d", i ? "," : "", cpus[i]);

                double pi_val = pi_acc * (double)SLICE / (double)step_acc;
                
                printf(" [%d/%d] affinity[%s]  cpu[%s]  π=%.10f  %.2f GFLOPS, dt = %.4f sec\n",
                       step, secs, get_affinity(), cpubuf, pi_val, (delta_flops / delta_time) / 1e9, delta_time);
                fflush(stdout);
                free(cpus);
            }
            
            // 所有 rank 统一更新时间步长基准
            prev_step_acc = step_acc;
            last_report_time = now;
        }
    }

    // ================= SUMMARY =================
    if (rank == 0) {
        double end_time = MPI_Wtime();
        double total_elapsed_time = end_time - start;
        double total_flop = (double)nprocs * ((double)step_acc / SLICE) * FLOPS_PER_CYCLE;
        double pi_final = pi_acc * (double)SLICE / (double)(step_acc > 0 ? step_acc : 1);
        double err = pi_final - 3.14159265358979323846;
        
        printf("========================================\n");
        printf(" π ≈ %.10f  (error %+.2e)\n", pi_final, err);
        printf(" Steps/proc: %.2f B\n", (double)step_acc / 1e9);
        printf(" Total FP:   %.2f G ops\n", total_flop / 1e9);
        printf(" Avg:        %.2f GLOPS\n", (total_flop / total_elapsed_time) / 1e9);
        printf(" Elps:       %.2f sec\n", total_elapsed_time);
        printf("========================================\n");
    }

    MPI_Finalize();
    return 0;
}
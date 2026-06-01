/*
 * mpi_test.c — CPU 绑定测试程序
 *
 * 模拟计算负载，定期显示 FLOPS。
 * 配合 ts + cpu_bind 测试 CPU 绑定效果。
 *
 * 用法:
 *   ./mpi_test <运行秒数> [线程数]
 *
 * 编译:
 *   gcc -O2 -lpthread -o mpi_test mpi_test.c
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <pthread.h>
#include <sched.h>

/* ---- 每个线程的工作参数 ---- */
typedef struct {
    int          thread_id;
    int          runtime_sec;
    volatile int stop_flag;
    volatile double flops; /* 该线程完成的浮点运算数 */
} ThreadCtx;

/* 浮点计算负载 — 混合乘加，阻止编译器优化 */
static double workload(double a, double b, int iterations)
{
    double x = a, y = b;
    for (int i = 0; i < iterations; i++) {
        x = x * y + a;
        y = y * x + b;
    }
    return x + y;
}

/* 每线程入口 */
static void *thread_run(void *arg)
{
    ThreadCtx *ctx = (ThreadCtx *)arg;
    const int CHUNK = 500000;       /* 每次调用的浮点运算次数 */
    double acc = 1.0;
    double count = 0;

    while (!ctx->stop_flag) {
        acc += workload(acc, acc + 0.5, CHUNK);
        count += (double)CHUNK * 4.0;  /* workload 内每次迭代约 4 次浮点运算 */
        ctx->flops = count;            /* 实时更新供主线程读取 */
    }

    /* 防止编译器优化掉 acc */
    if (acc < 0) printf("[%d] unexpected\n", ctx->thread_id);
    return NULL;
}

/* 获取当前绑定的 CPU 个数 */
static int get_cpu_count(void)
{
    cpu_set_t mask;
    CPU_ZERO(&mask);

    if (sched_getaffinity(0, sizeof(mask), &mask) != 0) {
        perror("sched_getaffinity");
        return sysconf(_SC_NPROCESSORS_ONLN);
    }

    int count = 0;
    for (int i = 0; i < CPU_SETSIZE; i++) {
        if (CPU_ISSET(i, &mask)) count++;
    }
    return count;
}

/* 获取绑定的 CPU 列表字符串（静态缓冲区） */
static const char *get_cpu_list(void)
{
    static char buf[256];
    cpu_set_t mask;
    CPU_ZERO(&mask);

    if (sched_getaffinity(0, sizeof(mask), &mask) != 0) {
        snprintf(buf, sizeof(buf), "(unknown)");
        return buf;
    }

    int pos = 0;
    int first = 1;
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
    if (argc < 2) {
        fprintf(stderr, "用法: %s <运行秒数> [线程数]\n", argv[0]);
        fprintf(stderr, "示例: %s 10 4    — 运行 10 秒，4 线程\n", argv[0]);
        return 1;
    }

    int runtime_sec = atoi(argv[1]);
    if (runtime_sec <= 0) runtime_sec = 10;

    int num_threads = 1;
    if (argc >= 3) {
        num_threads = atoi(argv[2]);
        if (num_threads < 1) num_threads = 1;
    }

    /* 显示启动信息 */
    printf("========================================\n");
    printf(" 命令: ");
    for (int i = 0; i < argc; i++)
        printf("%s%c", argv[i], i < argc - 1 ? ' ' : '\n');
    printf(" 运行: %d 秒\n", runtime_sec);
    printf(" 线程: %d\n", num_threads);
    printf(" CPU:  %s (%d 核)\n", get_cpu_list(), get_cpu_count());
    printf("========================================\n");
    fflush(stdout);

    /* 创建线程 */
    pthread_t *threads = malloc((size_t)num_threads * sizeof(pthread_t));
    ThreadCtx *ctxs   = calloc((size_t)num_threads, sizeof(ThreadCtx));
    if (!threads || !ctxs) {
        fprintf(stderr, "malloc failed\n");
        return 1;
    }

    for (int i = 0; i < num_threads; i++) {
        ctxs[i].thread_id   = i;
        ctxs[i].runtime_sec = runtime_sec;
        ctxs[i].stop_flag   = 0;
        ctxs[i].flops       = 0;
        pthread_create(&threads[i], NULL, thread_run, &ctxs[i]);
    }

    /* 每秒输出 FLOPS */
    time_t start = time(NULL);
    double prev_total = 0.0;

    for (int elapsed = 1; elapsed <= runtime_sec; elapsed++) {
        sleep(1);

        double total = 0.0;
        for (int i = 0; i < num_threads; i++)
            total += ctxs[i].flops;

        double delta_flops = total - prev_total;
        prev_total = total;

        printf(" [%d/%ds] %.2e FLOPS  (累计 %.2e)\n",
               elapsed, runtime_sec, delta_flops, total);
        fflush(stdout);
    }

    /* 停止所有线程 */
    for (int i = 0; i < num_threads; i++)
        ctxs[i].stop_flag = 1;

    for (int i = 0; i < num_threads; i++)
        pthread_join(threads[i], NULL);

    /* 最终汇总 */
    double final_total = 0.0;
    for (int i = 0; i < num_threads; i++)
        final_total += ctxs[i].flops;

    double real_sec = (double)(time(NULL) - start);
    printf("========================================\n");
    printf(" 总计: %.2e 次浮点运算, %.1f 秒\n", final_total, real_sec);
    printf(" 均值: %.2e FLOPS\n", final_total / real_sec);
    printf("========================================\n");

    free(threads);
    free(ctxs);
    return 0;
}

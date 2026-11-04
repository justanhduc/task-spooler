#include <stdlib.h>
#include <stdio.h>
#include "main.h"
#include "feishu.h"
int main(int argc, char **argv) {
    struct Job job = {0};
    int allowed[] = {0, 2}, assigned[] = {2};
    if (argc != 3 && argc != 4) return 2;
    job.jobid = 9; job.command = "python eval.py"; job.label = "test";
    job.num_gpus = 1; job.gpu_ids = assigned;
    job.allowed_gpu_ids = allowed; job.num_allowed_gpus = 2;
    job.wait_free_gpus = GPU_ALLOWED_FREE;
    job.state = FINISHED; job.pid = 123;
    job.result.real_ms = strtof(argv[1], NULL);
    if (atoi(argv[2])) { job.result.died_by_signal = 1; job.result.signal = 15; }
    const char *event = argc == 4 ? argv[3] : "succeeded";
    if (argc == 4) { job.state = ALLOCATING; job.pid = 0; }
    char *payload = feishu_card_payload(&job, event);
    if (!payload) return 1;
    puts(payload); free(payload); return 0;
}

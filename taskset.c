#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "main.h"
#define MAX_CORE_NUM 16 // 256
#define MAX_CORE_NUM_HALF 8 // 128
#define MAX_CORE_NUM_QUAD 4 // 64

static int core_id[MAX_CORE_NUM];
// = {0,64,1,65,2,66,3,67,4,68,5,69,6,70,7,71,8,72,9,73,10,74,11,75,12,76,13,77,14,78,15,79,16,80,17,81,18,82,19,83,20,84,21,85,22,86,23,87,24,88,25,89,26,90,27,91,28,92,29,93,30,94,31,95,32,96,33,97,34,98,35,99,36,100,37,101,38,102,39,103,40,104,41,105,42,106,43,107,44,108,45,109,46,110,47,111,48,112,49,113,50,114,51,115,52,116,53,117,54,118,55,119,56,120,57,121,58,122,59,123,60,124,61,125,62,126,63,127,128,192,129,193,130,194,131,195,132,196,133,197,134,198,135,199,136,200,137,201,138,202,139,203,140,204,141,205,142,206,143,207,144,208,145,209,146,210,147,211,148,212,149,213,150,214,151,215,152,216,153,217,154,218,155,219,156,220,157,221,158,222,159,223,160,224,161,225,162,226,163,227,164,228,165,229,166,230,167,231,168,232,169,233,170,234,171,235,172,236,173,237,174,238,175,239,176,240,177,241,178,242,179,243,180,244,181,245,182,246,183,247,184,248,185,249,186,250,187,251,188,252,189,253,190,254,191,255};
static struct Job* core_jobs[MAX_CORE_NUM] = { NULL };
int task_cores_id[MAX_CORE_NUM] = {0};
int task_array_id[MAX_CORE_NUM] = {0};
int task_core_num, core_usage;

void init_taskset() {
    // printf("CPU taskset()\n");
    task_core_num = 0;
    core_usage = 0;
    #ifdef TASKSET
    for (int i = 0; i < MAX_CORE_NUM; i++) {
        core_id[i] = i / 2 + ((i % 2) + (i >= MAX_CORE_NUM_HALF)) * MAX_CORE_NUM_QUAD;
        printf("[%3d] => %3d\t", i, core_id[i]);
        if ((i+1)%8 == 0) printf("\n");
    }
    #endif
}

int allocate_cores(int N) {
    if (N + core_usage > MAX_CORE_NUM) return 0;
    task_core_num = 0;
    int i = 0;
    while(task_core_num < N && i < MAX_CORE_NUM) {
        if (core_jobs[i] == NULL) {
            task_cores_id[task_core_num] = core_id[i];
            task_array_id[task_core_num] = i;
            task_core_num++;
        }
        i++;
    }
    if (task_core_num != N) task_core_num = 0;
    return task_core_num;
}

void lock_core_by_job(struct Job* p) {
    if (p == NULL) return;
    for (int i = 0; i < task_core_num; i++) {
        int iA = task_array_id[i];
        core_jobs[iA] = p;
    }
    core_usage += task_core_num;
    task_core_num = 0;
}

void unlock_core_by_job(struct Job* p) {
    #ifdef TASKSET
    if (p == NULL) return;
    for (int i = 0; i < MAX_CORE_NUM; i++) {
        if (core_jobs[i] == p) {
            core_jobs[i] = NULL;
            core_usage--;
        }
    }
    free(p->cores);
    p->cores = NULL;
    #endif
}

int set_task_cores(struct Job* p, const char* extra) {
    if (p == NULL || p->pid <= 0) return -1;
#ifdef TASKSET
    int N = p->num_slots;

    if (allocate_cores(N) != N) {
        printf("cannot allocate %d cores\n", N);
        return -1;
    }
    lock_core_by_job(p);
    
    char* core_str = ints_to_chars(N, task_cores_id, ",");
    int size = strlen(core_str) + 50;
    char* cmd = (char*) malloc(sizeof(char) * size);
    sprintf(cmd, "taskset -cp %s ", core_str);
    if (extra == NULL) {
        ; // printf("[CMD] %s %d\n", cmd, p->pid);
    } else {       
        printf("[CMD] %s %d; %s %d\n", cmd, p->pid, extra, p->pid);
    }

    kill_pid(p->pid, cmd, extra);
    p->cores = core_str;
    // free(core_str);
    free(cmd);
#endif
    return 0;
}



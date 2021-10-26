//
// Created by justanhduc on 11/10/20.
//

#include <stdlib.h>
#include <nvml.h>
#include <string.h>

#include "main.h"

#define TS_VISIBLE_DEVICES "TS_VISIBLE_DEVICES"

int *used_gpus;
int num_total_gpus;

void initGPU() {
    unsigned int nDevices;
    nvmlReturn_t result;

    result = nvmlInit();
    if (NVML_SUCCESS != result)
        error("Failed to initialize NVML: %s", nvmlErrorString(result));

    result = nvmlDeviceGetCount_v2(&nDevices);
    if (NVML_SUCCESS != result) {
        error("Failed to get device count: %s", nvmlErrorString(result));
        goto Error;
    }
    num_total_gpus = (int) nDevices;
    used_gpus = (int *) malloc(num_total_gpus * sizeof(int));
    memset(used_gpus, 0, num_total_gpus * sizeof(int));  /* 0 is not in used, 1 is in used */
    return;

    Error:
        result = nvmlShutdown();
        if (NVML_SUCCESS != result)
            error("Failed to shutdown NVML: %s", nvmlErrorString(result));
}

static int getVisibleGpus(int *visibility) {
    const char* tmp = getenv(TS_VISIBLE_DEVICES);

    if (tmp) {
        char* visFlag = malloc(strlen(tmp) + 1);
        strcpy(visFlag, tmp);
        int num = strtok_int(visFlag, ",", visibility);
        return num;
    }

    for (int i = 0; i < num_total_gpus; i++)
        visibility[i] = i;

    return num_total_gpus;
}

int * getGpuList(int *num) {
    int *gpuList, *visible;
    int i, count = 0;
    int numVis;
    nvmlReturn_t result;

    result = nvmlInit();
    if (NVML_SUCCESS != result)
        error("Failed to initialize NVML: %s", nvmlErrorString(result));

    visible = malloc(num_total_gpus * sizeof(int));
    numVis = getVisibleGpus(visible);
    if (numVis == 0) {
        *num = 0;
        goto Error;
    }

    gpuList = (int *) malloc(numVis * sizeof(int));
    for (i = 0; i < numVis; i++) {
        nvmlMemory_t mem;
        nvmlDevice_t dev;
        result = nvmlDeviceGetHandleByIndex_v2(visible[i], &dev);
        if (result != 0) {
            error("Failed to get GPU handle for GPU %d: %s", visible[i], nvmlErrorString(result));
            goto Error;
        }

        result = nvmlDeviceGetMemoryInfo(dev, &mem);
        if (result != 0) {
            error("Failed to get GPU memory for GPU %d: %s", visible[i], nvmlErrorString(result));
            goto Error;
        }

        if (mem.free > .9 * mem.total)
            gpuList[count++] = visible[i];
    }
    *num = count;
    result = nvmlShutdown();
    if (NVML_SUCCESS != result)
        error("Failed to shutdown NVML: %s", nvmlErrorString(result));

    return gpuList;

    Error:
        result = nvmlShutdown();
        if (NVML_SUCCESS != result)
            error("Failed to shutdown NVML: %s", nvmlErrorString(result));
        return NULL;
}

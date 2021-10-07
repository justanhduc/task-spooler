//
// Created by justanhduc on 11/10/20.
//

#include <stdlib.h>
#include <nvml.h>
#include <string.h>

#include "main.h"

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

int * getGpuList(int *num) {
    int * gpuList;
    int i, count = 0;
    nvmlReturn_t result;

    result = nvmlInit();
    if (NVML_SUCCESS != result)
        error("Failed to initialize NVML: %s", nvmlErrorString(result));

    gpuList = (int *) malloc(num_total_gpus * sizeof(int));
    for (i = 0; i < num_total_gpus; ++i) {
        nvmlMemory_t mem;
        nvmlDevice_t dev;
        result = nvmlDeviceGetHandleByIndex_v2(i, &dev);
        if (result != 0) {
            error("Failed to get GPU handle for GPU %d: %s", i, nvmlErrorString(result));
            goto Error;
        }

        result = nvmlDeviceGetMemoryInfo(dev, &mem);
        if (result != 0) {
            error("Failed to get GPU memory for GPU %d: %s", i, nvmlErrorString(result));
            goto Error;
        }

        if (mem.free < .9 * mem.total)
            continue;

        gpuList[count++] = i;
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

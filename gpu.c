//
// Created by justanhduc on 11/10/20.
//

#include <stdlib.h>
#include <nvml.h>

#include "main.h"

int * getFreeGpuList(int *numFree) {
    int * gpuList;
    unsigned int nDevices;
    int i, j = 0, count = 0;
    nvmlReturn_t result;

    result = nvmlInit();
    if (NVML_SUCCESS != result)
        error("Failed to initialize NVML: %s", nvmlErrorString(result));

    result = nvmlDeviceGetCount_v2(&nDevices);
    if (NVML_SUCCESS != result)
        error("Failed to get device count: %s", nvmlErrorString(result));

    gpuList = (int *) malloc(nDevices * sizeof(int));
    for (i = 0; i < nDevices; ++i) {
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

        if (mem.free > .1 * mem.total) {
            gpuList[j] = i;
            count++;
            j++;
        }
    }
    *numFree = count;
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

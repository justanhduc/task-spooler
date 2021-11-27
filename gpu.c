//
// Created by justanhduc on 11/10/20.
//
#define _DEFAULT_SOURCE
#include <stdlib.h>
#include <nvml.h>
#include <string.h>

#include "main.h"

#define TS_VISIBLE_DEVICES "TS_VISIBLE_DEVICES"

static int free_percentage = 90;
static int num_total_gpus;
static struct GPU *gpuList;

static void queryGpu();

static void unset_cuda_visible_devices_env() {
    unsetenv("CUDA_VISIBLE_DEVICES");
}

void initGPU() {
    unsigned int nDevices;
    nvmlReturn_t result;

    unset_cuda_visible_devices_env();
    result = nvmlInit();
    if (NVML_SUCCESS != result)
        warning("Failed to initialize NVML: %s", nvmlErrorString(result));

    result = nvmlDeviceGetCount_v2(&nDevices);
    if (NVML_SUCCESS != result)
        warning("Failed to get device count: %s", nvmlErrorString(result));

    num_total_gpus = (int) nDevices;
    gpuList = malloc(sizeof(struct GPU) * num_total_gpus);
    queryGpu();
    for (int i = 0; i < num_total_gpus; i++)
        gpuList[i].available = 1;
}

static void getVisibleGpus() {
    const char* tmp = getenv(TS_VISIBLE_DEVICES);

    if (tmp) {
        char* visFlag = malloc(strlen(tmp) + 1);
        int *visibility = malloc(sizeof(int) * num_total_gpus);
        int num;

        strcpy(visFlag, tmp);
        num = strtok_int(visFlag, ",", visibility);
        for (int i = 0; i < num_total_gpus; i++)
            gpuList[i].visible = 0;

        for (int i = 0; i < num; i++)
            if (visibility[i] >= 0)
                gpuList[visibility[i]].visible = 1;

        return;
    }
    for (int i = 0; i < num_total_gpus; i++)
        gpuList[i].visible = 1;
}

static void queryGpu() {
    nvmlReturn_t result;

    for (int i = 0; i < num_total_gpus; i++) {
        nvmlMemory_t mem;
        nvmlDevice_t dev;
        nvmlUtilization_t util;

        result = nvmlDeviceGetHandleByIndex_v2(i, &dev);
        if (result != 0)
            warning("Failed to get GPU handle for GPU %d: %s", i, nvmlErrorString(result));

        result = nvmlDeviceGetMemoryInfo(dev, &mem);
        if (result != 0)
            warning("Failed to get GPU memory for GPU %d: %s", i, nvmlErrorString(result));

        // utilization
        result = nvmlDeviceGetUtilizationRates(dev, &util);
        if (result != 0)
            warning("Failed to get GPU utilization for GPU %d: %s", i, nvmlErrorString(result));

        // name
        gpuList[i].name = malloc(NVML_DEVICE_NAME_BUFFER_SIZE + 1);
        result = nvmlDeviceGetName(dev, gpuList[i].name, NVML_DEVICE_NAME_BUFFER_SIZE);
        if (result != 0)
            warning("Failed to get name of device %u: %s\n", i, nvmlErrorString(result));

        gpuList[i].total_mem = mem.total / (1024. * 1024. * 1024.);
        gpuList[i].used_mem = mem.used / (1024. * 1024. * 1024.);
        gpuList[i].free_mem = mem.free / (1024. * 1024. * 1024.);
        gpuList[i].usage = (int) util.gpu;
    }
    getVisibleGpus();
}

int * getGpuList(int *num) {
    int *freeGpuList;
    int count = 0;

    queryGpu();
    freeGpuList = malloc(sizeof(int) * num_total_gpus);
    for (int i = 0; i < num_total_gpus; i++)
        if (gpuList[i].free_mem > free_percentage / 100. * gpuList[i].total_mem &&
            gpuList[i].visible &&
            gpuList[i].available)
            freeGpuList[count++] = i;

    *num = count;
    return freeGpuList;
}

void broadcastUsedGpus(int num, const int *list) {
    for (int i = 0; i < num; i++)
        gpuList[list[i]].available = 0;
}

void broadcastFreeGpus(int num, const int *list) {
    for (int i = 0; i < num; i++)
        gpuList[list[i]].available = 1;
}

int isAvailable(int id) {
    return gpuList[id].available;
}

void setFreePercentage(int percent) {
    free_percentage = percent;
}

int getFreePercentage() {
    return free_percentage;
}

static double round(double num) {
    int numInt = (int) num;
    if (num - numInt >= .5)
        numInt++;

    return numInt;
}

static char* getProgressBar(float percentage) {
    int perc = (int) round(percentage / 10.);
    char *bar = malloc(11);
    char *emptyBar = malloc(10 - perc + 1);
    sprintf(bar, "%.*s", perc, "||||||||||");
    sprintf(emptyBar, "%.*s", 10 - perc, "----------");
    strcat(bar, emptyBar);
    free(emptyBar);
    return bar;
}

int numGpus() {
    return num_total_gpus;
}

char *smiHeader() {
    char *line;

    line = malloc(110);
    snprintf(line, 110, "%-4s %-24s %-7s %-10s %-15s %-15s\n",
             "ID",
             "Name",
             "Status",
             "Mem (GB)",
             "Mem (%)",
             "Usage (%)");
    return line;
}

char *smiForId(int id) {
    char *line;
    float memPercent = 100. * gpuList[id].used_mem / gpuList[id].total_mem;
    char* memProgBar = getProgressBar(memPercent);
    char *utilProgBar = getProgressBar(gpuList[id].usage);

    queryGpu();
    line = malloc(110);
    snprintf(line, 110, "%-4d %-24s %d/%-5d %-10.2f %s %-4d %s %-4d\n",
             id,
             gpuList[id].name,
             gpuList[id].visible,
             gpuList[id].available,
             gpuList[id].total_mem,
             memProgBar,
             (int) memPercent,
             utilProgBar,
             gpuList[id].usage);
    return line;
}

void cleanupGpu() {
    nvmlReturn_t result;
    result = nvmlShutdown();
    if (NVML_SUCCESS != result)
        error("Failed to shutdown NVML: %s", nvmlErrorString(result));

    for (int i = 0; i < num_total_gpus; i++)
        free(gpuList[i].name);

    free(gpuList);
}

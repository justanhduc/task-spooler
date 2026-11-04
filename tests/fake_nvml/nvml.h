#ifndef TEST_NVML_H
#define TEST_NVML_H
typedef int nvmlReturn_t;
typedef unsigned int nvmlDevice_t;
typedef struct { unsigned long long total, free, used; } nvmlMemory_t;
#define NVML_SUCCESS 0
nvmlReturn_t nvmlInit(void);
nvmlReturn_t nvmlShutdown(void);
nvmlReturn_t nvmlDeviceGetCount_v2(unsigned int *count);
nvmlReturn_t nvmlDeviceGetHandleByIndex_v2(unsigned int index, nvmlDevice_t *device);
nvmlReturn_t nvmlDeviceGetMemoryInfo(nvmlDevice_t device, nvmlMemory_t *memory);
const char *nvmlErrorString(nvmlReturn_t result);
#endif

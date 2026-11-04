/* CPU-only fake NVML: each integer in the state file is a GPU's free percentage. */
#include <stdio.h>
#include <stdlib.h>
#include "nvml.h"
static int read_state(int *values) {
    const char *path = getenv("TS_TEST_GPU_STATE");
    FILE *file = path ? fopen(path, "r") : NULL;
    int count = 0;
    if (!file) return 0;
    while (count < 16 && fscanf(file, "%d", &values[count]) == 1) ++count;
    fclose(file);
    return count;
}
nvmlReturn_t nvmlInit(void) { return 0; }
nvmlReturn_t nvmlShutdown(void) { return 0; }
nvmlReturn_t nvmlDeviceGetCount_v2(unsigned int *count) {
    int values[16]; *count = read_state(values); return 0;
}
nvmlReturn_t nvmlDeviceGetHandleByIndex_v2(unsigned int index, nvmlDevice_t *device) {
    int values[16]; if (index >= (unsigned int)read_state(values)) return 1;
    *device = index; return 0;
}
nvmlReturn_t nvmlDeviceGetMemoryInfo(nvmlDevice_t device, nvmlMemory_t *memory) {
    int values[16]; if (device >= (unsigned int)read_state(values)) return 1;
    memory->total = 100; memory->free = values[device]; memory->used = 100 - memory->free; return 0;
}
const char *nvmlErrorString(nvmlReturn_t result) { (void)result; return "invalid fake GPU"; }

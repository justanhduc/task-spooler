//
// Created by justanhduc on 11/10/20.
//

#include <stdlib.h>
#include <cuda_runtime_api.h>

int * getFreeGpuList(int *numFree) {
    int * gpuList;
    int nDevices;
    int i, count = 0;

    cudaGetDeviceCount(&nDevices);
    gpuList = (int *) malloc(nDevices * sizeof(int));
    for (i = 0; i < nDevices; ++i) {
        cudaSetDevice(i);
        size_t freeMem;
        size_t totalMem;
        cudaMemGetInfo(&freeMem, &totalMem);
        if (freeMem > .9 * totalMem) {
            gpuList[i] = i;
            count++;
        }
    }
    *numFree = count;
    free(gpuList);
    return gpuList;
}

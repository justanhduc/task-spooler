//
// Created by justanhduc on 11/10/20.
//

#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>

#include "main.h"

char* getfield(char* line, int num, char *delimiter)
{
    char* tok;
    char newDelim[3];
    sprintf(newDelim, "%s\n", delimiter);
    for (tok = strtok(line, delimiter);
         tok && *tok;
         tok = strtok(NULL, newDelim))
    {
        if (!--num)
            return tok;
    }
    return NULL;
}

int * getFreeGpuList(int *numFree, float thres) {
    int * gpuList;
    int j = 0, count = -1;
    FILE *stream;
    int fd;
    int res;
    char * fname = "/tmp/tmp-gpu-query";
    char line[1024];
    int stdoutDup = dup(STDOUT_FILENO);
    int nDevices = 100;  // just a big number

    stream = fopen(fname, "w");
    fd = fileno(stream);
    dup2(fd, STDOUT_FILENO);
    res = system("nvidia-smi --query-gpu=memory.free,memory.total --format=csv");
    if (res != 0)
        error("Cannot exec nvidia-smi");

    close(fd);
    fclose(stream);
    gpuList = (int *) malloc(nDevices * sizeof(int));
    memset(gpuList, -1, nDevices);
    stream = fopen(fname, "r");
    while (fgets(line, 1024, stream))
    {
        if (count == -1) {
            count++;
            continue;
        }

        char* tmp = strdup(line);
        char * freeMB = getfield(tmp, 1, ",");
        tmp = strdup(line);
        char * totalMB = getfield(tmp, 2, ",");
        int freeMem = atoi(getfield(freeMB, 1, " "));
        int totalMem = atoi(getfield(totalMB, 1, " "));

        if (((float) freeMem / totalMem) >= thres) {
            gpuList[j] = count;
            j++;
        }
        count++;
        free(tmp);
    }
    fclose(stream);

    if (!(remove(fname) == 0))
        error("Cannot remove temp GPU query file");

    dup2(stdoutDup, STDOUT_FILENO);
    *numFree = j;
    return gpuList;
}

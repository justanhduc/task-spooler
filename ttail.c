/*
    Task Spooler - a task queue system for the unix user
    Copyright (C) 2007-2013  Lluís Batlle i Rossell

    Please find the license in the provided COPYING file.
*/
#include <sys/time.h> /* Dep de main.h */
#include <stdio.h> /* Dep de main.h */

#include "main.h"

int main(int argc, char **argv)
{
    tail_file(argv[1]);
    return 0;
}

/*
    Task Spooler - a task queue system for the unix user
    Copyright (C) 2007-2009  Lluís Batlle i Rossell

    Please find the license in the provided COPYING file.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include "main.h"

/* From jobs.c */
extern int busy_slots;
extern int max_slots;

char * joblistdump_headers()
{
    char * line;

    line = malloc(600);
    snprintf(line, 600, "#!/bin/sh\n# - task spooler (ts) job dump\n"
            "# This file has been created because a SIGTERM killed\n"
            "# your queue server.\n"
            "# The finished commands are listed first.\n"
            "# The commands running or to be run are stored as you would\n"
            "# probably run them. Take care - some quotes may have got"
            " broken\n\n");

    return line;
}

char * joblist_headers()
{
    char * line;

    line = malloc(100);
    snprintf(line, 100, "%-4s %-10s %-20s %-8s %-14s %s [run=%i/%i]\n",
            "ID",
            "State",
            "Output",
            "E-Level",
            "Times(r/u/s)",
            "Command",
            busy_slots,
            max_slots);

    return line;
}

static int max(int a, int b)
{
    if (a > b)
        return a;
    return b;
}

static const char * ofilename_shown(const struct Job *p)
{
    const char * output_filename;

    if (p->state == SKIPPED)
    {
        output_filename = "(no output)";
    } else if (p->store_output)
    {
        if (p->state == QUEUED)
        {
            output_filename = "(file)";
        } else
        {
            if (p->output_filename == 0)
                /* This may happen due to concurrency
                 * problems */
                output_filename = "(...)";
            else
                output_filename = p->output_filename;
        }
    } else
        output_filename = "stdout";


    return output_filename;
}

static char * print_noresult(const struct Job *p)
{
    const char * jobstate;
    const char * output_filename;
    int maxlen;
    char * line;
    /* 18 chars should suffice for a string like "[int]&& " */
    char dependstr[18] = "";

    jobstate = jstate2string(p->state);
    output_filename = ofilename_shown(p);

    maxlen = 4 + 1 + 10 + 1 + max(20, strlen(output_filename)) + 1 + 8 + 1
        + 14 + 1 + strlen(p->command) + 20; /* 20 is the margin for errors */

    if (p->label)
        maxlen += 3 + strlen(p->label);
    if (p->do_depend)
    {
        maxlen += sizeof(dependstr);
        if (p->depend_on == -1)
            snprintf(dependstr, sizeof(dependstr), "&& ");
        else
            snprintf(dependstr, sizeof(dependstr), "[%i]&& ", p->depend_on);
    }

    line = (char *) malloc(maxlen);
    if (line == NULL)
        error("Malloc for %i failed.\n", maxlen);

    if (p->label)
        snprintf(line, maxlen, "%-4i %-10s %-20s %-8s %14s %s[%s]%s\n",
                p->jobid,
                jobstate,
                output_filename,
                "",
                "",
		        dependstr,
                p->label,
                p->command);
    else
        snprintf(line, maxlen, "%-4i %-10s %-20s %-8s %14s %s%s\n",
                p->jobid,
                jobstate,
                output_filename,
                "",
                "",
		        dependstr,
                p->command);

    return line;
}

static char * print_result(const struct Job *p)
{
    const char * jobstate;
    int maxlen;
    char * line;
    const char * output_filename;
    /* 18 chars should suffice for a string like "[int]&& " */
    char dependstr[18] = "";

    jobstate = jstate2string(p->state);
    output_filename = ofilename_shown(p);

    maxlen = 4 + 1 + 10 + 1 + max(20, strlen(output_filename)) + 1 + 8 + 1
        + 14 + 1 + strlen(p->command) + 20; /* 20 is the margin for errors */

    if (p->label)
        maxlen += 3 + strlen(p->label);
    if (p->do_depend)
    {
        maxlen += sizeof(dependstr);
        if (p->depend_on == -1)
            snprintf(dependstr, sizeof(dependstr), "&& ");
        else
            snprintf(dependstr, sizeof(dependstr), "[%i]&& ", p->depend_on);
    }

    line = (char *) malloc(maxlen);
    if (line == NULL)
        error("Malloc for %i failed.\n", maxlen);

    if (p->label)
        snprintf(line, maxlen, "%-4i %-10s %-20s %-8i %0.2f/%0.2f/%0.2f %s[%s]"
                "%s\n",
                p->jobid,
                jobstate,
                output_filename,
                p->result.errorlevel,
                p->result.real_ms,
                p->result.user_ms,
                p->result.system_ms,
                dependstr,
                p->label,
                p->command);
    else
        snprintf(line, maxlen, "%-4i %-10s %-20s %-8i %0.2f/%0.2f/%0.2f %s%s\n",
                p->jobid,
                jobstate,
                output_filename,
                p->result.errorlevel,
                p->result.real_ms,
                p->result.user_ms,
                p->result.system_ms,
                dependstr,
                p->command);

    return line;
}

char * joblist_line(const struct Job *p)
{
    char * line;

    if (p->state == FINISHED)
        line = print_result(p);
    else
        line = print_noresult(p);

    return line;
}

char * joblistdump_torun(const struct Job *p)
{
    int maxlen;
    char * line;

    maxlen = 10 + strlen(p->command) + 20; /* 20 is the margin for errors */

    line = (char *) malloc(maxlen);
    if (line == NULL)
        error("Malloc for %i failed.\n", maxlen);

    snprintf(line, maxlen, "ts %s\n", p->command);

    return line;
}

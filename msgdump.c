/*
    Task Spooler - a task queue system for the unix user
    Copyright (C) 2007-2009  Lluís Batlle i Rossell

    Please find the license in the provided COPYING file.
*/
#include <stdio.h>
#include <sys/time.h>
#include "main.h"

void msgdump(FILE *f, const struct msg *m)
{
    fprintf(f, "msgdump:\n");
    switch(m->type)
    {
        case KILL_SERVER:
            fprintf(f, " KILL SERVER\n");
            break;
        case NEWJOB:
            fprintf(f, " NEWJOB\n");
            fprintf(f, " Commandsize: %i\n", m->u.newjob.command_size);
            break;
        case NEWJOB_OK:
            fprintf(f, " NEWJOB_OK\n");
            fprintf(f, " JobID: '%i'\n", m->u.jobid);
            break;
        case RUNJOB:
            fprintf(f, " RUNJOB\n");
            break;
        case RUNJOB_OK:
            fprintf(f, " RUNJOB_OK\n");
            fprintf(f, " Outputsize: %i\n", m->u.output.ofilename_size);
            fprintf(f, " pid: %i\n", m->u.output.pid);
            break;
        case ENDJOB:
            fprintf(f, " ENDJOB\n");
            break;
        case LIST:
            fprintf(f, " LIST\n");
            break;
        case LIST_LINE:
            fprintf(f, " LIST_LINE\n");
            fprintf(f, " Linesize: %i\n", m->u.size);
            break;
        case ASK_OUTPUT:
            fprintf(f, " ASK_OUTPUT\n");
            fprintf(f, " Jobid: %i\n", m->u.jobid);
            break;
        case ANSWER_OUTPUT:
            fprintf(f, " ANSWER_OUTPUT\n");
            fprintf(f, " Outputsize: %i\n", m->u.output.ofilename_size);
            fprintf(f, " PID: %i\n", m->u.output.pid);
            break;
        default:
            fprintf(f, " Unknown message: %i\n", m->type);
    }
}

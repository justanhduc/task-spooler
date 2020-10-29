/*
    Task Spooler - a task queue system for the unix user
    Copyright (C) 2007-2009  Lluís Batlle i Rossell

    Please find the license in the provided COPYING file.
*/
#include <unistd.h>
#include <stdio.h>
#include <time.h>
#include <sys/types.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <stdarg.h>
#include <stdlib.h>
#include <sys/time.h>

#include "main.h"

enum Etype
{
    WARNING,
    ERROR
};

/* Declared in main.h as extern */
enum Process_type process_type;

static int real_errno;

/* Local protos */
static void dump_structs(FILE *out);


static void print_date(FILE *out)
{
    time_t t;
    const char *tstr;

    t = time(NULL);
    tstr = ctime(&t);

    fprintf(out, "date %s", tstr);
}

static void dump_proc_info(FILE *out)
{
    print_date(out);
    fprintf(out, "pid %i\n", getpid());
    if (process_type == SERVER)
        fprintf(out, "type SERVER\n");
    else if (process_type == CLIENT)
        fprintf(out, "type CLIENT\n");
    else
        fprintf(out, "type UNKNOWN\n");
}

static FILE * open_error_file()
{
    int fd;
    FILE* out;
    char *path;
    int new_size;
    char *new_path;

    create_socket_path(&path);
    new_size = strlen(path)+10;
    new_path = malloc(new_size);

    strncpy(new_path, path, new_size);
    strncat(new_path, ".error", (new_size - strlen(path)) - 1);
    free(path);

    fd = open(new_path, O_CREAT | O_APPEND | O_WRONLY, 0600);
    if (fd == -1)
    {
        free(new_path);
        return 0;
    }

    out = fdopen(fd, "a");
    if (out == NULL)
    {
        close(fd);
        free(new_path);
        return 0;
    }

    free(new_path);

    return out;
}

static void print_error(FILE *out, enum Etype type, const char *str, va_list ap)
{
    fprintf(out, "-------------------");
    if (type == ERROR)
        fprintf(out, "Error\n");
    else if (type == WARNING)
        fprintf(out, "Warning\n");
    else
        fprintf(out, "Unknown kind of error\n");

    fprintf(out, " Msg: ");

    vfprintf(out, str, ap);

    fprintf(out, "\n");
    fprintf(out, " errno %i, \"%s\"\n", real_errno, strerror(real_errno));
}

static void problem(enum Etype type, const char *str, va_list ap)
{
    FILE *out;

    out = open_error_file();
    if (out == 0)
        return;

    /* out is ready */
    print_error(out, type, str, ap);
    dump_structs(out);

    /* this will close the fd also */
    fclose(out);
}

static void problem_msg(enum Etype type, const struct msg *m, const char *str, va_list ap)
{
    FILE *out;

    out = open_error_file();
    if (out == 0)
        return;

    /* out is ready */
    print_error(out, type, str, ap);
    msgdump(out, m);
    dump_structs(out);

    /* this will close the fd also */
    fclose(out);
}

void error(const char *str, ...)
{
    va_list ap;

    va_start(ap, str);

    real_errno = errno;

    if (process_type == CLIENT)
    {
        vfprintf(stderr, str, ap);
        fputc('\n', stderr);
    }

    problem(ERROR, str, ap);
    exit(-1);
}

void error_msg(const struct msg *m, const char *str, ...)
{
    va_list ap;

    va_start(ap, str);

    real_errno = errno;

    problem_msg(ERROR, m, str, ap);
    exit(-1);
}

void warning(const char *str, ...)
{
    va_list ap;

    va_start(ap, str);

    real_errno = errno;

    problem(WARNING, str, ap);
}

void warning_msg(const struct msg *m, const char *str, ...)
{
    va_list ap;

    va_start(ap, str);

    real_errno = errno;

    problem_msg(WARNING, m, str, ap);
}

static void dump_structs(FILE *out)
{
    dump_proc_info(out);
    if (process_type == SERVER)
    {
        dump_jobs_struct(out);
        dump_notifies_struct(out);
        dump_conns_struct(out);
    }
}

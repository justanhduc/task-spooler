/*
    Task Spooler PLUS - a multi-user job scheduler like slurm.
    Copyright (C) 2007-2026  Kylin JIANG - Lluís Batlle i Rossell

    Please find the license in the provided COPYING file.
*/
#ifndef ERROR_H
#define ERROR_H

struct Msg;

void error(const char *str, ...);
void warning(const char *str, ...);
void debug(const char *str, ...);
void error_msg(const struct Msg *m, const char *str, ...);
void warning_msg(const struct Msg *m, const char *str, ...);

#endif /* ERROR_H */

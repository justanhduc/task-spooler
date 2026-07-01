/*
    Task Spooler PLUS - a multi-user job scheduler like slurm.
    Copyright (C) 2007-2026  Kylin JIANG - Lluís Batlle i Rossell

    Please find the license in the provided COPYING file.
*/
#ifndef MSGDUMP_H
#define MSGDUMP_H

#include <stdio.h>
#include "msg.h"

void msgdump(FILE *f, const struct Msg *m);

#endif /* MSGDUMP_H */

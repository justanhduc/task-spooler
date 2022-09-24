//
// Created by justanhduc on 24/09/2022.
//

#ifndef TASK_SPOOLER_VERSION_H
#define TASK_SPOOLER_VERSION_H

#ifndef TS_VERSION
#define TS_VERSION 1.4.0
#endif

/* from https://github.com/LLNL/lbann/issues/117
 * and https://gcc.gnu.org/onlinedocs/cpp/Stringizing.html#Stringizing */
#define TS_MAKE_STR(x) _TS_MAKE_STR(x)
#define _TS_MAKE_STR(x) #x

#endif //TASK_SPOOLER_VERSION_H

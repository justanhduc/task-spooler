# Task Spooler PLUS

This project is a fork of Task Spooler by  [Task Spooler by Lluís Batlle i Rossell](https://vicerveza.homeunix.net/~viric/soft/ts/)., a software that offers basic task management. I have enhanced this software with more useful features, such as **multiple user support, fatal crash recovery, and processor allocation and binding.** The aim of this project is to provide <u>an instant, standalone task management system</u>, unlike *SLURM* and *PBS* which have complex installation and dependency issues. This task-spooler-PLUS is suitable for task management on PC and workstation with several to tens of users.

## Introduction 

As a computer scientist, I often need to submit several to tens of simulation tasks on my own workstations and share the computational resources with other users. I tried the original task-spooler software, but it did not support multiple users. Everyone had their own task queue. Therefore, I modified the task-spooler and renamed it as **task-spooler-PLUS** to provide multiple user support. Recently, I also added fatal crash recovery and processor binding features. After a fatal crash, the task-spooler-PLUS can read the data from *Sqlite3* to recover all tasks, including running, queued, and finished ones. The processor binding is done through the *taskset* command. Unlike the original version, the task-spooler-PLUS server needs to run in the background with root privileges.

### Changelog

See [CHANGELOG](CHANGELOG.md).

## Features

I enhanced the Task Spooler to run tasks on my workstation with multiple users. The following are the features of task-spooler-PLUS.

* Task queue management for GNU/Linux, Darwin, Cygwin, and FreeBSD
* Multiple user support with the different limits on the maximum processors usage
* Fatal crush recovery by reading and writing the task log into Sqlite3 database
* Ability To pause and rerun any running or queued task 
* Ability To stop or continue all tasks by a single user
* Good information output (default, json, and tab)
* Easy installation and configuration
* Optional separation of stdout and stderr

## Setup

### Install Task Spooler PLUS

Simple run the provided script

```
./make
```
if you don't need the processors binding feature, try to remove `-DTASKSET` option of `CFLAGS`.

**The default positions** of log file and database is defined in `default.inc`.

```c
#define DEFAULT_USER_PATH "/home/kylin/task-spooler/user.txt"
#define DEFAULT_LOG_PATH "/home/kylin/task-spooler/log.txt"
#define DEFAULT_SQLITE_PATH "/home/kylin/task-spooler/task-spooler.db"
#define DEFAULT_NOTIFICATION_SOUND "/home/kylin/task-spooler/notifications-sound.wav"
#define DEFAULT_ERROR_SOUND "/home/kylin/task-spooler/error.wav"
#define DEFAULT_PULSE_SERVER "unix:/mnt/wslg/PulseServer"
#define DEFAULT_EMAIL_SENDER "kylincaster@foxmail.com"
#define DEFAULT_EMAIL_TIME 45.0%
```

You can specific the positions by the environment variables `TS_USER_PATH`, `TS_LOGFILE_PATH`, and `TS_SQLITE_PATH`, respectively on the invoking of daemon server. Otherwise, you could specify the positions in the `user config` file.



In `taskset.c`, **the sequence of processors binding** is determined by there variables `MAX_CORE_NUM`,`MAX_CORE_NUM_HALF`, and `MAX_CORE_NUM_QUAD`. `MAX_CORE_NUM` defines the total number of processors in your computer.

For a personal laptop with 2 CPU, and each CPU have 4 cores and 8 logical processors, by Hyper-threading. The optimal configuration would be:

```
#define MAX_CORE_NUM 16
#define MAX_CORE_NUM_HALF 8
#define MAX_CORE_NUM_QUAD 4
```

For a AMD workstation with 2 CPU, 128 cores and 256 logical processors. The configuration would be:

```
#define MAX_CORE_NUM 256
#define MAX_CORE_NUM_HALF 128
#define MAX_CORE_NUM_QUAD 64
```

For a Intel workstation with 2 CPU, 128 cores and 128 logical processors. The configuration would be:

```
#define MAX_CORE_NUM 128
#define MAX_CORE_NUM_HALF 128
#define MAX_CORE_NUM_QUAD 64
```

For the other hardware, the sequence of the processors could be specific manually as:

```
static int core_id[MAX_CORE_NUM] = {0, 4, 1, 5, 2, 6, 3, 7}
```



To use `ts` anywhere, `ts`  needs to be added to `$PATH` if it hasn't been done already.
To use `man`, you may also need to add `$HOME/.local/share/man` to `$MANPATH`.

#### Common problems
* Once the suspending of the task-spooler Plus client: try to remove the socket file `/tmp/socket-ts.root` define by `TS_SOCKET` 
* After a fatal crash, the recovered server cannot capture the exit-code and signal of the running task

## User configuration

using the `TS_USER_PATH` environment variable to specify the path to the user configuration. The format of the user file is shown as an example. The UID could be found by `id -u [user]`.

```
# 1231 	# comments
TS_SLOTS = 4 # The number of slots
# uid     name    slots
1000     Kylin    10
3021     test1    10
1001     test0    100
34       user2    30

qweq qweq qweq # error, automatically skipped
```

Note that  the number of slots could be specified in the user configuration file (2nd line).

## Mailing list

I created a GoogleGroup for the program. You look for the archive and the join methods in the taskspooler google group page.

Alessandro Öhler once maintained a mailing list for discussing newer functionalities and interchanging use experiences. I think this doesn't work anymore, but you can look at the old archive or even try to subscribe.

## How it works

The queue is maintained by a server process. This server process is started if it isn't there already. The communication goes through a unix socket usually in /tmp/.

When the user requests a job (using a ts client), the client waits for the server message to know when it can start. When the server allows starting , this client usually forks, and runs the command with the proper environment, because the client runs run the job and not the server, like in 'at' or 'cron'. So, the ulimits, environment, pwd,. apply.

When the job finishes, the client notifies the server. At this time, the server may notify any waiting client, and stores the output and the errorlevel of the finished job.

Moreover the client can take advantage of many information from the server: when a job finishes, where does the job output go to, etc.

## History 

Андрей Пантюхин (Andrew Pantyukhin) maintains the BSD port.

Alessandro Öhler provided a Gentoo ebuild for 0.4, which with simple changes I updated to the ebuild for 0.6.4. Moreover, the Gentoo Project Sunrise already has also an ebuild (maybe old) for ts.

Alexander V. Inyukhin maintains unofficial debian packages for several platforms. Find the official packages in the debian package system.

Pascal Bleser packed the program for SuSE and openSuSE in RPMs for various platforms.

Gnomeye maintains the AUR package.

Eric Keller wrote a nodejs web server showing the status of the task spooler queue (github project).

Duc Nguyen took the project and develops a GPU-support version.

Kylin wrote the multiple user support, fatal crush recovery through Sqlite3 database and processing binding via taskset

## Manual

See below or `man ts` for more details.

```
.1.0 - a task queue system for the unix user.
Copyright (C) 2007-2023  Kylin JIANG - Duc Nguyen - Lluis Batlle i Rossell
usage: ts [action] [-ngfmdE] [-L <lab>] [-D <id>] [cmd...]
Env vars:
  TS_SOCKET  the path to the unix socket used by the ts command.
  TS_MAIL_FROM who send the result mail, default (kylincaster@foxmail.com)
  TS_MAIL_TIME the duration criterion to send a email, default (45.000 sec)
  TS_MAXFINISHED  maximum finished jobs in the queue.
  TS_MAXCONN  maximum number of ts connections at once.
  TS_ONFINISH  binary called on job end (passes jobid, error, outfile, command).
  TS_ENV  command called on enqueue. Its output determines the job information.
  TS_SAVELIST  filename which will store the list, if the server dies.
  TS_SLOTS   amount of jobs which can run at once, read on server start.
  TS_USER_PATH  path to the user configuration file, read on server starts.
  TS_LOGFILE_PATH  path to the job log file, read on server starts
  TS_SQLITE_PATH  path to the job log file, read on server starts
  TS_FIRST_JOBID  The first job ID (default: 1000), read on server starts.
  TS_SORTJOBS  Switch to control the job sequence sort, read on server starts.
  TMPDIR     directory where to place the output files and the default socket.
Long option actions:
  --getenv   [var]                get the value of the specified variable in server environment.
  --setenv   [var]                set the specified flag to server environment.
  --unsetenv   [var]              remove the specified flag from server environment.
  --get_label      || -a [id]     show the job label. Of the last added, if not specified.
  --full_cmd       || -F [id]     show full command. Of the last added, if not specified.
  --check_daemon                  Check the daemon is running or not.  --count_running  || -R          return the number of running jobs
  --last_queue_id  || -q          show the job ID of the last added.
  --get_logdir                    Retrieve the path where log files are stored.
  --set_logdir [path]             Set the path for storing log files.
  --serialize   ||  -M [format]   Serialize the job list to the specified format. Options: {default, json, tab}.
  --daemon                        Run the server as a daemon (Root access only).
  --tmp                           save the logfile to tmp folder
  --hold [jobid]                  Pause a specific task by its job ID.
  --cont [jobid]                  Resume a paused task by its job ID.
  --suspend [user]                For regular users, pause all tasks and lock the user account.
                                For root user, lock all user accounts or a specific user's account.
  --resume [user]                 For regular users, resume all paused tasks and unlock the user account.
                                For root user, unlock all user accounts or a specific user's account.
  --lock                          Lock the server (Timeout: 30 seconds). For root user, there is no timeout.
  --unlock                        Unlock the server.
  --relink [PID]                  Relink running tasks using their [PID] in case of an unexpected failure.
  --job [joibid] || -J [joibid]   set the jobid of the new or relink job
Actions:
  -A           Display information for all users.
  -X           Update user configuration by UID (Max. 100 users, root access only)
  -K           Terminate the task spooler server (root access only)
  -C           Clear the list of finished jobs for the current user.
  -l           Show the job list (default action).
  -S [num]     Get/Set the maximum number of simultaneous server jobs (root access only).
  -t [id]      "tail -n 10 -f" the output of the job. Last run if not specified.
  -c [id]      like -t, but shows all the lines. Last run if not specified.
  -p [id]      show the PID of the job. Last run if not specified.
  -o [id]      show the output file. Of last job run, if not specified.
  -i [id]      show job information. Of last job run, if not specified.
  -s [id]      show the job state. Of the last added, if not specified.
  -r [id]      remove a job. The last added, if not specified.
  -w [id]      wait for a job. The last added, if not specified.
  -k [id]      send SIGTERM to the job process group. The last run, if not specified.
  -T           send SIGTERM to all running job groups.  (only available for root)
  -u [id]      put that job first. The last added, if not specified.
  -U <id-id>   swap two jobs in the queue.
  -h | --help  show this help
  -V           show the program version
Options adding jobs:
  -B           in case of full clients on the server, quit instead of waiting.
  -n           don't store the output of the command.
  -E           Keep stderr apart, in a name like the output file, but adding '.e'.
  -O           Set name of the log file (without any path).
  -z           gzip the stored output (if not -n).
  -f           don't fork into background.
  -m <email>   send the output by e-mail (uses ssmtp).
  -d           the job will be run after the last job ends.
  -D <id,...>  the job will be run after the job of given IDs ends.
  -W <id,...>  the job will be run after the job of given IDs ends well (exit code 0).
  -L [label]   name this task with a label, to be distinguished on listing.
  -N [num]     number of slots required by the job (1 default).
```


## Restore from a fatal crush
Once, the task-spooler-PLUS server is crushed. The service would automatically recover all the tasks. Otherwise, we could do it manually via a automatically python script by `python relink.py`.

```
# relink.py setup
logfile = "/home/kylin/task-spooler/log.txt" # Path to the log file of tasks
days_num = 10 # only tasks starts within [days_num] will be relinked
```
or through the command line as

```
ts -N 10 --relink [pid] task-argv ...
ts -L myjob -N 4 --relink [pid] -J [Jobid] task-argv ...
```

where `[pid]` is the `PID ` of the running task and `[Jobid]` is the specified job id.

**Author**

- Kylin JIANG, gengpingjing@wust.edu.cn

- Duc Nguyen, <adnguyen@yonsei.ac.kr>

- Lluís Batlle i Rossell, <lluis@vicerveza.homeunix.net>

  

**Acknowledgement**

* To Raúl Salinas, for his inspiring ideas
* To Alessandro Öhler, the first non-acquaintance user, who proposed and created the mailing list.
* Андрею Пантюхину, who created the BSD port.
* To the useful, although sometimes uncomfortable, UNIX interface.
* To Alexander V. Inyukhin, for the debian packages.
* To Pascal Bleser, for the SuSE packages.
* To Sergio Ballestrero, who sent code and motivated the development of a multislot version of ts.
* To Duc Nguyen, for his faithful working on GPU versions
* To GNU, an ugly but working and helpful ol' UNIX implementation. 

**Software**

Memory checks with [Valgrind](https://valgrind.org/).

## Related projects

[Messenger](https://github.com/justanhduc/messenger)

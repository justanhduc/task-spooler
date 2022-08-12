# Task Spooler

Originally, [Task Spooler by Lluís Batlle i Rossell](https://vicerveza.homeunix.net/~viric/soft/ts/). I focked the task spooler and add the function to run is with multiple users.

## Introduction 

As in freshmeat.net:

> task spooler is a Unix batch system where the tasks spooled run one after the other. The amount of jobs to run at once can be set at any time. Each user in each system has his own job queue. The tasks are run in the correct context (that of enqueue) from any shell/process, and its output/results can be easily watched. It is very useful when you know that your commands depend on a lot of RAM, a lot of disk use, give a lot of output, or for whatever reason it's better not to run them all at the same time, while you want to keep your resources busy for maximum benfit. Its interface allows using it easily in scripts. 

For your first contact, you can read an article at linux.com, 
which I like as overview, guide and examples (original url). 
On more advanced usage, don't neglect the TRICKS file in the package.

### Changelog

See [CHANGELOG](CHANGELOG.md).

## Tutorial

A tutorial with colab is available [here](https://librecv.github.io/blog/spooler/task%20manager/deep%20learning/2021/02/09/task-spooler.html).

## Features

I wrote Task Spooler because I didn't have any comfortable way of running batch jobs in my linux computer. I wanted to:

* Queue jobs from different terminals.
* Use it locally in my machine (not as in network queues).
* Have a good way of seeing the output of the processes (tail, errorlevels, ...).
* Easy use: almost no configuration.
* Easy to use in scripts. 

At the end, after some time using and developing ts, it can do something more:

* It works in most systems I use and some others, like GNU/Linux, Darwin, Cygwin, and FreeBSD.
* No configuration at all for a simple queue.
* Good integration with renice, kill, etc. (through `ts -p` and process groups).
* Have any amount of queues identified by name, writting a simple wrapper script for each (I use ts2, tsio, tsprint, etc).
* Control how many jobs may run at once in any queue (taking profit of multicores).
* It never removes the result files, so they can be reached even after we've lost the ts task list.
* Transparent if used as a subprogram with -nf.
* Optional separation of stdout and stderr. 

![ts-sample](assets/sample.png)

## Setup

### Install Task Spooler

Simple run the provided script

```
./install_cmake
```
to use CMake, or 
```
./install_make
```
to use Makefile. If Task Spooler has already been installed, and you want to reinstall, execute 

```
./reinstall
```

#### Local installation
To install without sudo privilege, one can use the following command
```
make install-local
```

Note that, the installation will create a `bin` folder in `$HOME` if it does not exist. 
To use `ts` anywhere, `$HOME/bin` needs to be added to `$PATH` if it hasn't been done already.
To use `man`, you may also need to add `$HOME/.local/share/man` to `$MANPATH`.

#### Common problems
* Cannot find CUDA: Did you set a `CUDA_HOME` flag?
* `/usr/bin/ld: cannot find -lnvidia-ml`: This lib lies in `$CUDA_HOME/lib64/stubs`. 
Please append this path to `LD_LIBRARY_PATH`.
Sometimes, this problem persists even after adding the lib path.
Then one can add `-L$(CUDA_HOME)/lib64/stubs` to [this line](./Makefile#L29) in the Makefile.
* list.c:22:5: error: implicitly declaring library function 'snprintf' with type 'int (char *, unsigned long, const char *, ...)': Please remove `-D_XOPEN_SOURCE=500 -D__STRICT_ANSI__` in the Makefile as reported [here](https://github.com/justanhduc/task-spooler/issues/4).


### Uinstall Task Spooler

```
./uninstall
```
Why would you want to do that anyway?

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

## Manual

See below or `man ts` for more details.

```
usage: ts [action] [-ngfmdE] [-L <lab>] [-D <id>] [cmd...]
Env vars:
  TS_SOCKET  the path to the unix socket used by the ts command.
  TS_MAILTO  where to mail the result (on -m). Local user by default.
  TS_MAXFINISHED  maximum finished jobs in the queue.
  TS_MAXCONN  maximum number of ts connections at once.
  TS_ONFINISH  binary called on job end (passes jobid, error, outfile, command).
  TS_ENV  command called on enqueue. Its output determines the job information.
  TS_SAVELIST  filename which will store the list, if the server dies.
  TS_SLOTS   amount of jobs which can run at once, read on server start.
  TS_USER_PATH  path to the user configuration file, read on server starts.
  TS_LOGFILE_PATH  path to the job log file, read on server starts
  TS_FIRST_JOBID  The first job ID (default: 1000), read on server starts.
  TS_SORTJOBS  Switch to control the job sequence sort, read on server starts.
  TMPDIR     directory where to place the output files and the default socket.
Long option actions:
  --getenv   [var]                get the value of the specified variable in server environment.
  --setenv   [var]                set the specified flag to server environment.
  --unsetenv   [var]              remove the specified flag from server environment.
  --get_label      || -a [id]     show the job label. Of the last added, if not specified.
  --full_cmd       || -F [id]     show full command. Of the last added, if not specified.
  --count_running  || -R          return the number of running jobs
  --last_queue_id  || -q          show the job ID of the last added.
  --get_logdir                    get the path containing log files.
  --set_logdir [path]             set the path containing log files.
  --plain                         list jobs in plain tab-separated texts.
  --hold_job [jobid]              hold on a task.
  --restart_job [jobid]           restart a task.
  --lock                          Locker the server (Timeout: 30 sec.)git For Root, timeout is infinity.
  --unlock                        Unlocker the server.
  --stop [user]                   For normal user, pause all tasks and lock the account.
                                  For root, to lock all users or single [user].
  --cont [user]                   For normal user, continue all paused tasks and lock the account.
                                  For root, to unlock all users or single [user].
Actions:
  -A           Show all users information
  -X           Refresh the user configuration (only available for root)
  -K           kill the task spooler server (only available for root)
  -C           clear the list of finished jobs for current user
  -l           show the job list (default action)
  -S [num]     get/set the number of max simultaneous jobs of the server.  (only available for root)
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
  -m           send the output by e-mail (uses sendmail).
  -d           the job will be run after the last job ends.
  -D <id,...>  the job will be run after the job of given IDs ends.
  -W <id,...>  the job will be run after the job of given IDs ends well (exit code 0).
  -L [label]   name this task with a label, to be distinguished on listing.
  -N [num]     number of slots required by the job (1 default).
```

## Thanks

**Author**
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
* To GNU, an ugly but working and helpful ol' UNIX implementation. 

**Software**

Memory checks with [Valgrind](https://valgrind.org/).

## Related projects

[Messenger](https://github.com/justanhduc/messenger)

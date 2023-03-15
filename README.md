# GPU Task Spooler

## Contents

* [About](https://github.com/justanhduc/task-spooler#about)
* [Setup](https://github.com/justanhduc/task-spooler#setup)
* [Changelog](https://github.com/justanhduc/task-spooler#changelog)
* [Tutorial](https://github.com/justanhduc/task-spooler#tutorial)
* [Tricks](https://github.com/justanhduc/task-spooler#tricks)
* [Manual](https://github.com/justanhduc/task-spooler#manual)
* [People](https://github.com/justanhduc/task-spooler#people)
* [Related projects](https://github.com/justanhduc/task-spooler#related-projects)

## About

**GPU Task Spooler**, or `ts` for short, is a spooling system that helps manage CPU/GPU tasks easily.
You can think of [SLURM](https://slurm.schedmd.com/elastic_computing.html) but for small individual servers rather 
than high-performance clusters.

### Features
`ts` can offer you the following features

* Queue and execute jobs, be it on CPUs or GPUs.
* Automatically allocate free GPUs for your jobs: 
just forget `CUDA_VISIBLE_DEVICES`.
* Control number of jobs running in parallel.
* View your job outputs in terminal and/or `txt`.
* Very minimalistic: easy setup and almost no configuration.
* Terminal agnostic: queue in one terminal and view in another.
* Jobs can be set to run in foreground or background.
* Simple CLI, but there is a [GUI addon]().


## Setup

See the installation steps in [INSTALL.md](INSTALL.md).

## Changelog

See [CHANGELOG](CHANGELOG.md).

## Tutorial

A tutorial with colab is available [here](https://librecv.github.io/blog/spooler/task%20manager/deep%20learning/2021/02/09/task-spooler.html).

## Tricks

See [here](TRICKS.md) for some cool tricks to extend `ts`.

### A note for DL/ML researchers

If the codes are modified after a job is queued,
the modified version will be executed rather than the version at the time the job is queued.
To ensure the right version of the codes is executed, it is necessary to use a versioning mechanism.
Personally, I simply clone the whole code base excluding binary files to a temporary location with `rsync`
and execute the job there.
Another way is to use git to check out the right version before running, 
but it requires committing every small changes.

#### Working with remote servers

Like above, one can use `rsync` to copy the code base to a temporary location on the remote server, 
and use `ssh` to launch the job using `ts`.
This can be done either with a script or using a small plug-in [here](https://github.com/justanhduc/messenger).

## Manual

See below/`man ts`/`ts -h` for more details.

```
usage: ts [action] [-ngfmdE] [-L <lab>] [-D <id>] [cmd...]
Env vars:
  TS_VISIBLE_DEVICES     the GPU IDs that are visible to ts. Jobs will be run on these GPUs only.
  TS_SOCKET              the path to the unix socket used by the ts command.
  TS_MAILTO              where to mail the result (on -m). Local user by default.
  TS_MAXFINISHED         maximum finished jobs in the queue.
  TS_MAXCONN             maximum number of ts connections at once.
  TS_ONFINISH            binary called on job end (passes jobid, error, outfile, command).
  TS_ENV                 command called on enqueue. Its output determines the job information.
  TS_SAVELIST            filename which will store the list, if the server dies.
  TS_SLOTS               amount of jobs which can run at once, read on server start.
  TMPDIR                 directory where to place the output files and the default socket.
Long option actions:
  --getenv               [var]        get the value of the specified variable in server environment.
  --setenv               [var]        set the specified flag to server environment.
  --unsetenv             [var]        remove the specified flag from server environment.
  --set_gpu_free_perc    [num]        set the value of GPU memory threshold above which GPUs are considered available (90 by default).
  --get_gpu_free_perc                 get the value of GPU memory threshold above which GPUs are considered available.
  --get_label          || -a [id]     show the job label. Of the last added, if not specified.
  --full_cmd           || -F [id]     show full command. Of the last added, if not specified.
  --count_running      || -R          return the number of running jobs
  --last_queue_id      || -q          show the job ID of the last added.
  --get_logdir                        get the path containing log files.
  --set_logdir           [path]       set the path containing log files. 
  --serialize [format] || -M [format] serialize the job list to the specified format. Choices: {default, json, tab}.
Long option adding jobs:
  --gpus               || -G [num]    number of GPUs required by the job (1 default).
  --gpu_indices        || -g [id,...] the job will be on these GPU indices without checking whether they are free.
Actions (can be performed only one at a time):
  -K           kill the task spooler server
  -C           clear the list of finished jobs
  -l           show the job list (default action)
  -g           list all jobs running on GPUs and the corresponding GPU IDs
  -S [num]     get/set the number of max simultaneous jobs of the server.
  -t [id]      \"tail -n 10 -f\" the output of the job. Last run if not specified.
  -c [id]      like -t, but shows all the lines. Last run if not specified.
  -p [id]      show the pid of the job. Last run if not specified.
  -o [id]      show the output file. Of last job run, if not specified.
  -i [id]      show job information. Of last job run, if not specified.
  -s [id]      show the job state. Of the last added, if not specified.
  -r [id]      remove a job. The last added, if not specified.
  -w [id]      wait for a job. The last added, if not specified.
  -k [id]      send SIGTERM to the job process group. The last run, if not specified.
  -T           send SIGTERM to all running job groups.
  -u [id]      put that job first. The last added, if not specified.
  -U [id-id]   swap two jobs in the queue.
  -B           in case of full queue on the server, quit (2) instead of waiting.
  -h           show this help
  -V           show the program version
Options adding jobs:
  -n           don't store the output of the command.
  -E           Keep stderr apart, in a name like the output file, but adding '.e'.
  -O           Set name of the log file (without any path).
  -z           gzip the stored output (if not -n).
  -f           don't fork into background.
  -m           send the output by e-mail (uses sendmail).
  -d           the job will be run after the last job ends.
  -D [id,...]  the job will be run after the job of given IDs ends.
  -W [id,...]  the job will be run after the job of given IDs ends well (exit code 0).
  -L [lab]     name this task with a label, to be distinguished on listing.
  -N [num]     number of slots required by the job (1 default).
```

## People

<!-- ALL-CONTRIBUTORS-LIST:START - Do not remove or modify this section -->
<!-- prettier-ignore-start -->
<!-- markdownlint-disable -->


<!-- markdownlint-restore -->
<!-- prettier-ignore-end -->

<!-- ALL-CONTRIBUTORS-LIST:END -->

**Acknowledgement**
* To Lluís Batlle i Rossell, the author of the original Task Spooler
* To Raúl Salinas, for his inspiring ideas
* To Alessandro Öhler, the first non-acquaintance user, who proposed and created the mailing list
* To Андрею Пантюхину, who created the BSD port
* To the useful, although sometimes uncomfortable, UNIX interface
* To Alexander V. Inyukhin, for the debian packages
* To Pascal Bleser, for the SuSE packages
* To Sergio Ballestrero, who sent code and motivated the development of a multislot version of ts
* To GNU, an ugly but working and helpful ol' UNIX implementation

**Others**

Many memory bugs are identified thanks to [Valgrind](https://valgrind.org/).

## Related projects

[Messenger](https://github.com/justanhduc/messenger)
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

## Feishu notifications

This fork can notify a Feishu custom bot whenever a job is accepted, starts,
succeeds, or fails. CPU and GPU jobs use the same notification path.

```bash
# Configure the running queue (replace the placeholder with your bot's URL).
ts --set_feishu_webhook 'https://open.feishu.cn/open-apis/bot/v2/hook/YOUR_TOKEN'
ts --get_feishu_webhook

# Submit jobs normally; no per-job notification flag is required.
ts -G 2 -L evaluation python eval.py
ts -G 4 bash -lc 'cd /data/project && python eval.py'

# Disable notifications for future events.
ts --unset_feishu_webhook
```

The configuration belongs to the **server/queue**, not the submitting terminal.
All users sharing a queue must use the same `TS_SOCKET` and have access to that
socket. These options do not merge separate users' default queues or change
scheduling policy. They reuse the existing server environment protocol; restart
any older running `ts` server after installing this version.

CLI settings last until the queue server exits. To enable notifications after
every restart, export `TS_FEISHU_WEBHOOK` in the environment that starts the server
(for example, in its service configuration). Exporting it in a new shell does
not update a server that is already running; use `--set_feishu_webhook` instead.
`--get_feishu_webhook` prints the full configured URL; an empty line means disabled.

Messages are Feishu JSON 2.0 cards with rendered Markdown and Chinese status
headings. Blue/cyan mark submission/start, green marks success, red marks failure,
and orange marks skipped jobs. Labels, resource details, results, shell command
code blocks and log paths are arranged in separate sections. Cards include the
host, job ID, event time (with timezone), requested GPU count, allocated GPU IDs,
PID, exit status, termination signal and elapsed seconds as applicable. A job skipped
because of `-W` dependencies produces `submitted` and `skipped`, without a false
`running` event. A normal job produces exactly three events:
`submitted`, `running`, and either `succeeded` or `failed`.

A single background thread sends messages in event order. Sending uses libcurl
with a 2-second connection timeout and a 5-second total timeout per message;
network latency and bot errors do not block scheduling or change job exit codes.
HTTP errors, nonzero Feishu response codes, and malformed responses are logged
to the queue socket path plus `.error` (for example, `$TS_SOCKET.error`). Webhook
URLs and response bodies are not included in notification error messages.

Delivery is best effort: there is no retry (to avoid duplicate bot messages),
the in-memory queue holds at most 256 waiting notifications, and overflow is
logged. A graceful `ts -K` drains pending messages before the server exits, which
may take time if the endpoint is unavailable. Abrupt termination can lose pending
messages. Disabling or replacing the webhook affects future events; messages
already waiting retain their original destination.

Use an HTTPS webhook. HTTP loopback URLs are also accepted for local tests.
Bots with keyword restrictions must allow `Task Spooler`. Bot signature-secret
verification is not implemented; this integration uses the webhook token.

Build dependencies include libcurl development headers and POSIX threads; see
[INSTALL.md](INSTALL.md). Run the local HTTP integration tests with:

```bash
cmake -S . -B build -DTASK_SPOOLER_COMPILE_CUDA=OFF
cmake --build build
TS_BINARY=./build/ts python3 -m unittest discover -s tests -v
```

## Queue priority

```bash
ts -u 12  # Make waiting job 12 the first waiting job.
ts -u     # Promote the last submitted job that is still in the active queue.
```

Promotion places the selected waiting job after all running jobs and before
all other waiting jobs. With no running jobs, it becomes the queue head, even
if the old head is waiting for GPUs. Other running and waiting jobs retain
their relative order within each group. Running jobs are never interrupted.
Promoting the waiting head again (including a queue with just one waiting job)
is a safe no-op. A running, finished, unknown, or not-yet-admitted job returns
an error without changing the queue. Omitting the ID selects the most recently
submitted active job, even after previous queue reordering.

Promotion changes priority only: GPU availability, execution slots and job
dependencies still determine whether the task can start. Existing servers
must be restarted after their queues drain to pick up this fix; changing the
binary does not update a running server's in-memory queue.

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
  -u [id]      promote a waiting job ahead of other waiting jobs, after running jobs. Last added by default.
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

### Queue on selected GPUs

Use `--gpu-ids` to restrict a job to a comma-separated set of physical NVML GPU indices while retaining GPU memory checks and the queue's GPU reservations. `-G` is the number of GPUs needed from that set; it defaults to one when `--gpu-ids` is given alone.

```bash
# Wait for GPU 1, even if another GPU is free.
ts -G 1 --gpu-ids 1 -L '明-model-eval' python eval.py
# Wait for either GPU 0 or GPU 1; never use another device.
ts -G 1 --gpu-ids 0,1 -L '明-model-eval' python eval.py
# Require both GPUs to be available at the same time.
ts -G 2 --gpu-ids 0,1 -L '明-model-eval' python eval.py
```

A restricted job stays `allocating` until enough GPUs in its set are visible to the server, free according to the configured memory threshold, and unreserved by other jobs. Other jobs can still use GPUs outside that set. Dependencies and slot limits continue to apply. Invalid list syntax, duplicate IDs, and requesting more GPUs than the list contains are rejected; nonexistent or server-hidden GPU IDs cannot be allocated and may leave the job waiting. `TS_VISIBLE_DEVICES` remains a server-wide limit, and the job's allowed set cannot override it.

`ts -i ID` and `ts -M json` expose the allowed set separately from actual assignments. Task Spooler sets `CUDA_VISIBLE_DEVICES` for the allocated physical devices; applications should use the resulting logical device IDs rather than override that variable. The legacy `-g/--gpu_indices` option still bypasses availability checks and cannot be combined with `--gpu-ids`.

Feishu cards show candidate GPU IDs even while queued, followed by the actual allocation once running. Event times use the server's local timezone and `YYYY-MM-DD HH:MM`. Durations display minutes/seconds below one hour, hours/minutes below one day, and days/hours/minutes for longer jobs.

This feature uses protocol 731. Update the client and server together after draining an existing queue; clients reject a server with the old protocol rather than misreading GPU restrictions. GPU scheduling regression tests use a fake NVML backend and isolated sockets, without reserving real GPUs or sending notifications to Feishu:

```bash
cmake -S . -B build -DTASK_SPOOLER_COMPILE_CUDA=OFF -DBUILD_TESTING=ON
cmake --build build -j
ctest --test-dir build --output-on-failure
```

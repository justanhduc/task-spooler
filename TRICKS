System wide queue
-------------------------
You can set the $TS_SOCKET variable to the same name path for all your
'ts' processes, and then
they'll use the same socket for intercommunication. This means - single queue.
You should be certain that any 'ts' can read/write to that socket, using
chmod.


A queue for each resource
-------------------------
You can use $TS_SOCKET and aliases/scripts for having different queues for
different resources. For instance, using bash:
alias tsdisk='TS_SOCKET=/tmp/socket.disk ts'
alias tsram='TS_SOCKET=/tmp/socket.ram ts'
alias tsnet='TS_SOCKET=/tmp/socket.net ts'

You can also create shell scripts like this:
<< FILE ts2
#!/bin/sh
export TS_SOCKET=/tmp/socket-ts2.$USER
ts "$@"
>> END OF FILE ts2


Be notified of a task finished
-------------------------
In X windows, inside bash, after submitting the task, I use:
$ ( ts -w ; xmessage Finished! ) &


Killing process groups
-------------------------
ts creates a new session for the job, so the pid of the command run can be
used as the process group id for the command and its childs. So, you can use
something like:
$ kill -- -`ts -p`
in order to kill the job started and all its childs. I find it useful when
killing 'make's.


Limiting the number of ts processes
-------------------------
Each queued job remains in the system as a waiting process. On environments
where the number of processes is quite limited, the user can select the amount
of the maximum number of ts server connections to ts clients. That will be
read from the environment variable TS_MAXCONN at the server start, and cannot be
set again once the server runs:
$ ts -K     # we assure we will start the server at the next ts call
$ TS_MAXCONN=5 ts
Internally there is a maximum of 1000 connexions that cannot be exceeded without
modifying the source code (server.c).

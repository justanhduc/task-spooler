/*
    Task Spooler - a task queue system for the unix user
    Copyright (C) 2007-2013  Lluís Batlle i Rossell

    Please find the license in the provided COPYING file.
*/
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#ifdef linux
  #include <sys/time.h>
#endif
#include <sys/resource.h>
#include <sys/un.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <limits.h>
#include <signal.h>
#include <fcntl.h>
#include <libgen.h>

#include <stdio.h>

#include "main.h"

enum
{
    MAXCONN=1000
};

enum Break
{
    BREAK,
    NOBREAK,
    CLOSE
};

/* Prototypes */
static void server_loop(int ls);
static enum Break
    client_read(int index);
static void end_server(int ls);
static void s_newjob_ok(int index);
static void s_newjob_nok(int index);
static void s_runjob(int jobid, int index);
static void clean_after_client_disappeared(int socket, int index);

struct Client_conn
{
    int socket;
    int hasjob;
    int jobid;
};

/* Globals */
static struct Client_conn client_cs[MAXCONN];
static int nconnections;
static char *path;
static int max_descriptors;

/* in jobs.c */
extern int max_jobs;

static void s_send_version(int s)
{
    struct msg m;

    m.type = VERSION;
    m.u.version = PROTOCOL_VERSION;

    send_msg(s, &m);
}

static void sigterm_handler(int n)
{
    const char *dumpfilename;
    int fd;

    /* Dump the job list if we should to */
    dumpfilename = getenv("TS_SAVELIST");
    if (dumpfilename != NULL)
    {
        fd = open(dumpfilename, O_WRONLY | O_APPEND | O_CREAT, 0600);
        if (fd != -1)
        {
            joblist_dump(fd);
            close(fd);
        } else
            warning("The TS_SAVELIST file \"%s\" cannot be opened",
                    dumpfilename);
    }

    /* path will be initialized for sure, before installing the handler */
    unlink(path);
    exit(1);
}

static void set_default_maxslots()
{
    char *str;

    str = getenv("TS_SLOTS");
    if (str != NULL)
    {
        int slots;
        slots = abs(atoi(str));
        s_set_max_slots(slots);
    }
}

static void install_sigterm_handler()
{
  struct sigaction act;

  act.sa_handler = sigterm_handler;
  /* Reset the mask */
  memset(&act.sa_mask,0,sizeof(act.sa_mask));
  act.sa_flags = 0;

  sigaction(SIGTERM, &act, NULL);
}

static int get_max_descriptors()
{
    const int MARGIN = 5; /* stdin, stderr, listen socket, and whatever */
    int max;
    struct rlimit rlim;
    int res;
    const char *str;

    max = MAXCONN;

    str = getenv("TS_MAXCONN");
    if (str != NULL)
    {
        int user_maxconn;
        user_maxconn = abs(atoi(str));
        if (max > user_maxconn)
            max = user_maxconn;
    }

    if (max > FD_SETSIZE)
        max = FD_SETSIZE - MARGIN;

    /* I'd like to use OPEN_MAX or NR_OPEN, but I don't know if any
     * of them is POSIX compliant */

    res = getrlimit(RLIMIT_NOFILE, &rlim);
    if (res != 0)
        warning("getrlimit for open files");
    else
    {
        if (max > rlim.rlim_cur)
            max = rlim.rlim_cur - MARGIN;
    }

    if (max < 1)
        error("Too few opened descriptors available");

    return max;
}

void server_main(int notify_fd, char *_path)
{
    int ls;
    struct sockaddr_un addr;
    int res;
    char *dirpath;

    process_type = SERVER;
    max_descriptors = get_max_descriptors();

    /* Arbitrary limit, that will block the enqueuing, but should allow space
     * for usual ts queries */
    max_jobs = max_descriptors - 5;

    path = _path;

    /* Move the server to the socket directory */
    dirpath = malloc(strlen(path)+1);
    strcpy(dirpath, path);
    chdir(dirname(dirpath));
    free(dirpath);

    nconnections = 0;

    ls = socket(AF_UNIX, SOCK_STREAM, 0);
    if(ls == -1)
        error("cannot create the listen socket in the server");

    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, path);

    res = bind(ls, (struct sockaddr *) &addr, sizeof(addr));
    if (res == -1)
        error("Error binding.");

    res = listen(ls, 0);
    if (res == -1)
        error("Error listening.");

    install_sigterm_handler();

    set_default_maxslots();

    notify_parent(notify_fd);

    server_loop(ls);
}

static int get_conn_of_jobid(int jobid)
{
    int i;
    for(i=0; i< nconnections; ++i)
        if (client_cs[i].hasjob && client_cs[i].jobid == jobid)
            return i;
    return -1;
}

static void server_loop(int ls)
{
    fd_set readset;
    int i;
    int maxfd;
    int keep_loop = 1;
    int newjob;

    while (keep_loop)
    {
        FD_ZERO(&readset);
        maxfd = 0;
        /* If we can accept more connections, go on.
         * Otherwise, the system block them (no accept will be done). */
        if (nconnections < max_descriptors)
        {
            FD_SET(ls,&readset);
            maxfd = ls;
        }
        for(i=0; i< nconnections; ++i)
        {
            FD_SET(client_cs[i].socket, &readset);
            if (client_cs[i].socket > maxfd)
                maxfd = client_cs[i].socket;
        }
        select(maxfd + 1, &readset, NULL, NULL, NULL);
        if (FD_ISSET(ls,&readset))
        {
            int cs;
            cs = accept(ls, NULL, NULL);
            if (cs == -1)
                error("Accepting from %i", ls);
            client_cs[nconnections].hasjob = 0;
            client_cs[nconnections].socket = cs;
            ++nconnections;
        }
        for(i=0; i< nconnections; ++i)
            if (FD_ISSET(client_cs[i].socket, &readset))
            {
                enum Break b;
                b = client_read(i);
                /* Check if we should break */
                if (b == CLOSE)
                {
                    warning("Closing");
                    /* On unknown message, we close the client,
                       or it may hang waiting for an answer */
                    clean_after_client_disappeared(client_cs[i].socket, i);
                }
                else if (b == BREAK)
                    keep_loop = 0;
            }
        /* This will return firstjob->jobid or -1 */
        newjob = next_run_job();
        if (newjob != -1)
        {
            int conn, awaken_job;
            conn = get_conn_of_jobid(newjob);
            /* This next marks the firstjob state to RUNNING */
            s_mark_job_running(newjob);
            s_runjob(newjob, conn);

            while ((awaken_job = wake_hold_client()) != -1)
            {
                int wake_conn = get_conn_of_jobid(awaken_job);
                if (wake_conn == -1)
                    error("The job awaken does not have a connection open");
                s_newjob_ok(wake_conn);
            }
        }
    }

    end_server(ls);
}

static void end_server(int ls)
{
    close(ls);
    unlink(path);
    /* This comes from the parent, in the fork after server_main.
     * This is the last use of path in this process.*/
    free(path); 
}

static void remove_connection(int index)
{
    int i;

    if(client_cs[index].hasjob)
    {
        s_removejob(client_cs[index].jobid);
    }

    for(i=index; i<(nconnections-1); ++i)
    {
        memcpy(&client_cs[i], &client_cs[i+1], sizeof(client_cs[0]));
    }
    nconnections--;
}

static void
clean_after_client_disappeared(int socket, int index)
{
    /* Act as if the job ended. */
    int jobid = client_cs[index].jobid;
    if (client_cs[index].hasjob)
    {
        struct Result r;

        r.errorlevel = -1;
        r.died_by_signal = 1;
        r.signal = SIGKILL;
        r.user_ms = 0;
        r.system_ms = 0;
        r.real_ms = 0;
        r.skipped = 0;

        warning("JobID %i quit while running.", jobid);
        job_finished(&r, jobid);
        /* For the dependencies */
        check_notify_list(jobid);
        /* We don't want this connection to do anything
         * more related to the jobid, secially on remove_connection
         * when we receive the EOC. */
        client_cs[index].hasjob = 0;
    }
    else
        /* If it doesn't have a running job,
         * it may well be a notification */
        s_remove_notification(socket);

    close(socket);
    remove_connection(index);
}

static enum Break
    client_read(int index)
{
    struct msg m;
    int s;
    int res;

    s = client_cs[index].socket;

    /* Read the message */
    res = recv_msg(s, &m);
    if (res == -1)
    {
        warning("client recv failed");
        clean_after_client_disappeared(s, index);
        return NOBREAK;
    }
    else if (res == 0)
    {
        clean_after_client_disappeared(s, index);
        return NOBREAK;
    }

    /* Process message */
    switch(m.type)
    {
        case KILL_SERVER:
            return BREAK; /* break in the parent*/
            break;
        case NEWJOB:
            client_cs[index].jobid = s_newjob(s, &m);
            client_cs[index].hasjob = 1;
            if (!job_is_holding_client(client_cs[index].jobid))
                s_newjob_ok(index);
            else if (!m.u.newjob.wait_enqueuing)
            {
                s_newjob_nok(index);
                clean_after_client_disappeared(s, index);
            }
            break;
        case RUNJOB_OK:
            {
                char *buffer = 0;
                if (m.u.output.store_output)
                {
                    /* Receive the output filename */
                    buffer = (char *) malloc(m.u.output.ofilename_size);
                    res = recv_bytes(s, buffer,
                        m.u.output.ofilename_size);
                    if (res != m.u.output.ofilename_size)
                        error("Reading the ofilename");
                }
                s_process_runjob_ok(client_cs[index].jobid, buffer,
                        m.u.output.pid);
            }
            break;
        case LIST:
            s_list(s);
            /* We must actively close, meaning End of Lines */
            close(s);
            remove_connection(index);
            break;
        case INFO:
            s_job_info(s, m.u.jobid);
            close(s);
            remove_connection(index);
            break;
        case ENDJOB:
            job_finished(&m.u.result, client_cs[index].jobid);
            /* For the dependencies */
            check_notify_list(client_cs[index].jobid);
            /* We don't want this connection to do anything
             * more related to the jobid, secially on remove_connection
             * when we receive the EOC. */
            client_cs[index].hasjob = 0;
            break;
        case CLEAR_FINISHED:
            s_clear_finished();
            break;
        case ASK_OUTPUT:
            s_send_output(s, m.u.jobid);
            break;
        case REMOVEJOB:
            {
                int went_ok;
                /* Will update the jobid. If it's -1, will set the jobid found */
                went_ok = s_remove_job(s, &m.u.jobid);
                if (went_ok)
                {
                    int i;
                    for(i = 0; i < nconnections; ++i)
                    {
                        if (client_cs[i].hasjob && client_cs[i].jobid == m.u.jobid)
                        {
                            close(client_cs[i].socket);

                            /* So remove_connection doesn't call s_removejob again */
                            client_cs[i].hasjob = 0;

                            /* We don't try to remove any notification related to
                             * 'i', because it will be for sure a ts client for a job */
                            remove_connection(i);
                        }
                    }
                }
            }
            break;
        case WAITJOB:
            s_wait_job(s, m.u.jobid);
            break;
        case WAIT_RUNNING_JOB:
            s_wait_running_job(s, m.u.jobid);
            break;
        case URGENT:
            s_move_urgent(s, m.u.jobid);
            break;
        case SET_MAX_SLOTS:
            s_set_max_slots(m.u.max_slots);
            break;
        case GET_MAX_SLOTS:
            s_get_max_slots(s);
            break;
        case SWAP_JOBS:
            s_swap_jobs(s, m.u.swap.jobid1,
                    m.u.swap.jobid2);
            break;
        case GET_STATE:
            s_send_state(s, m.u.jobid);
            break;
        case GET_VERSION:
            s_send_version(s);
            break;
        default:
            /* Command not supported */
            /* On unknown message, we close the client,
               or it may hang waiting for an answer */
            warning("Unknown message: %i", m.type);
            return CLOSE;
    }

    return NOBREAK; /* normal */
}

static void s_runjob(int jobid, int index)
{
    int s;
    
    if (!client_cs[index].hasjob)
        error("Run job of the client %i which doesn't have any job", index);

    s = client_cs[index].socket;

    s_send_runjob(s, jobid);
}

static void s_newjob_ok(int index)
{
    int s;
    struct msg m;
    
    if (!client_cs[index].hasjob)
        error("Run job of the client %i which doesn't have any job", index);

    s = client_cs[index].socket;

    m.type = NEWJOB_OK;
    m.u.jobid = client_cs[index].jobid;

    send_msg(s, &m);
}

static void s_newjob_nok(int index)
{
    int s;
    struct msg m;
    
    if (!client_cs[index].hasjob)
        error("Run job of the client %i which doesn't have any job", index);

    s = client_cs[index].socket;

    m.type = NEWJOB_NOK;

    send_msg(s, &m);
}

static void dump_conn_struct(FILE *out, const struct Client_conn *p)
{
    fprintf(out, "  new_conn\n");
    fprintf(out, "    socket %i\n", p->socket);
    fprintf(out, "    hasjob \"%i\"\n", p->hasjob);
    fprintf(out, "    jobid %i\n", p->jobid);
}

void dump_conns_struct(FILE *out)
{
    int i;

    fprintf(out, "New_conns");

    for(i=0; i < nconnections; ++i)
    {
        dump_conn_struct(out, &client_cs[i]);
    }
}

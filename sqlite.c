#include <sqlite3.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "main.h"
#include "default.inc"

sqlite3 *db = NULL;

const char *get_sqlite_path() {
  char *str;
  str = getenv("TS_SQLITE_PATH");
  if (str == NULL || strlen(str) == 0) {
    return DEFAULT_SQLITE_PATH;
  } else {
    return str;
  }
}


static int callback(void *max, int argc, char **argv, char **azColName) {
    if (argv[0]) {
        *(int*)max = atoi(argv[0]);
    } else {
        *(int*)max = 0;
    }
    return 0;
}

static int check_order_id(const char* op) {
    char sql[100];
    char *err_msg = NULL;
    sprintf(sql, "SELECT %s(order_id) FROM Jobs", op);
    int value = 0;
    int rc = sqlite3_exec(db, sql, callback, &value, &err_msg);
    if (rc != SQLITE_OK ) {
        fprintf(stderr, "[check_order_id] SQL error: %s, sql: %s\n",
            sql, err_msg);
        sqlite3_free(err_msg);
    }
    return value;
}

static int max_order_id() {
    return check_order_id("MAX");
}

static int min_order_id() {
    return check_order_id("MIN");
}

static int get_order_id(int jobid) {
    char *err_msg = 0;
    char sql[1024];
    int value = 0;
    sprintf(sql, "SELECT order_id FROM Jobs WHERE jobid=%d", jobid);
    int rc = sqlite3_exec(db, sql, callback, &value, &err_msg);
    if (rc != SQLITE_OK ) {
        fprintf(stderr, "[get_order_id] SQL error: %s\n", err_msg);
        sqlite3_free(err_msg);
        return 0;
    }
    return value;
}

void close_sqlite() {
    // free(jobDB_Jobs);
    sqlite3_close(db);
}


int open_sqlite() {
    const char* path = get_sqlite_path();
    char *zErrMsg = 0;
    int rc;
    rc = sqlite3_open(path, &db);

    if (rc) {
        printf("Can't open database: %s\n", sqlite3_errmsg(db));
        sqlite3_close(db);
        return(1);
    }

  char *sql = "CREATE TABLE IF NOT EXISTS Jobs(" \
               "jobid INT PRIMARY KEY     NOT NULL," \
               "command           TEXT    NOT NULL," \
               "state             INT     NOT NULL," \
               "output_filename   TEXT    NOT NULL," \
               "store_output      INT     NOT NULL," \
               "pid               INT     NOT NULL," \
               "ts_UID            INT     NOT NULL," \
               "should_keep_finished INT NOT NULL," \
               "depend_on         INT     NOT NULL," \
               "depend_on_size    INT     NOT NULL," \
               "notify_errorlevel_to INT   NOT NULL," \
               "notify_errorlevel_to_size INT NOT NULL," \
               "dependency_errorlevel INT NOT NULL," \
               "label TEXT NOT NULL," \
               "email TEXT NOT NULL," \
               "num_slots INT NOT NULL, " \
               "errorlevel INT NOT NULL, died_by_signal INT NOT NULL, signal INT NOT NULL, "\
               "user_ms FLOAT NOT NULL, system_ms FLOAT NOT NULL, real_ms FLOAT NOT NULL, skipped INT NOT NULL, " \
               "ptr TEXT NOT NULL, nchars INT NOT NULL, allocchars INT NOT NULL, "\
               "enqueue_time INT NOT NULL, start_time INT NOT NULL, end_time INT NOT NULL, " \
               "enqueue_time_ms INT NOT NULL, start_time_ms INT NOT NULL, end_time_ms INT NOT NULL, " \
               "order_id INT NOT NULL, command_strip INT NOT NULL, work_dir TEXT    NOT NULL);";

    rc = sqlite3_exec(db, sql, 0, 0, &zErrMsg);

    if (rc != SQLITE_OK) {
        printf("[open_sqlite0] SQL error: %s\n", zErrMsg);
        sqlite3_free(zErrMsg);
    } else {
        printf("Table Jobs created successfully\n");
    }

    char *sql2 = "CREATE TABLE IF NOT EXISTS Finished(" \
               "jobid INT PRIMARY KEY     NOT NULL," \
               "command           TEXT    NOT NULL," \
               "state             INT     NOT NULL," \
               "output_filename   TEXT    NOT NULL," \
               "store_output      INT     NOT NULL," \
               "pid               INT     NOT NULL," \
               "ts_UID            INT     NOT NULL," \
               "should_keep_finished INT NOT NULL," \
               "depend_on         INT     NOT NULL," \
               "depend_on_size    INT     NOT NULL," \
               "notify_errorlevel_to INT   NOT NULL," \
               "notify_errorlevel_to_size INT NOT NULL," \
               "dependency_errorlevel INT NOT NULL," \
               "label TEXT NOT NULL," \
               "email TEXT NOT NULL," \
               "num_slots INT NOT NULL, " \
               "errorlevel INT NOT NULL, died_by_signal INT NOT NULL, signal INT NOT NULL, "\
               "user_ms FLOAT NOT NULL, system_ms FLOAT NOT NULL, real_ms FLOAT NOT NULL, skipped INT NOT NULL, " \
               "ptr TEXT NOT NULL, nchars INT NOT NULL, allocchars INT NOT NULL, "\
               "enqueue_time INT NOT NULL, start_time INT NOT NULL, end_time INT NOT NULL, " \
               "enqueue_time_ms INT NOT NULL, start_time_ms INT NOT NULL, end_time_ms INT NOT NULL, " \
               "order_id INT NOT NULL, command_strip INT NOT NULL, work_dir TEXT    NOT NULL);";

    rc = sqlite3_exec(db, sql2, 0, 0, &zErrMsg);

    if (rc != SQLITE_OK) {
        printf("[open_sqlite1] SQL error: %s\n", zErrMsg);
        sqlite3_free(zErrMsg);
    } else {
        printf("Table Finished created successfully\n");
    }

    sql = "CREATE TABLE IF NOT EXISTS Global(" \
        "id INT PRIMARY KEY NOT NULL," \
        "JOBIDs INT NOT NULL); INSERT INTO Global (id, JOBIDs) VALUES (1, 1000);";
    rc = sqlite3_exec(db, sql, 0, 0, &zErrMsg);
    if (rc != SQLITE_OK ) {
        printf("[open_sqlite2] SQL error: %s\n", zErrMsg);
        sqlite3_free(zErrMsg);
    }

    return 0;
}

int get_jobids_DB() {
    char *err_msg = 0;
    char *sql = "SELECT JOBIDs FROM Global WHERE id=1;";
    int value = 0;
    int rc = sqlite3_exec(db, sql, callback, &value, &err_msg);
    if (rc != SQLITE_OK ) {
        fprintf(stderr, "[get_jobids_DB] SQL error: %s\n", err_msg);
        sqlite3_free(err_msg);
        return 1000;
    }
    return value;
}

void set_jobids_DB(int value) {
    char *err_msg = 0;
    char sql[1024];
    sprintf(sql, "INSERT OR REPLACE INTO Global (id, JOBIDs) VALUES (1, %d);", value);
    int rc = sqlite3_exec(db, sql, 0, 0, &err_msg);
    if (rc != SQLITE_OK ) {
        fprintf(stderr, "[set_jobids_DB] SQL error: %s\n", err_msg);
        sqlite3_free(err_msg);
    }
}

int delete_DB(int jobid , const char* table) {
    char sql[1024];
    sprintf(sql,"DELETE FROM %s WHERE jobid=%d;", table, jobid);
    char* errmsg=NULL;

    if(sqlite3_exec(db ,sql,NULL,NULL,&errmsg)!=SQLITE_OK){
        fprintf(stderr,"[delete_DB] SQL error: %s by %s\n",errmsg, sql);
        sqlite3_free(errmsg);
        return -1;//返回-1表示删除失败
    }
    return 0;//返回0表示删除成功
}


static int edit_DB(struct Job* job, const char* table, const char* action) {
    struct Result* result = &(job->result);
    struct Procinfo* info= &(job->info);
    const char* label = job->label == NULL ? "(..)" : job->label;
    const char* email = job->email == NULL ? "(..)" : job->email;

    char sql[1024];
    
    int order_id = get_order_id(job->jobid);
    if (order_id == 0) {
        order_id = max_order_id() + 1;
    }
    char* depend_on = ints_to_chars(job->depend_on_size, job->depend_on, ",");
    char* notify_errorlevel_to = ints_to_chars(job->notify_errorlevel_to_size, job->notify_errorlevel_to, ",");

    sprintf(sql, "%s INTO %s (jobid, command, state, output_filename, store_output, pid, ts_UID, should_keep_finished, depend_on, depend_on_size," \
                "notify_errorlevel_to, notify_errorlevel_to_size, dependency_errorlevel,label,email,num_slots,errorlevel,died_by_signal," \
                "signal,user_ms,system_ms,real_ms,skipped," \
                "ptr,nchars,allocchars," \
                "enqueue_time,start_time,end_time," \
                "enqueue_time_ms,start_time_ms,end_time_ms, " \
                "order_id, command_strip, work_dir)" \
    "VALUES (%d,'%s',%d,'%s',%d,%d,%d,%d,'%s',%d,'%s',%d,%d,'%s','%s',%d," \
    "%d,%d,%d,%f,%f,%f,%d,"\
    "'%s',%d,%d,'%ld','%ld','%ld','%ld','%ld','%ld', " \
    "%d, %d,'%s');",
            action,
            table,
            job->jobid,
            job->command,
            job->state,
            job->output_filename,
            job->store_output,
            job->pid,
            job->ts_UID,
            job->should_keep_finished,
            depend_on, // job->depend_on,
            job->depend_on_size,
            notify_errorlevel_to,
            job->notify_errorlevel_to_size,
            job->dependency_errorlevel,
            label,
            email,
            job->num_slots,
            result->errorlevel, result->died_by_signal, result->signal, 
            result->user_ms, result->system_ms, result->real_ms, result->skipped,
            info->ptr, info->nchars, info->allocchars,
            info->enqueue_time.tv_sec, info->start_time.tv_sec, info->end_time.tv_sec,
            info->enqueue_time.tv_usec, info->start_time.tv_usec, info->end_time.tv_usec,
            order_id, job->command_strip, job->work_dir
            );
    char *errmsg = NULL;
    int rs = sqlite3_exec(db, sql,NULL,NULL,&errmsg);
    free(depend_on);
    free(notify_errorlevel_to);
    if (rs != SQLITE_OK) {
        fprintf(stderr,"[insert_DB] SQL error: %s by %s\n", errmsg, sql);
        sqlite3_free(errmsg);
        return -1; // 返回-1表示插入失败
    }
    return 0; // 返回0表示插入成功
}


int insert_DB(struct Job* job, const char* table) {
    return edit_DB(job, table, "INSERT");
}

int insert_or_replace_DB(struct Job* job, const char* table) {
    return edit_DB(job, table, "INSERT OR REPLACE");
}

static void set_order_id_DB(int jobid, int order_id) {
    char *err_msg = 0;
    char sql[1024];
    sprintf(sql, "UPDATE Jobs SET order_id=%d WHERE jobid=%d", order_id, jobid);
    int rc = sqlite3_exec(db, sql, 0, 0, &err_msg);
    if (rc != SQLITE_OK ) {
        fprintf(stderr, "[movetop_DB] SQL error: %s\n", err_msg);
        sqlite3_free(err_msg);
    }
}

void set_state_DB(int jobid, int state) {
    char *err_msg = 0;
    char sql[1024];
    sprintf(sql, "UPDATE Jobs SET state=%d WHERE jobid=%d", state, jobid);
    int rc = sqlite3_exec(db, sql, 0, 0, &err_msg);
    if (rc != SQLITE_OK ) {
        fprintf(stderr, "[movetop_DB] SQL error: %s\n", err_msg);
        sqlite3_free(err_msg);
    }
}

void swap_DB(int jobid0, int jobid1) {
    int id0 = get_order_id(jobid0);
    int id1 = get_order_id(jobid1);
    set_order_id_DB(jobid0, id1);
    set_order_id_DB(jobid1, id0);
}

void movetop_DB(int jobid) {
    int order_id = min_order_id() - 1;
    if (order_id == 0) order_id = -1;
    set_order_id_DB(jobid, order_id);
}

/*
static void clear_DB(const char* table) {
    char sql[1024];
    char* err_msg;
    sprintf(sql, "DELETE FROM %s", table);
    int rc = sqlite3_exec(db, sql, 0, 0, &err_msg);
    if (rc != SQLITE_OK ) {
        fprintf(stderr, "[clear_DB] SQL error: %s\n", err_msg);
        sqlite3_free(err_msg);
    }
}
*/
int read_jobid_DB(int** jobids, const char* table) {
    int n;
    char sql[1024];
    sprintf(sql, "SELECT COUNT(*) FROM %s;", table);
    char *errmsg = NULL;

    if (sqlite3_exec(db, sql,NULL,NULL,&errmsg) != SQLITE_OK) {
        fprintf(stderr,"[read_jobid_DB0] SQL error: %s by %s\n",
        sqlite3_errmsg(db), sql);
        sqlite3_free(errmsg);
        return -1; // 返回-1表示查询失败
    }

    // 从查询结果中读取数据
    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr,"[read_jobid_DB1] SQL error: %s by %s\n",
        sqlite3_errmsg(db), sql);
        return -2; // 返回-1表示查询失败
    }

    if (sqlite3_step(stmt) == SQLITE_ROW) {
        n = sqlite3_column_int(stmt, 0);
    }
    if (n == 0) return 0;
    *jobids = (int*)malloc(n * sizeof(int));

    sprintf(sql, "SELECT jobid FROM %s ORDER BY order_id;", table);
    rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr,"[read_jobid_DB2] SQL error: %s from %s\n",
         sqlite3_errmsg(db), sql);
        debug_write("test0");
        return -3; // 返回-1表示查询失败
    }

    int i = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        (*jobids)[i++] = sqlite3_column_int(stmt, 0);
    }

    return n; // 返回0表示查询成功
}


struct Job* read_DB(int jobid, const char* table) {
    struct Job* job = (struct Job*)malloc(sizeof(struct Job)*1);
    job->cores = NULL;
    struct Result* result = &(job->result);
    struct Procinfo* info= &(job->info);

    char sql[2048];
    sprintf(sql, "SELECT * FROM %s WHERE jobid=%d;", table, jobid);
    char *errmsg = NULL;

    if (sqlite3_exec(db, sql,NULL,NULL,&errmsg) != SQLITE_OK) {
        fprintf(stderr,"[read_DB0] SQL error: %s\n", errmsg);
        sqlite3_free(errmsg);
        return NULL; // 返回-1表示查询失败
    }

    // 从查询结果中读取数据
    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr,"[read_DB1] SQL error: %s\n", sqlite3_errmsg(db));
        return NULL; // 返回-1表示查询失败
    }

    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        // 从查询结果中读取数据
        job->jobid = sqlite3_column_int(stmt, 0);
        
        strcpy(sql, (const char*) sqlite3_column_text(stmt, 1));
        job->command = (char*) malloc(sizeof(char) * (strlen(sql)+1));
        strcpy(job->command, sql);
        
        job->state = sqlite3_column_int(stmt, 2);
        
        strcpy(sql, (const char*) sqlite3_column_text(stmt, 3));
        job->output_filename = (char*) malloc(sizeof(char) * (strlen(sql)+1));
        strcpy(job->output_filename, sql);
        
        job->store_output = sqlite3_column_int(stmt, 4);
        job->pid = sqlite3_column_int(stmt, 5);
        job->ts_UID = sqlite3_column_int(stmt, 6);
        job->should_keep_finished = sqlite3_column_int(stmt, 7);


        // job->depend_on_size = sqlite3_column_bytes(stmt, 9);
        // job->notify_errorlevel_to_size = sqlite3_column_bytes(stmt, 11);
        job->depend_on_size = sqlite3_column_int(stmt, 9);
        if (job->depend_on_size == 0) {
            job->depend_on = NULL;
        } else {
            strcpy(sql, (const char*) sqlite3_column_text(stmt, 8));
            job->depend_on = chars_to_ints(&job->depend_on_size, sql, ",");        
        }

        job->notify_errorlevel_to_size = sqlite3_column_int(stmt, 11);
        if (job->notify_errorlevel_to_size == 0) {
            job->notify_errorlevel_to = NULL;
        } else {
            strcpy(sql, (const char*) sqlite3_column_text(stmt, 10));
            job->notify_errorlevel_to = chars_to_ints(&job->notify_errorlevel_to_size, sql, ",");        
        }
        
        job->dependency_errorlevel = sqlite3_column_int(stmt, 12);
        
        strcpy(sql, (const char*) sqlite3_column_text(stmt, 13));
        job->label = (char*) malloc(sizeof(char) * (strlen(sql)+1));
        strcpy(job->label, sql);

        strcpy(sql, (const char*) sqlite3_column_text(stmt, 14));
        job->email = (char*) malloc(sizeof(char) * (strlen(sql)+1));
        if (strcmp(sql, "(..)") != 0) {
            strcpy(job->email, sql);
        } else {
            job->email = NULL;
        }

        job->num_slots = sqlite3_column_int(stmt, 15);

        result->errorlevel = sqlite3_column_int(stmt, 16);
        result->died_by_signal = sqlite3_column_int(stmt, 17);
        result->signal = sqlite3_column_int(stmt, 18);
        result->user_ms = (float)sqlite3_column_double(stmt, 19);
        result->system_ms = (float)sqlite3_column_double(stmt, 20);
        result->real_ms = (float)sqlite3_column_double(stmt, 21);
        result->skipped = sqlite3_column_int(stmt, 22);

        strcpy(sql, (const char*) sqlite3_column_text(stmt, 23));
        info->ptr = (char*) malloc(sizeof(char) * (strlen(sql)+1));
        strcpy(info->ptr, sql);

        info->nchars=sqlite3_column_bytes(stmt,24)/sizeof(char); 
        info->allocchars=sqlite3_column_bytes(stmt,25)/sizeof(char); 

        info->enqueue_time.tv_sec=sqlite3_column_int64(stmt,26); 
        info->start_time.tv_sec=sqlite3_column_int64(stmt,27); 
        info->end_time.tv_sec=sqlite3_column_int64(stmt,28); 

        info->enqueue_time.tv_usec=sqlite3_column_int64(stmt,29); 
        info->start_time.tv_usec=sqlite3_column_int64(stmt,30); 
        info->end_time.tv_usec=sqlite3_column_int64(stmt,31); 
        job->command_strip=sqlite3_column_int(stmt, 33);

        strcpy(sql, (const char*) sqlite3_column_text(stmt, 34));
        job->work_dir = (char*) malloc(sizeof(char) * (strlen(sql)+1));
        strcpy(job->work_dir, sql);

    } else {
      fprintf(stderr,"[read_DB2] SQL error: %s\n", sqlite3_errmsg(db));
      return NULL; // 返回-1表示查询失败
    }
    
    return job; // 返回0表示查询成功
}

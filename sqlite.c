#include <sqlite3.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "defaults.h"
#include "main.h"
#include "user.h"
#include "sqlite.h"
#include "utils.h"
#include "error.h"

sqlite3 *db = NULL;
char sql[1024*16] = "";

const char *get_sqlite_path() {
  char *str;
  str = getenv("TS_SQLITE_PATH");
  if (str == NULL || strlen(str) == 0) {
    return DEFAULT_SQLITE_PATH;
  } else {
    return str;
  }
}

static void copy_with_nullcheck(char **dst, char *src) {
  if (strcmp(src, "(null)") != 0 && strcmp(src, "(..)") != 0) {
    dst[0] = (char *)malloc(sizeof(char) * (strlen(src) + 1));
    strcpy(dst[0], src);
  } else {
    dst[0] = NULL;
  }
}

static int callback(void *max, int argc, char **argv, char **azColName) {
  if (argv[0]) {
    *(int *)max = atoi(argv[0]);
  } else {
    *(int *)max = 0;
  }
  return 0;
}

static int check_order_id(const char *op, int* err) {
  char *err_msg = NULL;
  snprintf(sql, sizeof(sql), "SELECT %s(order_id) FROM Jobs", op);
  int value = 0;
  int rc = sqlite3_exec(db, sql, callback, &value, &err_msg);
  if (rc != SQLITE_OK) {
    fprintf(stderr, "[check_order_id] SQL error: %s, sql: %s\n", sql, err_msg);
    sqlite3_free(err_msg);
    err[0] = -1;
  } else {
    err[0] = 0;
  }
  return value;
}

static int max_order_id(int* err) { return check_order_id("MAX", err); }

static int min_order_id(int* err) { return check_order_id("MIN", err); }

static int get_order_id(int jobid, int* err) {
  char *err_msg = 0;
  int value = 0;
  snprintf(sql, sizeof(sql), "SELECT order_id FROM Jobs WHERE jobid=%d", jobid);
  int rc = sqlite3_exec(db, sql, callback, &value, &err_msg);
  if (rc != SQLITE_OK) {
    fprintf(stderr, "[get_order_id] SQL error: %s\n", err_msg);
    sqlite3_free(err_msg);
    err[0] = -1;
  } else {
    err[0] = 0;
  }
  return value;
}

int close_sqlite() {
  sqlite3_exec(db, "PRAGMA wal_checkpoint(TRUNCATE)", 0, 0, 0);
  return sqlite3_close(db);
}

int open_sqlite() {
  const char *path = get_sqlite_path();
  int rc;
  int error_flag = 0;

  rc = sqlite3_open(path, &db);
  if (rc) {
    printf("Can't open database: %s\n", sqlite3_errmsg(db));
    sqlite3_close(db);
    return -1;
  }

  /* WAL: writes don't block reads. busy_timeout: retry on SQLITE_BUSY */
  sqlite3_exec(db, "PRAGMA journal_mode=WAL", 0, 0, 0);
  sqlite3_exec(db, "PRAGMA busy_timeout=5000", 0, 0, 0);

  char *zErrMsg = 0;
  char *sql =
      "CREATE TABLE IF NOT EXISTS Jobs("
      "jobid INT PRIMARY KEY     NOT NULL,"
      "command           TEXT    NOT NULL,"
      "state             INT     NOT NULL,"
      "output_filename   TEXT    NOT NULL,"
      "store_output      INT     NOT NULL,"
      "pid               INT     NOT NULL,"
      "ts_UID            INT     NOT NULL,"
      "should_keep_finished INT NOT NULL,"
      "depend_on         INT     NOT NULL,"
      "depend_on_size    INT     NOT NULL,"
      "notify_errorlevel_to INT   NOT NULL,"
      "notify_errorlevel_to_size INT NOT NULL,"
      "dependency_errorlevel INT NOT NULL,"
      "label TEXT NOT NULL,"
      "email TEXT NOT NULL,"
      "num_slots INT NOT NULL, "
      "errorlevel INT NOT NULL, died_by_signal INT NOT NULL, signal INT NOT "
      "NULL, "
      "user_sec INT NOT NULL, system_sec INT NOT NULL, real_sec INT NOT "
      "NULL, skipped INT NOT NULL, "
      "ptr TEXT NOT NULL, nchars INT NOT NULL, allocchars INT NOT NULL, wall_time INT NOT NULL,"
      "schedule_time INT NOT NULL,"
      "enqueue_time INT NOT NULL, start_time INT NOT NULL, end_time INT NOT "
      "NULL, "
      "pause_time INT NOT NULL, pause_duration INT NOT NULL, end_time_ms "
      "INT NOT NULL, "
      "order_id INT NOT NULL, command_strip INT NOT NULL, work_dir TEXT    NOT "
      "NULL);";

  /* Migrate existing databases */
  sqlite3_exec(db, "ALTER TABLE Jobs ADD COLUMN schedule_time INT NOT NULL DEFAULT 0", 0, 0, 0);

  rc = sqlite3_exec(db, sql, 0, 0, &zErrMsg);

  if (rc != SQLITE_OK) {
    sqlite3_free(zErrMsg);
    error_flag--;
  } else {
    printf("Table Jobs created successfully\n");
  }

  char *sql2 =
      "CREATE TABLE IF NOT EXISTS Finished("
      "jobid INT PRIMARY KEY     NOT NULL,"
      "command           TEXT    NOT NULL,"
      "state             INT     NOT NULL,"
      "output_filename   TEXT    NOT NULL,"
      "store_output      INT     NOT NULL,"
      "pid               INT     NOT NULL,"
      "ts_UID            INT     NOT NULL,"
      "should_keep_finished INT NOT NULL,"
      "depend_on         INT     NOT NULL,"
      "depend_on_size    INT     NOT NULL,"
      "notify_errorlevel_to INT   NOT NULL,"
      "notify_errorlevel_to_size INT NOT NULL,"
      "dependency_errorlevel INT NOT NULL,"
      "label TEXT NOT NULL,"
      "email TEXT NOT NULL,"
      "num_slots INT NOT NULL, "
      "errorlevel INT NOT NULL, died_by_signal INT NOT NULL, signal INT NOT "
      "NULL, "
      "user_sec INT NOT NULL, system_sec INT NOT NULL, real_sec INT NOT "
      "NULL, skipped INT NOT NULL, "
      "ptr TEXT NOT NULL, nchars INT NOT NULL, allocchars INT NOT NULL, wall_time INT NOT NULL,"
      "schedule_time INT NOT NULL,"
      "enqueue_time INT NOT NULL, start_time INT NOT NULL, end_time INT NOT "
      "NULL, "
      "pause_time INT NOT NULL, pause_duration INT NOT NULL, end_time_ms "
      "INT NOT NULL, "
      "order_id INT NOT NULL, command_strip INT NOT NULL, work_dir TEXT NOT "
      "NULL);";

  /* Migrate existing databases */
  sqlite3_exec(db, "ALTER TABLE Finished ADD COLUMN schedule_time INT NOT NULL DEFAULT 0", 0, 0, 0);

  rc = sqlite3_exec(db, sql2, 0, 0, &zErrMsg);

  if (rc != SQLITE_OK) {
    printf("[open_sqlite1] SQL error: %s\n", zErrMsg);
    sqlite3_free(zErrMsg);
    error_flag--;
  } else {
    printf("Table Finished created successfully\n");
  }

  sql =
      "CREATE TABLE IF NOT EXISTS Global("
      "id INT PRIMARY KEY NOT NULL,"
      "JOBIDs INT NOT NULL); INSERT INTO Global (id, JOBIDs) VALUES (1, 1000);";
  rc = sqlite3_exec(db, sql, 0, 0, &zErrMsg);
  if (rc != SQLITE_OK) {
    printf("[open_sqlite2] SQL error: %s\n", zErrMsg);
    sqlite3_free(zErrMsg);
    // error_flag--;
  }
  return error_flag;
}


/**
 * 修改整型字段值
 */
int update_field_int64(const char *tableName, int jobid, const char *columnName, int64_t newValue) {
    sqlite3_stmt *stmt = NULL;
    char sql[512];
    int rc;
    
    snprintf(sql, sizeof(sql), 
             "UPDATE %s SET %s = ? WHERE jobid = ?;", 
             tableName, columnName);
    
    rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "Failed to prepare statement: %s\n", sqlite3_errmsg(db));
        return -1;
    }
    
    sqlite3_bind_int64(stmt, 1, newValue);
    sqlite3_bind_int(stmt, 2, jobid);
    
    rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        fprintf(stderr, "Update failed: %s\n", sqlite3_errmsg(db));
        sqlite3_finalize(stmt);
        return -1;
    }
    
    sqlite3_finalize(stmt);
    return 0;
}

int get_jobids_DB() {
  char *err_msg = 0;
  char *sql = "SELECT JOBIDs FROM Global WHERE id=1;";
  int value = 0;
  int rc = sqlite3_exec(db, sql, callback, &value, &err_msg);
  if (rc != SQLITE_OK) {
    fprintf(stderr, "[get_jobids_DB] SQL error: %s\n", err_msg);
    sqlite3_free(err_msg);
    return 1000; // default value
  }
  return value;
}

// return error code
int set_jobids_DB(int value) {
  char *err_msg = 0;
  snprintf(sql, sizeof(sql), "INSERT OR REPLACE INTO Global (id, JOBIDs) VALUES (1, %d);",
          value);
  int rc = sqlite3_exec(db, sql, 0, 0, &err_msg);
  if (rc != SQLITE_OK) {
    fprintf(stderr, "[set_jobids_DB] SQL error: %s\n", err_msg);
    sqlite3_free(err_msg);
    return -1;
  }
  return 0;
}

// return error code
int delete_DB(int jobid, const char *table) {
  snprintf(sql, sizeof(sql), "DELETE FROM %s WHERE jobid=%d;", table, jobid);
  char *errmsg = NULL;

  if (sqlite3_exec(db, sql, NULL, NULL, &errmsg) != SQLITE_OK) {
    fprintf(stderr, "[delete_DB] SQL error: %s by %s\n", errmsg, sql);
    sqlite3_free(errmsg);
    return -1; //返回-1表示删除失败
  }
  return 0; //返回0表示删除成功
}

static int edit_DB(struct Job *job, const char *table, const char *action) {
  struct Result *result = &(job->result);
  struct Procinfo *info = &(job->info);
  const char *label = job->label == NULL ? "(..)" : job->label;
  const char *email = job->email == NULL ? "(..)" : job->email;
  int err = 0;
  int order_id = get_order_id(job->jobid, &err);
  if (err != 0) {
    order_id = max_order_id(&err) + 1;
    if (err !=0) return -1;
  }
  char *depend_on = ints_to_chars(job->depend_on_size, job->depend_on, ",");
  char *notify_errorlevel_to = ints_to_chars(job->notify_errorlevel_to_size,
                                             job->notify_errorlevel_to, ",");

  char *esc_command = sqlite3_mprintf("%q", job->command);
  char *esc_output = sqlite3_mprintf("%q", job->output_filename);
  char *esc_depend = sqlite3_mprintf("%q", depend_on);
  char *esc_notify = sqlite3_mprintf("%q", notify_errorlevel_to);
  char *esc_label = sqlite3_mprintf("%q", label);
  char *esc_email = sqlite3_mprintf("%q", email);
  char *esc_ptr = sqlite3_mprintf("%q", info->ptr);
  char *esc_work_dir = sqlite3_mprintf("%q", job->work_dir);

  int ts_uid = job->user ? (int)job->user->uid : -1;

  sprintf(
      sql,
      "%s INTO %s (jobid, command, state, output_filename, store_output, pid, "
      "ts_UID, should_keep_finished, depend_on, depend_on_size,"
      "notify_errorlevel_to, notify_errorlevel_to_size, "
      "dependency_errorlevel,label,email,num_slots,errorlevel,died_by_signal,"
      "signal,user_sec,system_sec,real_sec,skipped,"
      "ptr,nchars,allocchars,wall_time,schedule_time,"
      "enqueue_time,start_time,end_time,"
      "pause_time,pause_duration,end_time_ms, "
      "order_id, command_strip, work_dir)"
      "VALUES (%d,'%s',%d,'%s',%d,%d,%d,%d,'%s',%d,'%s',%d,%d,'%s','%s',%d,"
      "%d,%d,%d,%ld,%ld,%ld,%d,"
      "'%s',%d,%d,%ld,%ld,'%ld','%ld','%ld','%ld','%ld','%ld', "
      "%d, %d,'%s');",
      action, table, job->jobid, esc_command, job->state, esc_output,
      job->store_output, job->pid, ts_uid, job->should_keep_finished,
      esc_depend, // job->depend_on,
      job->depend_on_size, esc_notify, job->notify_errorlevel_to_size,
      job->dependency_errorlevel, esc_label, esc_email, job->num_slots,
      result->errorlevel, result->died_by_signal, result->signal,
      result->user_sec, result->system_sec, result->real_sec, result->skipped,
      esc_ptr, info->nchars, info->allocchars, job->wall_time, job->schedule_time, info->enqueue_time,
      info->start_time, info->end_time,
      info->pause_time, info->pause_duration, info->boot_time,
      order_id, job->command_strip, esc_work_dir);
  char *errmsg = NULL;
  int rs = sqlite3_exec(db, sql, NULL, NULL, &errmsg);
  free(depend_on);
  free(notify_errorlevel_to);

  sqlite3_free(esc_command);
  sqlite3_free(esc_output);
  sqlite3_free(esc_depend);
  sqlite3_free(esc_notify);
  sqlite3_free(esc_label);
  sqlite3_free(esc_email);
  sqlite3_free(esc_ptr);
  sqlite3_free(esc_work_dir);

  if (rs != SQLITE_OK) {
    fprintf(stderr, "[insert_DB] SQL error: %s by %s\n", errmsg, sql);
    sqlite3_free(errmsg);
    return -1; // 返回-1表示插入失败
  }
  return 0; // 返回0表示插入成功
}

int insert_DB(struct Job *job, const char *table) {
  return edit_DB(job, table, "INSERT");
}

int insert_or_replace_DB(struct Job *job, const char *table) {
  return edit_DB(job, table, "INSERT OR REPLACE");
}

//return error code
static int set_order_id_DB(int jobid, int order_id) {
  char *err_msg = 0;
  snprintf(sql, sizeof(sql), "UPDATE Jobs SET order_id=%d WHERE jobid=%d", order_id, jobid);
  int rc = sqlite3_exec(db, sql, 0, 0, &err_msg);
  if (rc != SQLITE_OK) {
    fprintf(stderr, "[movetop_DB] SQL error: %s\n", err_msg);
    sqlite3_free(err_msg);
    return -1;
  }
  return 0;
}

//return error code
int set_state_DB(int jobid, int state) {
  char *err_msg = 0;
  snprintf(sql, sizeof(sql), "UPDATE Jobs SET state=%d WHERE jobid=%d", state, jobid);
  int rc = sqlite3_exec(db, sql, 0, 0, &err_msg);
  if (rc != SQLITE_OK) {
    fprintf(stderr, "[movetop_DB] SQL error: %s\n", err_msg);
    sqlite3_free(err_msg);
    return -1;
  }
  return 0;
}

int swap_DB(int jobid0, int jobid1) {
  int err0, err1, err = 0;
  int id0 = get_order_id(jobid0, &err0);
  int id1 = get_order_id(jobid1, &err1);
  if (err0 == 0 && err1 == 0) {
    err0 = set_order_id_DB(jobid0, id1);
    err1 = set_order_id_DB(jobid1, id0);
    if (err0 != 0 || err1 != 0) {
      err = -1;
    }
  } else {
    err = -1;
  }
  return err;
}

int movetop_DB(int jobid) {
  int err;
  int order_id = min_order_id(&err) - 1;
  if (err != 0) {
    return err;
  }
  return set_order_id_DB(jobid, order_id);
}

/*
static void clear_DB(const char* table) {
    char* err_msg;
    snprintf(sql, sizeof(sql), "DELETE FROM %s", table);
    int rc = sqlite3_exec(db, sql, 0, 0, &err_msg);
    if (rc != SQLITE_OK ) {
        fprintf(stderr, "[clear_DB] SQL error: %s\n", err_msg);
        sqlite3_free(err_msg);
    }
}
*/

// return error code
int read_jobid_DB(int **jobids, const char *table) {
  int n = 0;
  snprintf(sql, sizeof(sql), "SELECT COUNT(*) FROM %s;", table);

  sqlite3_stmt *stmt;
  int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
  if (rc != SQLITE_OK) {
    fprintf(stderr, "[read_jobid_DB] SQL error: %s by %s\n",
            sqlite3_errmsg(db), sql);
    return -1;
  }

  if (sqlite3_step(stmt) == SQLITE_ROW) {
    n = sqlite3_column_int(stmt, 0);
  }
  sqlite3_finalize(stmt);

  if (n <= 0)
    return 0;
  *jobids = (int *)malloc((size_t)n * sizeof(int));

  snprintf(sql, sizeof(sql), "SELECT jobid FROM %s ORDER BY order_id;", table);
  rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
  if (rc != SQLITE_OK) {
    fprintf(stderr, "[read_jobid_DB] SQL error: %s from %s\n",
            sqlite3_errmsg(db), sql);
    free(*jobids);
    return -1;
  }

  int i = 0;
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    (*jobids)[i++] = sqlite3_column_int(stmt, 0);
  }
  sqlite3_finalize(stmt);

  return n;
}

struct Job *read_DB(int jobid, const char *table) {
  struct Job *job = (struct Job *)calloc(1, sizeof(struct Job));
  if (job == NULL) return NULL;
  struct Result *result = &(job->result);
  struct Procinfo *info = &(job->info);

  snprintf(sql, sizeof(sql), "SELECT * FROM %s WHERE jobid=%d;", table, jobid);

  sqlite3_stmt *stmt;
  int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
  if (rc != SQLITE_OK) {
    fprintf(stderr, "[read_DB] SQL error: %s\n", sqlite3_errmsg(db));
    free(job);
    return NULL;
  }

  rc = sqlite3_step(stmt);
  if (rc == SQLITE_ROW) {
    // 从查询结果中读取数据
    job->jobid = sqlite3_column_int(stmt, 0);

    strcpy(sql, (const char *)sqlite3_column_text(stmt, 1));
    copy_with_nullcheck(&(job->command), sql);

    job->state = sqlite3_column_int(stmt, 2);

    strcpy(sql, (const char *)sqlite3_column_text(stmt, 3));
    copy_with_nullcheck(&(job->output_filename), sql);

    job->store_output = sqlite3_column_int(stmt, 4);
    job->pid = sqlite3_column_int(stmt, 5);

    if (strcmp(table, "Jobs") == 0) {
        job->user = get_user_by_pid(job->pid);
    }
    if (job->user == NULL) {
        int saved_uid = sqlite3_column_int(stmt, 6);
        job->user = find_user_by_uid((uid_t)saved_uid);
    }
    job->should_keep_finished = sqlite3_column_int(stmt, 7);

    // job->depend_on_size = sqlite3_column_bytes(stmt, 9);
    // job->notify_errorlevel_to_size = sqlite3_column_bytes(stmt, 11);
    job->depend_on_size = sqlite3_column_int(stmt, 9);
    if (job->depend_on_size == 0) {
      job->depend_on = NULL;
    } else {
      strcpy(sql, (const char *)sqlite3_column_text(stmt, 8));
      job->depend_on = chars_to_ints(&job->depend_on_size, sql, ",");
    }

    job->notify_errorlevel_to_size = sqlite3_column_int(stmt, 11);
    if (job->notify_errorlevel_to_size == 0) {
      job->notify_errorlevel_to = NULL;
    } else {
      strcpy(sql, (const char *)sqlite3_column_text(stmt, 10));
      job->notify_errorlevel_to =
          chars_to_ints(&job->notify_errorlevel_to_size, sql, ",");
    }

    job->dependency_errorlevel = sqlite3_column_int(stmt, 12);

    strcpy(sql, (const char *)sqlite3_column_text(stmt, 13));
    copy_with_nullcheck(&(job->label), sql);

    strcpy(sql, (const char *)sqlite3_column_text(stmt, 14));
    copy_with_nullcheck(&(job->email), sql);

    job->num_slots = sqlite3_column_int(stmt, 15);

    result->errorlevel = sqlite3_column_int(stmt, 16);
    result->died_by_signal = sqlite3_column_int(stmt, 17);
    result->signal = sqlite3_column_int(stmt, 18);
    result->user_sec = sqlite3_column_int(stmt, 19);
    result->system_sec = sqlite3_column_int(stmt, 20);
    result->real_sec = sqlite3_column_int(stmt, 21);
    result->skipped = sqlite3_column_int(stmt, 22);

    strcpy(sql, (const char *)sqlite3_column_text(stmt, 23));
    copy_with_nullcheck(&(info->ptr), sql);

    info->nchars = sqlite3_column_bytes(stmt, 24);
    info->allocchars = sqlite3_column_bytes(stmt, 25);

    job->wall_time = sqlite3_column_int64(stmt, 26);
    info->enqueue_time = sqlite3_column_int64(stmt, 27);
    info->start_time = sqlite3_column_int64(stmt, 28);
    info->end_time = sqlite3_column_int64(stmt, 29);

    info->pause_time = sqlite3_column_int64(stmt, 30);
    info->pause_duration = sqlite3_column_int64(stmt, 31);
    info->boot_time = sqlite3_column_int64(stmt, 32);
    job->command_strip = sqlite3_column_int(stmt, 34);

    strcpy(sql, (const char *)sqlite3_column_text(stmt, 35));
    copy_with_nullcheck(&(job->work_dir), sql);

    job->schedule_time = sqlite3_column_int64(stmt, 36);

  } else {
    fprintf(stderr, "[read_DB] no row found for job %d in %s\n", jobid, table);
    sqlite3_finalize(stmt);
    free(job);
    return NULL;
  }

  sqlite3_finalize(stmt);
  return job;
}

#define USER_NAME_WIDTH 256
#define USER_MAX 100

char user_name[USER_MAX][USER_NAME_WIDTH];
int server_uid;
int user_max_slots[USER_MAX];
int user_UID[USER_MAX];
int user_busy[USER_MAX];
int user_jobs[USER_MAX];
int user_queue[USER_MAX];
int user_number;
char *logfile_path;
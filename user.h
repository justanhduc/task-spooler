#define USER_NAME_WIDTH 256
#define USER_MAX 100
#include <stdint.h>

char user_name[USER_MAX][USER_NAME_WIDTH]; // the linux user name
int server_uid;
int user_max_slots[USER_MAX]; // the max slots for each user in TS
int user_UID[USER_MAX];       // the linux UID for each user in TS
int user_busy[USER_MAX];      // the number of used slots
int user_jobs[USER_MAX];	  // the number of job in running
int user_queue[USER_MAX];     // the number of job in queue
int user_locked[USER_MAX];    // whether the user is locked
int user_number;
char *logfile_path;

struct ucred {
	uint32_t	pid;
	uint32_t	uid;
	uint32_t	gid;
};
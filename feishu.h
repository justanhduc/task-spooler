#ifndef TS_FEISHU_H
#define TS_FEISHU_H

struct Job;

/* Only the server thread calls these functions. The worker owns copied data. */
void feishu_init(void);
void feishu_shutdown(void);
void feishu_notify(const struct Job *job, const char *event);
int feishu_valid_webhook(const char *url);
char *feishu_card_payload(const struct Job *job, const char *event);

#endif

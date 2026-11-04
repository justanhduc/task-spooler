#define _POSIX_C_SOURCE 200809L
#include <curl/curl.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "main.h"
#include "feishu.h"
#include "cjson/cJSON.h"

#define QUEUE_LIMIT 256
#define RESPONSE_LIMIT 16384

struct Notification {
    struct Notification *next;
    char *webhook;
    char *payload;
    int jobid;
    char event[16];
};

static struct {
    pthread_t thread;
    pthread_mutex_t mutex;
    pthread_cond_t ready;
    struct Notification *head, *tail;
    unsigned int queued;
    int started, stopping;
    char *error_path;
} notifier = {
    .mutex = PTHREAD_MUTEX_INITIALIZER,
    .ready = PTHREAD_COND_INITIALIZER
};

/* Do not call warning() here: it traverses the server's mutable job list. */
static void notification_error(int jobid, const char *event, const char *reason) {
    char message[512];
    int length = snprintf(message, sizeof(message),
                         "Feishu notification: job=%d event=%s error=%s\n",
                         jobid, event, reason);
    int fd = notifier.error_path ? open(notifier.error_path,
                                       O_WRONLY | O_CREAT | O_APPEND, 0600) : -1;
    if (fd >= 0) {
        if (length > 0)
            (void) write(fd, message, (size_t) length < sizeof(message)
                                     ? (size_t) length : sizeof(message) - 1);
        close(fd);
    }
}

int feishu_valid_webhook(const char *url) {
    const char *port;
    char *end;
    long number;
    if (!url || strlen(url) > 2048 || strpbrk(url, "\r\n\t "))
        return 0;
    /* HTTP is allowed only for loopback integration-test receivers. */
    if (strncmp(url, "https://", 8) == 0 && url[8] != '\0')
        return 1;
    if (strncmp(url, "http://127.0.0.1:", 17) != 0 &&
        strncmp(url, "http://localhost:", 17) != 0)
        return 0;
    port = url + 17;
    if (*port < '0' || *port > '9')
        return 0;
    number = strtol(port, &end, 10);
    return number > 0 && number <= 65535 &&
           (*end == '\0' || *end == '/' || *end == '?' || *end == '#');
}

struct Response {
    char data[RESPONSE_LIMIT + 1];
    size_t length;
};

static size_t receive_response(char *data, size_t size, size_t count, void *arg) {
    struct Response *response = arg;
    size_t bytes = size * count;
    if (bytes > RESPONSE_LIMIT - response->length)
        return 0;
    memcpy(response->data + response->length, data, bytes);
    response->length += bytes;
    response->data[response->length] = '\0';
    return bytes;
}

static void deliver(const struct Notification *notification) {
    CURL *curl = curl_easy_init();
    struct curl_slist *headers = NULL;
    struct Response response = {{0}, 0};
    long status = 0;
    CURLcode result;
    cJSON *body, *code;
    if (!curl) {
        notification_error(notification->jobid, notification->event, "curl initialization failed");
        return;
    }
    headers = curl_slist_append(NULL, "Content-Type: application/json; charset=utf-8");
    if (!headers) {
        curl_easy_cleanup(curl);
        notification_error(notification->jobid, notification->event, "out of memory");
        return;
    }
    curl_easy_setopt(curl, CURLOPT_URL, notification->webhook);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, notification->payload);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 2L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, receive_response);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    result = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    body = cJSON_Parse(response.data);
    code = cJSON_GetObjectItemCaseSensitive(body, "code");
    if (!code)
        code = cJSON_GetObjectItemCaseSensitive(body, "StatusCode");
    if (result != CURLE_OK)
        notification_error(notification->jobid, notification->event, curl_easy_strerror(result));
    else if (status < 200 || status >= 300) {
        char reason[64];
        snprintf(reason, sizeof(reason), "HTTP status %ld", status);
        notification_error(notification->jobid, notification->event, reason);
    } else if (!cJSON_IsNumber(code) || code->valuedouble != 0) {
        char reason[96];
        if (cJSON_IsNumber(code))
            snprintf(reason, sizeof(reason), "bot rejected message: code %.0f", code->valuedouble);
        else
            snprintf(reason, sizeof(reason), "invalid JSON response (missing numeric code)");
        notification_error(notification->jobid, notification->event, reason);
    }
    cJSON_Delete(body);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
}

static void *notification_worker(void *unused) {
    (void) unused;
    for (;;) {
        struct Notification *notification;
        pthread_mutex_lock(&notifier.mutex);
        while (!notifier.head && !notifier.stopping)
            pthread_cond_wait(&notifier.ready, &notifier.mutex);
        notification = notifier.head;
        if (!notification) {
            pthread_mutex_unlock(&notifier.mutex);
            break;
        }
        notifier.head = notification->next;
        if (!notifier.head)
            notifier.tail = NULL;
        --notifier.queued;
        pthread_mutex_unlock(&notifier.mutex);
        deliver(notification);
        free(notification->webhook);
        free(notification->payload);
        free(notification);
    }
    return NULL;
}

void feishu_init(void) {
    char *socket_path;
    create_socket_path(&socket_path);
    notifier.error_path = malloc(strlen(socket_path) + sizeof(".error"));
    if (notifier.error_path)
        sprintf(notifier.error_path, "%s.error", socket_path);
    free(socket_path);
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
        notification_error(-1, "init", "curl initialization failed");
        return;
    }
    if (pthread_create(&notifier.thread, NULL, notification_worker, NULL) == 0)
        notifier.started = 1;
    else {
        notification_error(-1, "init", "cannot start worker");
        curl_global_cleanup();
    }
}

void feishu_shutdown(void) {
    if (notifier.started) {
        pthread_mutex_lock(&notifier.mutex);
        notifier.stopping = 1;
        pthread_cond_signal(&notifier.ready);
        pthread_mutex_unlock(&notifier.mutex);
        pthread_join(notifier.thread, NULL);
        curl_global_cleanup();
        notifier.started = 0;
    }
    free(notifier.error_path);
    notifier.error_path = NULL;
}

void feishu_notify(const struct Job *job, const char *event) {
    const char *webhook = getenv("TS_FEISHU_WEBHOOK");
    struct Notification *notification;
    char *json;
    if (!webhook || !*webhook)
        return;
    if (!notifier.started || !feishu_valid_webhook(webhook)) {
        notification_error(job->jobid, event, "worker unavailable or invalid webhook");
        return;
    }
    json = feishu_card_payload(job, event);
    notification = calloc(1, sizeof(*notification));
    if (!notification || !json) {
        free(notification);
        free(json);
        return;
    }
    notification->payload = json;
    notification->webhook = strdup(webhook);
    notification->jobid = job->jobid;
    snprintf(notification->event, sizeof(notification->event), "%s", event);
    pthread_mutex_lock(&notifier.mutex);
    if (!notification->webhook || notifier.queued >= QUEUE_LIMIT) {
        pthread_mutex_unlock(&notifier.mutex);
        notification_error(job->jobid, event, "notification queue full or out of memory");
        free(notification->webhook);
        free(notification->payload);
        free(notification);
        return;
    }
    if (notifier.tail)
        notifier.tail->next = notification;
    else
        notifier.head = notification;
    notifier.tail = notification;
    ++notifier.queued;
    pthread_cond_signal(&notifier.ready);
    pthread_mutex_unlock(&notifier.mutex);
}

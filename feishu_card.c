#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <math.h>
#include <limits.h>

#include "main.h"
#include "feishu.h"
#include "cjson/cJSON.h"

static void markdown(cJSON *elements, const char *content) {
    cJSON *element = cJSON_CreateObject();
    cJSON_AddStringToObject(element, "tag", "markdown");
    cJSON_AddStringToObject(element, "content", content);
    cJSON_AddItemToArray(elements, element);
}

/* Bound message size without splitting UTF-8 characters. */
static size_t field_length(const char *value, size_t limit) {
    size_t length = value ? strlen(value) : 0;
    if (length > limit) {
        length = limit;
        while (length && ((unsigned char) value[length] & 0xc0) == 0x80)
            --length;
    }
    return length;
}

/* Escape user text so labels cannot add links, formatting or @all mentions. */
static void label_markdown(cJSON *elements, const char *value) {
    char text[4096] = "**任务名称**\n";
    size_t used = strlen(text), length = field_length(value, 512);
    for (size_t i = 0; i < length; ++i) {
        const char *entity = NULL;
        if (value[i] == '<') entity = "&lt;";
        else if (value[i] == '>') entity = "&gt;";
        else if (value[i] == '&') entity = "&amp;";
        if (entity) {
            strcpy(text + used, entity);
            used += strlen(entity);
        } else {
            if (strchr("\\`*_[]()~#!|", value[i]))
                text[used++] = '\\';
            text[used++] = value[i];
        }
    }
    text[used] = '\0';
    if (value[length])
        strcat(text, "…（已截断）");
    markdown(elements, text);
}

static void code_markdown(cJSON *elements, const char *heading,
                          const char *value, size_t limit, const char *language) {
    size_t length = field_length(value, limit), run = 0, fence = 3;
    char *text, *cursor;
    /* A longer fence keeps embedded backticks inside the code block. */
    for (size_t i = 0; i < length; ++i) {
        run = value[i] == '`' ? run + 1 : 0;
        if (run >= fence)
            fence = run + 1;
    }
    text = malloc(length + fence * 2 + strlen(heading) + strlen(language) + 128);
    if (!text)
        return;
    cursor = text + sprintf(text, "**%s**\n", heading);
    memset(cursor, '`', fence);
    cursor += fence;
    cursor += sprintf(cursor, "%s\n", language);
    if (length) {
        memcpy(cursor, value, length);
        cursor += length;
    }
    *cursor++ = '\n';
    memset(cursor, '`', fence);
    cursor += fence;
    *cursor = '\0';
    if (value && value[length])
        strcat(text, "\n…（内容过长，已截断）");
    markdown(elements, text);
    free(text);
}

static void duration_text(char *buffer, size_t size, double seconds) {
    long long total = (isfinite(seconds) && seconds > 0 && seconds < (double)LLONG_MAX)
                      ? (long long)seconds : 0;
    if (total >= 86400)
        snprintf(buffer, size, "%lld天%lld小时%lld分", total / 86400,
                 total / 3600 % 24, total / 60 % 60);
    else if (total >= 3600)
        snprintf(buffer, size, "%lld小时%lld分", total / 3600, total / 60 % 60);
    else
        snprintf(buffer, size, "%lld分%lld秒", total / 60, total % 60);
}

static void gpu_constraint(cJSON *elements, const struct Job *job) {
    const int *ids = job->allowed_gpu_ids;
    int count = job->num_allowed_gpus;
    const char *heading = "候选 GPU";
    char text[1024];
    size_t used;
    if (!count && job->num_gpus && !job->wait_free_gpus) {
        ids = job->gpu_ids;
        count = job->num_gpus;
        heading = "指定 GPU（不等待空闲）";
    }
    if (!count) return;
    used = (size_t)snprintf(text, sizeof(text), "**%s**　", heading);
    for (int i = 0; i < count; ++i) {
        if (used > sizeof(text) - 48) {
            snprintf(text + used, sizeof(text) - used, "…（共%d张）", count);
            break;
        }
        used += (size_t)snprintf(text + used, sizeof(text) - used, "%s%d", i ? ", " : "", ids[i]);
    }
    markdown(elements, text);
}

char *feishu_card_payload(const struct Job *job, const char *event) {
    cJSON *root = cJSON_CreateObject(), *card, *header, *title, *body, *elements;
    const char *status = "📥 任务已提交", *color = "blue";
    char host[256] = "unknown", text[2048], timestamp[64];
    char *payload;
    time_t now = time(NULL);
    struct tm local;
    if (!root)
        return NULL;
    if (strcmp(event, "running") == 0) {
        status = "🚀 任务开始运行";
        color = "wathet";
    } else if (strcmp(event, "succeeded") == 0) {
        status = "✅ 任务执行成功";
        color = "green";
    } else if (strcmp(event, "failed") == 0) {
        status = "❌ 任务执行失败";
        color = "red";
    } else if (strcmp(event, "skipped") == 0) {
        status = "⏭️ 任务已跳过";
        color = "orange";
    }
    cJSON_AddStringToObject(root, "msg_type", "interactive");
    card = cJSON_AddObjectToObject(root, "card");
    cJSON_AddStringToObject(card, "schema", "2.0");
    header = cJSON_AddObjectToObject(card, "header");
    cJSON_AddStringToObject(header, "template", color);
    title = cJSON_AddObjectToObject(header, "title");
    cJSON_AddStringToObject(title, "tag", "plain_text");
    snprintf(text, sizeof(text), "%s · #%d", status, job->jobid);
    cJSON_AddStringToObject(title, "content", text);
    gethostname(host, sizeof(host) - 1);
    host[sizeof(host) - 1] = '\0';
    title = cJSON_AddObjectToObject(header, "subtitle");
    cJSON_AddStringToObject(title, "tag", "plain_text");
    snprintf(text, sizeof(text), "Task Spooler · %s", host);
    cJSON_AddStringToObject(title, "content", text);
    body = cJSON_AddObjectToObject(card, "body");
    elements = cJSON_AddArrayToObject(body, "elements");

    if (job->label && *job->label)
        label_markdown(elements, job->label);
    localtime_r(&now, &local);
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M", &local);
    snprintf(text, sizeof(text), "**任务编号**　`#%d`\n**申请资源**　%s%d%s\n**事件时间**　%s",
             job->jobid, job->num_gpus ? "" : "CPU 任务（", job->num_gpus,
             job->num_gpus ? " 张 GPU" : " 张 GPU）", timestamp);
    markdown(elements, text);

    gpu_constraint(elements, job);

    if (job->pid > 0 && strcmp(event, "submitted") != 0) {
        char ids[512] = "";
        size_t used = 0;
        for (int i = 0; i < job->num_gpus && used < sizeof(ids) - 32; ++i)
            used += (size_t) snprintf(ids + used, sizeof(ids) - used,
                                     "%s%d", i ? ", " : "", job->gpu_ids[i]);
        snprintf(text, sizeof(text), "**分配 GPU**　%s\n**进程 PID**　`%d`",
                 job->num_gpus ? ids : "无需 GPU", job->pid);
        markdown(elements, text);
    }

    if (job->state == FINISHED || job->state == SKIPPED) {
        char duration[128];
        duration_text(duration, sizeof(duration), job->result.real_ms);
        if (job->result.skipped)
            snprintf(text, sizeof(text), "**执行结果**\n依赖任务未成功，当前任务已跳过。\n**退出码**　`%d`", job->result.errorlevel);
        else if (job->result.died_by_signal)
            snprintf(text, sizeof(text), "**执行结果**\n进程被信号 `%d` 终止。\n**退出码**　`%d`　　**运行耗时**　%s",
                     job->result.signal, job->result.errorlevel, duration);
        else
            snprintf(text, sizeof(text), "**执行结果**\n**退出码**　`%d`　　**运行耗时**　%s",
                     job->result.errorlevel, duration);
        markdown(elements, text);
    }

    code_markdown(elements, "执行命令", job->command, 2048, "bash");
    if (job->output_filename && *job->output_filename) {
        code_markdown(elements, "日志文件", job->output_filename, 512, "");
        snprintf(text, sizeof(text), "**查看日志**　`ts -t %d`", job->jobid);
        markdown(elements, text);
    }
    payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return payload;
}

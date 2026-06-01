#!/bin/bash
#
# TS Guardian Login Check — 用户登录时检查是否有被冻结的未管理进程
#
# 用法: 在 /etc/profile.d/ 或用户的 ~/.bashrc 中 source 此脚本
#   source /path/to/task-spooler/tools/login-check.sh
#
# 如果存在日志文件，用户将看到警告信息。

LOG_DIR="${TS_GUARDIAN_LOG_DIR:-/var/log/ts-guardian}"

login_check() {
    local username="${USER:-$(whoami)}"
    local logfile="$LOG_DIR/${username}.log"

    if [[ -f "$logfile" ]]; then
        echo ""
        echo "╔══════════════════════════════════════════════════════════════╗"
        echo "║  TS Guardian: 检测到以下非 ts 管理的长时间运行程序已被冻结 ║"
        echo "║  如有疑问请联系管理员                                       ║"
        echo "╠══════════════════════════════════════════════════════════════╣"
        while IFS= read -r line; do
            printf "║  %-56s ║\n" "${line:0:56}"
        done < "$logfile"
        echo "╚══════════════════════════════════════════════════════════════╝"
        echo ""
    fi
}

login_check

#!/bin/bash
#
# TS Guardian — 扫描指定用户的非系统进程，冻结运行超过阈值的未管理进程
#
# "系统程序" = 可执行文件属主为 root (UID 0)
# 被冻结的进程满足:
#   1. 属主是指定的被监控用户
#   2. 可执行文件属主非 root (非系统程序)
#   3. 运行时间 > RUNTIME_HOURS 阈值
#   4. 当前未被暂停 (State != T)
#   5. 父 PID 不归 task-spooler 管理 (PPID=1 或 ts --find-by-pid 找不到)
#   6. 尚未被记录到日志中 (去重)
#
# 用法: ts-guardian.sh [CONF_FILE]

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
CONF="${1:-$SCRIPT_DIR/ts-guardian.conf}"

# ============================================================
# 默认值
# ============================================================
RUNTIME_HOURS=100
LOG_DIR=/var/log/ts-guardian
TS_BIN="ts"
declare -a MONITOR_USERS=()

# ============================================================
# 读取配置文件
# ============================================================
read_conf() {
    if [[ ! -f "$CONF" ]]; then
        echo "ERROR: config file not found: $CONF"
        exit 1
    fi
    local line
    while IFS= read -r line || [[ -n "$line" ]]; do
        # 去掉注释和首尾空白
        line="${line%%#*}"
        line="$(echo "$line" | sed -e 's/^[[:space:]]*//' -e 's/[[:space:]]*$//')"
        [[ -z "$line" ]] && continue

        local key="${line%% *}"
        local val="${line#* }"
        val="$(echo "$val" | sed -e 's/^[[:space:]]*//' -e 's/[[:space:]]*$//')"

        case "$key" in
            USER)           MONITOR_USERS+=("$val") ;;
            RUNTIME_HOURS)  RUNTIME_HOURS="$val" ;;
            LOG_DIR)        LOG_DIR="$val" ;;
            TS_BIN)         TS_BIN="$val" ;;
        esac
    done < "$CONF"

    if [[ ${#MONITOR_USERS[@]} -eq 0 ]]; then
        echo "ERROR: no USER entries in $CONF"
        exit 1
    fi
}

# ============================================================
# 检测 cgroup 版本
# ============================================================
CGROUP_VER=0
detect_cgroup_ver() {
    if [[ -f /sys/fs/cgroup/cgroup.controllers ]]; then
        CGROUP_VER=2
    elif [[ -d /sys/fs/cgroup/freezer ]]; then
        CGROUP_VER=1
    else
        echo "ERROR: cannot detect cgroup version (no cgroup.controllers, no freezer)"
        exit 1
    fi
    echo "[INFO] cgroup v$CGROUP_VER detected"
}

# ============================================================
# 获取可执行文件属主 UID
# 返回 "root" 或 "nonroot" 或 "none" (无法读取, 如内核线程)
# ============================================================
exe_owner_type() {
    local pid=$1
    local exe_path="/proc/$pid/exe"
    local owner_uid

    if [[ ! -L "$exe_path" ]]; then
        # 无法读取 exe (内核线程 / 权限不足 / 僵尸)
        return 1
    fi

    owner_uid=$(stat -c '%u' "$exe_path" 2>/dev/null) || return 1

    if [[ "$owner_uid" == "0" ]]; then
        return 0  # root 拥有 → 系统程序
    else
        return 2  # 非 root 拥有 → 用户程序
    fi
}

# ============================================================
# 获取进程的运行时间（秒）
# /proc/PID/stat 字段 22 = starttime (clock ticks since boot)
# ============================================================
CLK_TCK=$(getconf CLK_TCK 2>/dev/null || echo 100)

get_uptime_sec() {
    read -r up _ < /proc/uptime
    echo "${up%.*}"
}

# 返回原始 starttime (clock ticks since boot)
get_process_starttime() {
    local pid=$1
    local stat_file="/proc/$pid/stat"
    [[ -r "$stat_file" ]] || return 1

    local stat
    stat=$(cat "$stat_file" 2>/dev/null) || return 1

    # 跳过 comm 字段 (可能含空格和括号)
    local after_comm="${stat##*)}"
    # 字段 22 = starttime (after_comm 的第 20 个字段)
    local starttime
    starttime=$(echo "$after_comm" | awk '{print $20}' 2>/dev/null) || return 1
    [[ -n "$starttime" ]] || return 1
    echo "$starttime"
}

get_process_elapsed_sec() {
    local pid=$1
    local starttime
    starttime=$(get_process_starttime "$pid" 2>/dev/null) || return 1

    local uptime
    uptime=$(get_uptime_sec)
    local elapsed=$(( uptime - (starttime / CLK_TCK) ))
    [[ $elapsed -gt 0 ]] || elapsed=0
    echo "$elapsed"
}

# ============================================================
# 获取父 PID
# /proc/PID/stat 字段 4 = PPID
# ============================================================
get_ppid() {
    local pid=$1
    local stat_file="/proc/$pid/stat"
    [[ -r "$stat_file" ]] || return 1

    local stat
    stat=$(cat "$stat_file" 2>/dev/null) || return 1

    local after_comm="${stat##*)}"
    local ppid
    ppid=$(echo "$after_comm" | awk '{print $2}' 2>/dev/null) || return 1
    echo "$ppid"
}

# ============================================================
# 获取进程的 UID
# /proc/PID/status Uid: 行
# ============================================================
get_process_uid() {
    local pid=$1
    local status_file="/proc/$pid/status"
    [[ -r "$status_file" ]] || return 1

    local uid_line
    uid_line=$(grep '^Uid:' "$status_file" 2>/dev/null) || return 1
    # Uid: <real> <effective> <saved> <fs>
    local real_uid
    real_uid=$(echo "$uid_line" | awk '{print $2}')
    echo "$real_uid"
}

# ============================================================
# 获取进程的命令行
# ============================================================
get_process_cmdline() {
    local pid=$1
    local cmdline_file="/proc/$pid/cmdline"
    if [[ -r "$cmdline_file" ]]; then
        tr '\0' ' ' < "$cmdline_file" 2>/dev/null || echo "(unknown)"
    else
        echo "(unknown)"
    fi
}

# ============================================================
# 检查进程是否为非系统程序
# 非系统程序 = exe 属主非 root
# 返回 0 表示是非系统程序
# ============================================================
is_non_system() {
    local pid=$1

    exe_owner_type "$pid"
    local owner_rc=$?
    if [[ $owner_rc -eq 1 ]]; then
        # 无法读取 exe (内核线程等)，跳过
        return 1
    elif [[ $owner_rc -eq 0 ]]; then
        # root 拥有 → 系统程序，跳过
        return 1
    fi
    # 非 root → 用户程序
    return 0
}

# ============================================================
# 检查进程是否处于暂停/冻结状态
# /proc/PID/status State 字段 = T (stopped)
# ============================================================
is_paused() {
    local pid=$1
    local status_file="/proc/$pid/status"
    [[ -r "$status_file" ]] || return 1

    local state_line
    state_line=$(grep '^State:' "$status_file" 2>/dev/null) || return 1

    # State 格式: "State:\tS (sleeping)" 或 "State:\tT (stopped)"
    if [[ "$state_line" == *"(stopped)"* ]] || [[ "$state_line" == *"(tracing stop)"* ]]; then
        return 0
    fi
    return 1
}

# ============================================================
# 检查 PID 是否被 task-spooler 管理
# 使用 ts --find-by-pid <pid> 检查
# 返回 0 表示被 ts 管理，返回 1 表示未被管理
# ============================================================
is_ts_managed() {
    local pid=$1
    local ts_cmd="${TS_BIN}"

    if ! command -v "$ts_cmd" &>/dev/null; then
        # ts 不可用，保守处理：认为不受管理
        return 1
    fi

    local result
    result=$("$ts_cmd" --find-by-pid "$pid" 2>/dev/null) || true
    if [[ "$result" =~ ^[0-9]+$ ]] && [[ "$result" -gt 0 ]]; then
        return 0  # 找到 → 被 ts 管理
    fi
    return 1  # 未找到 → 不受管理
}

# ============================================================
# 检查父 PID 是否归 ts 管理
# 条件: PPID != 1 且 ts --find-by-pid <PPID> 能找到
# 返回 0 表示父进程归 ts 管理
# ============================================================
ppid_is_ts_managed() {
    local pid=$1
    local ppid
    ppid=$(get_ppid "$pid") || return 1

    # PPID = 1 (init) → 肯定不受 ts 管理
    if [[ "$ppid" == "1" ]]; then
        return 1
    fi

    # PPID = 自身 → 孤儿
    if [[ "$ppid" == "$pid" ]]; then
        return 1
    fi

    # ts --find-by-pid <PPID> 检查
    if is_ts_managed "$ppid"; then
        return 0
    fi

    return 1
}

# ============================================================
# 日志去重：检查 PID 是否已在日志中
# 同时还检查 starttime 防止 PID 复用误判
# ============================================================
is_already_logged() {
    local pid=$1
    local starttime=$2
    local logfile=$3
    [[ -f "$logfile" ]] || return 1

    # 同时匹配 PID 和 STARTTIME，防止 PID 复用误判
    grep -qF "PID=$pid STARTTIME=$starttime " "$logfile" 2>/dev/null
}

# ============================================================
# 冻结进程 (cgroup freezer)
# ============================================================
freeze_process() {
    local pid=$1
    local cg_name="TS_GUARDIAN_${pid}"

    if [[ $CGROUP_VER -eq 2 ]]; then
        # --- cgroup v2 ---
        local cg_dir="/sys/fs/cgroup/$cg_name"
        mkdir -p "$cg_dir" 2>/dev/null || {
            echo "    WARN: cannot create cgroup dir $cg_dir"
            return 1
        }
        # 写入 PID
        echo "$pid" > "$cg_dir/cgroup.procs" 2>/dev/null || true
        # 冻结
        echo "1" > "$cg_dir/cgroup.freeze" 2>/dev/null || {
            echo "    WARN: freeze write failed for PID $pid"
            return 1
        }

    else
        # --- cgroup v1 ---
        local cg_dir="/sys/fs/cgroup/freezer/$cg_name"
        mkdir -p "$cg_dir" 2>/dev/null || {
            echo "    WARN: cannot create freezer dir $cg_dir"
            return 1
        }
        # 先写 THAWED 初始化
        echo "THAWED" > "$cg_dir/freezer.state" 2>/dev/null || true
        # 写入 PID
        echo "$pid" > "$cg_dir/cgroup.procs" 2>/dev/null || true
        # 冻结
        echo "FROZEN" > "$cg_dir/freezer.state" 2>/dev/null || {
            echo "    WARN: freeze write failed for PID $pid"
            return 1
        }
    fi

    # 验证冻结是否生效
    sleep 0.1
    if is_paused "$pid"; then
        echo "    OK: PID $pid frozen"
        return 0
    else
        echo "    WARN: PID $pid may not be fully frozen yet"
        return 0
    fi
}

# ============================================================
# 记录到日志
# ============================================================
log_process() {
    local pid=$1
    local user=$2
    local elapsed_h=$3
    local ppid=$4
    local starttime=$5
    local cmdline=$6
    local logfile=$7

    mkdir -p "$(dirname "$logfile")" 2>/dev/null || true

    local timestamp
    timestamp=$(date '+%Y-%m-%d %H:%M:%S')

    {
        echo "PID=$pid STARTTIME=$starttime USER=$user RUNTIME_H=${elapsed_h} PPID=$ppid TIMESTAMP=$timestamp"
        echo "  CMD: $cmdline"
    } >> "$logfile"
}

# ============================================================
# 清理遗留的 TS_GUARDIAN cgroup 目录 (PID 已死 / 目录为空)
# ============================================================
cleanup_guardian_cgroups() {
    local scan_dir
    if [[ $CGROUP_VER -eq 2 ]]; then
        scan_dir="/sys/fs/cgroup"
    else
        scan_dir="/sys/fs/cgroup/freezer"
    fi

    local cleaned=0
    for cg_dir in "$scan_dir"/TS_GUARDIAN_*; do
        [[ -d "$cg_dir" ]] || continue
        local dirname="${cg_dir##*/}"
        local pid="${dirname#TS_GUARDIAN_}"
        [[ "$pid" =~ ^[0-9]+$ ]] || continue

        # PID 不存活 → 清理
        if ! kill -0 "$pid" 2>/dev/null; then
            echo "[CLEANUP] PID $pid dead, removing $cg_dir"
            if [[ $CGROUP_VER -eq 2 ]]; then
                echo "0" > "$cg_dir/cgroup.freeze" 2>/dev/null || true
                rmdir "$cg_dir" 2>/dev/null || true
            else
                echo "THAWED" > "$cg_dir/freezer.state" 2>/dev/null || true
                rmdir "$cg_dir" 2>/dev/null || true
            fi
            cleaned=$(( cleaned + 1 ))
        fi
    done
    [[ $cleaned -gt 0 ]] && echo "[CLEANUP] removed $cleaned stale cgroup dir(s)"
}

# ============================================================
# 扫描单个用户
# ============================================================
scan_user() {
    local username=$1
    local uid
    uid=$(id -u "$username" 2>/dev/null) || {
        echo "[WARN] user '$username' not found on system, skipping"
        return
    }

    local logfile="$LOG_DIR/${username}.log"
    local threshold_sec=$(( RUNTIME_HOURS * 3600 ))

    echo "[SCAN] user=$username (UID=$uid), threshold=${RUNTIME_HOURS}h"

    local found=0
    local frozen=0

    # 遍历 /proc
    for pid_dir in /proc/[0-9]*; do
        local pid="${pid_dir##*/}"

        # 跳过自身
        [[ "$pid" == "$$" ]] && continue

        # 检查进程属主
        local proc_uid
        proc_uid=$(get_process_uid "$pid" 2>/dev/null) || continue
        [[ "$proc_uid" == "$uid" ]] || continue

        # 检查是否非系统程序
        is_non_system "$pid" || continue

        # 检查运行时间
        local elapsed
        elapsed=$(get_process_elapsed_sec "$pid" 2>/dev/null) || continue
        [[ "$elapsed" -gt "$threshold_sec" ]] || continue
        local elapsed_h=$(( elapsed / 3600 ))

        # 检查是否已暂停
        if is_paused "$pid"; then
            echo "  SKIP PID=$pid: already paused/frozen"
            continue
        fi

        # 检查父 PID 是否归 ts 管理
        if ppid_is_ts_managed "$pid"; then
            echo "  SKIP PID=$pid: PPID is managed by ts"
            continue
        fi

        # 日志去重 (PID + starttime)
        local starttime
        starttime=$(get_process_starttime "$pid" 2>/dev/null) || {
            echo "  SKIP PID=$pid: cannot read starttime"
            continue
        }
        if is_already_logged "$pid" "$starttime" "$logfile"; then
            echo "  SKIP PID=$pid: already in log (dedup)"
            continue
        fi

        # 获取信息用于日志
        local ppid
        ppid=$(get_ppid "$pid" 2>/dev/null || echo "?")
        local cmdline
        cmdline=$(get_process_cmdline "$pid")

        found=$(( found + 1 ))
        echo "  FOUND PID=$pid PPID=$ppid runtime=${elapsed_h}h cmd='$cmdline'"

        # 冻结进程
        if freeze_process "$pid"; then
            frozen=$(( frozen + 1 ))
        fi

        # 记录日志
        log_process "$pid" "$username" "$elapsed_h" "$ppid" "$starttime" "$cmdline" "$logfile"
    done

    echo "[DONE] user=$username: found=$found frozen=$frozen"
}

# ============================================================
# main
# ============================================================
main() {
    echo "=== TS Guardian $(date '+%Y-%m-%d %H:%M:%S') ==="
    echo "[CONF] $CONF"

    read_conf
    detect_cgroup_ver
    cleanup_guardian_cgroups

    for user in "${MONITOR_USERS[@]}"; do
        scan_user "$user"
    done

    echo "=== TS Guardian finished ==="
}

main "$@"

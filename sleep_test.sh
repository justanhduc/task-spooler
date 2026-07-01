#!/usr/bin/env bash

duration=${1:-10}

if ! [[ "$duration" =~ ^[0-9]+$ ]] || [ "$duration" -le 0 ]; then
    echo "Error: please provide a positive integer number of seconds."
    exit 1
fi

start=$(date +%s)

echo "Start CPU busy loop for ~${duration}s (wall time)"

while true; do
    now=$(date +%s)
    elapsed=$((now - start))

    # 输出状态（可选）
    printf "\rElapsed: %3ds | Remaining: %3ds" \
        "$elapsed" "$((duration - elapsed))"

    # 关键：这里做计算，占用CPU
    for ((i=0; i<100000; i++)); do
        :  # no-op
    done

    if (( elapsed >= duration )); then
        break
    fi
done

echo
echo "Done."

exit 0
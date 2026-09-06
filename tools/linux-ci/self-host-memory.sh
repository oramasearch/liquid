#!/usr/bin/env bash

set -u

if [ "$#" -lt 2 ]; then
    echo "usage: self-host-memory.sh <log> <command> [args...]" >&2
    exit 2
fi

log=$1
shift

descendants() {
    local parent=$1
    local child

    for child in $(ps -eo pid=,ppid= | awk -v parent="$parent" '$2 == parent { print $1 }'); do
        echo "$child"
        descendants "$child"
    done
}

sample() {
    local current
    local rss_kib=0
    local hwm_kib=0
    local total_rss_kib=0
    local total_hwm_kib=0

    printf 'timestamp=%s root_pid=%s\n' "$(date --iso-8601=seconds)" "$root_pid"
    printf 'memory.current=%s\n' "$(cat /sys/fs/cgroup/memory.current)"
    grep -E '^(anon|file|kernel|kernel_stack|shmem|slab) ' /sys/fs/cgroup/memory.stat
    for current in "$root_pid" $(descendants "$root_pid"); do
        if [ ! -r "/proc/$current/status" ]; then
            continue
        fi
        rss_kib=$(awk '/^VmRSS:/{ print $2 }' "/proc/$current/status")
        hwm_kib=$(awk '/^VmHWM:/{ print $2 }' "/proc/$current/status")
        rss_kib=${rss_kib:-0}
        hwm_kib=${hwm_kib:-0}
        total_rss_kib=$((total_rss_kib + rss_kib))
        total_hwm_kib=$((total_hwm_kib + hwm_kib))
        printf 'pid=%s vmrss_kib=%s vmhwm_kib=%s\n' "$current" "$rss_kib" "$hwm_kib"
    done
    printf 'process_rss_kib=%s process_hwm_kib=%s\n\n' "$total_rss_kib" "$total_hwm_kib"
}

"$@" &
root_pid=$!

while kill -0 "$root_pid" 2>/dev/null; do
    sample >>"$log"
    sleep 10
done

sample >>"$log"
wait "$root_pid"

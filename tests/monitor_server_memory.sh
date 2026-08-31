#!/bin/bash
set -uo pipefail
export LC_ALL=C

# Monitor whole-server CPU/memory and the API server process until STOP_FILE exists.
#
# Usage:
#   monitor_server_memory.sh OUTPUT_CSV STOP_FILE [INTERVAL] [PROCESS_NAME] \
#       [PID_FILE] [UDP_LISTEN_PORT]

if [ "$#" -lt 2 ] || [ "$#" -gt 6 ]; then
    echo "Usage: $0 OUTPUT_CSV STOP_FILE [INTERVAL] [PROCESS_NAME] [PID_FILE] [UDP_LISTEN_PORT]" >&2
    exit 2
fi

OUTPUT_CSV=$1
STOP_FILE=$2
INTERVAL=${3:-0.5}
PROCESS_NAME=${4:-mini_server}
PID_FILE=${5:--}
PROCESS_PORT=${6:-0}
stopping=0
sleep_pid=""

if ! awk -v value="$INTERVAL" '
    BEGIN {
        if (value ~ /^([0-9]+([.][0-9]*)?|[.][0-9]+)$/ && value + 0 > 0) {
            exit 0
        }
        exit 1
    }
'; then
    echo "INTERVAL must be a positive number: $INTERVAL" >&2
    exit 2
fi

case "$PROCESS_PORT" in
    ''|*[!0-9]*)
        echo "UDP_LISTEN_PORT must be a non-negative integer" >&2
        exit 2
        ;;
esac

handle_stop() {
    stopping=1
    if [ -n "${sleep_pid:-}" ]; then
        kill "$sleep_pid" 2>/dev/null || true
    fi
}

cleanup_control_file() {
    rm -f -- "$STOP_FILE"
}

trap handle_stop HUP INT TERM
trap cleanup_control_file EXIT

if ! mkdir -p "$(dirname "$OUTPUT_CSV")"; then
    echo "Unable to create output directory: $(dirname "$OUTPUT_CSV")" >&2
    exit 5
fi
if ! rm -f -- "$STOP_FILE"; then
    echo "Unable to remove stop file: $STOP_FILE" >&2
    exit 5
fi

read_uptime() {
    awk '{print $1}' /proc/uptime
}

read_cpu_snapshot() {
    awk '
        /^cpu / {
            total = 0
            for (i = 2; i <= NF; i++) {
                total += $i
            }
            idle = $5 + $6
            printf "%.0f %.0f\n", total, idle
            exit
        }
    ' /proc/stat
}

read_memory_snapshot() {
    awk '
        /^MemTotal:/     { total = $2 }
        /^MemAvailable:/ { available = $2 }
        /^MemFree:/      { free_kb = $2 }
        /^Buffers:/      { buffers = $2 }
        /^Cached:/       { cached = $2 }
        /^SReclaimable:/ { sreclaimable = $2 }
        /^Shmem:/        { shmem = $2 }
        END {
            if (available == 0) {
                available = free_kb + buffers + cached + sreclaimable - shmem
            }
            used = total - available
            if (used < 0) {
                used = 0
            }
            pct = total > 0 ? 100.0 * used / total : 0.0
            printf "%d %d %d %.3f %d %d\n",
                used, total, available, pct, cached, buffers
        }
    ' /proc/meminfo
}

resolve_server_pid() {
    candidate=""

    if [ "$PROCESS_PORT" -gt 0 ]; then
        candidate=$(
            ss -H -lunp 2>/dev/null \
                | sed -n "/:${PROCESS_PORT}[[:space:]]/s/.*pid=\([0-9][0-9]*\).*/\1/p" \
                | head -n 1
        )
        case "$candidate" in
            ''|*[!0-9]*) candidate="" ;;
        esac
        if [ -n "$candidate" ] \
            && [ -r "/proc/$candidate/status" ] \
            && [ -r "/proc/$candidate/stat" ] \
            && [ -r "/proc/$candidate/comm" ]; then
            read -r listener_name < "/proc/$candidate/comm" || listener_name=""
            expected_comm=$(printf '%.15s' "$PROCESS_NAME")
            if [ "$listener_name" = "$expected_comm" ]; then
                printf '%s\n' "$candidate"
                return
            fi
        fi
        printf '0\n'
        return
    fi

    # A supplied PID file is authoritative. Never fall back to a same-name
    # process if the tracked PID/start time is invalid, because that could
    # silently monitor an unrelated service.
    if [ "$PID_FILE" != "-" ]; then
        if [ ! -r "$PID_FILE" ]; then
            printf '0\n'
            return
        fi
        tracked_start=""
        read -r candidate tracked_start < "$PID_FILE" || candidate=""
        case "$candidate" in
            ''|*[!0-9]*) candidate="" ;;
        esac
        if [ -z "$candidate" ] \
            || [ ! -r "/proc/$candidate/status" ] \
            || [ ! -r "/proc/$candidate/stat" ]; then
            printf '0\n'
            return
        fi
        if [ -n "$tracked_start" ]; then
            current_start=$(awk '{print $22}' "/proc/$candidate/stat" 2>/dev/null || true)
            if [ "$current_start" != "$tracked_start" ]; then
                printf '0\n'
                return
            fi
        fi
        printf '%s\n' "$candidate"
        return
    fi

    if command -v pgrep >/dev/null 2>&1; then
        candidate=$(pgrep -n -x "$PROCESS_NAME" 2>/dev/null || true)
    fi
    if [ -z "$candidate" ] && command -v pidof >/dev/null 2>&1; then
        candidate=$(pidof "$PROCESS_NAME" 2>/dev/null | awk '{print $1}')
    fi
    printf '%s\n' "${candidate:-0}"
}

read_process_snapshot() {
    process_pid=$1
    if [ "$process_pid" = "0" ] \
        || [ ! -r "/proc/$process_pid/status" ] \
        || [ ! -r "/proc/$process_pid/stat" ]; then
        printf '0 0 0 0 0 0\n'
        return
    fi

    process_start=$(awk '{print $22}' "/proc/$process_pid/stat" 2>/dev/null) || {
        printf '0 0 0 0 0 0\n'
        return
    }
    memory_values=$(awk -v pid="$process_pid" '
        /^VmRSS:/  { rss = $2 }
        /^VmHWM:/  { hwm = $2 }
        /^VmSize:/ { vmsize = $2 }
        END {
            printf "%d %d %d %d", pid, rss + 0, hwm + 0, vmsize + 0
        }
    ' "/proc/$process_pid/status")
    process_ticks=$(awk '{ print $14 + $15 }' "/proc/$process_pid/stat")
    printf '%s %s %s\n' "$memory_values" "$process_start" "${process_ticks:-0}"
}

if ! printf '%s\n' \
    'elapsed_s,timestamp_utc,system_cpu_pct,system_used_kb,system_used_pct,mem_total_kb,mem_available_kb,cached_kb,buffers_kb,server_pid,server_process_cpu_pct,server_rss_kb,server_hwm_kb,server_vmsize_kb' \
    > "$OUTPUT_CSV"; then
    echo "Unable to initialize output CSV: $OUTPUT_CSV" >&2
    exit 5
fi

initial_server_pid=$(resolve_server_pid)
if [ "$initial_server_pid" = "0" ]; then
    if [ "$PROCESS_PORT" -gt 0 ]; then
        echo "Unable to resolve $PROCESS_NAME listening on UDP port $PROCESS_PORT" >&2
    else
        echo "Unable to resolve process $PROCESS_NAME" >&2
    fi
    exit 3
fi
echo "Monitoring pid=$initial_server_pid process=$PROCESS_NAME udp_port=$PROCESS_PORT" >&2

start_uptime=$(read_uptime)
clock_ticks_per_second=$(getconf CLK_TCK 2>/dev/null || true)
case "$clock_ticks_per_second" in
    ''|*[!0-9]*)
        echo "Unable to read a valid CLK_TCK value from getconf" >&2
        exit 4
        ;;
esac
if [ "$clock_ticks_per_second" -le 0 ]; then
    echo "CLK_TCK must be greater than zero: $clock_ticks_per_second" >&2
    exit 4
fi
previous_sample_uptime=$start_uptime
read -r previous_cpu_total previous_cpu_idle < <(read_cpu_snapshot)
read -r previous_server_pid _previous_rss _previous_hwm _previous_vmsize \
    initial_server_start previous_process_ticks \
    < <(read_process_snapshot "$initial_server_pid")
if [ "$previous_server_pid" != "$initial_server_pid" ] \
    || [ -z "$initial_server_start" ] \
    || [ "$initial_server_start" = "0" ]; then
    echo "Unable to read initial server process snapshot: PID=$initial_server_pid" >&2
    exit 3
fi
echo \
    "Process CPU baseline pid=$previous_server_pid start=$initial_server_start ticks=$previous_process_ticks clk_tck=$clock_ticks_per_second" \
    >&2

while [ "$stopping" -eq 0 ] && [ ! -e "$STOP_FILE" ]; do
    sleep "$INTERVAL" &
    sleep_pid=$!
    wait "$sleep_pid" 2>/dev/null || true
    sleep_pid=""

    if [ "$stopping" -ne 0 ] || [ -e "$STOP_FILE" ]; then
        break
    fi

    read -r used_kb total_kb available_kb used_pct cached_kb buffers_kb \
        < <(read_memory_snapshot)
    read -r current_cpu_total current_cpu_idle < <(read_cpu_snapshot)
    read -r server_pid rss_kb hwm_kb vmsize_kb server_start process_ticks \
        < <(read_process_snapshot "$initial_server_pid")
    current_uptime=$(read_uptime)

    system_cpu_pct=$(awk \
        -v t1="$previous_cpu_total" \
        -v i1="$previous_cpu_idle" \
        -v t2="$current_cpu_total" \
        -v i2="$current_cpu_idle" '
            BEGIN {
                dt = t2 - t1
                di = i2 - i1
                if (dt > 0) {
                    printf "%.3f", 100.0 * (dt - di) / dt
                } else {
                    printf "0.000"
                }
            }
        ')

    if [ "$server_pid" = "0" ] \
        || [ "$server_pid" != "$previous_server_pid" ] \
        || [ "$server_start" != "$initial_server_start" ]; then
        echo \
            "Process identity changed during sampling: previous_pid=$previous_server_pid current_pid=$server_pid initial_start=$initial_server_start current_start=$server_start" \
            >&2
        exit 4
    fi

    if ! server_process_cpu_pct=$(
        awk \
            -v p1="$previous_process_ticks" \
            -v p2="$process_ticks" \
            -v u1="$previous_sample_uptime" \
            -v u2="$current_uptime" \
            -v hz="$clock_ticks_per_second" '
                BEGIN {
                    dp = p2 - p1
                    elapsed = u2 - u1
                    if (dp < 0 || hz <= 0) {
                        exit 2
                    }
                    if (elapsed <= 0) {
                        printf "0.000"
                        exit
                    }
                    printf "%.3f", 100.0 * dp / (elapsed * hz)
                }
            '
    ); then
        echo \
            "Failed to calculate process CPU: pid=$server_pid p1=$previous_process_ticks p2=$process_ticks u1=$previous_sample_uptime u2=$current_uptime clk_tck=$clock_ticks_per_second" \
            >&2
        exit 4
    fi
    if [ -z "$server_process_cpu_pct" ]; then
        echo "Process CPU calculation returned an empty value for pid=$server_pid" >&2
        exit 4
    fi

    elapsed_s=$(awk -v start="$start_uptime" -v now="$current_uptime" \
        'BEGIN { printf "%.3f", now - start }')
    timestamp_utc=$(date -u '+%Y-%m-%dT%H:%M:%SZ')

    if ! printf '%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n' \
        "$elapsed_s" \
        "$timestamp_utc" \
        "$system_cpu_pct" \
        "$used_kb" \
        "$used_pct" \
        "$total_kb" \
        "$available_kb" \
        "$cached_kb" \
        "$buffers_kb" \
        "$server_pid" \
        "$server_process_cpu_pct" \
        "$rss_kb" \
        "$hwm_kb" \
        "$vmsize_kb" >> "$OUTPUT_CSV"; then
        echo "Unable to append resource sample: $OUTPUT_CSV" >&2
        exit 5
    fi

    previous_cpu_total=$current_cpu_total
    previous_cpu_idle=$current_cpu_idle
    previous_server_pid=$server_pid
    previous_process_ticks=$process_ticks
    previous_sample_uptime=$current_uptime

done

exit 0

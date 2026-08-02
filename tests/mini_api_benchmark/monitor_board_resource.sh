#!/bin/bash
set -uo pipefail
export LC_ALL=C

# Monitor only one API Demo client process until STOP_FILE exists.
#
# Usage:
#   monitor_board_resource.sh OUTPUT_CSV STOP_FILE [INTERVAL_SECONDS] CLIENT_PID

if [ "$#" -ne 4 ]; then
    echo "Usage: $0 OUTPUT_CSV STOP_FILE INTERVAL_SECONDS CLIENT_PID" >&2
    exit 2
fi

OUTPUT_CSV=$1
STOP_FILE=$2
INTERVAL=$3
CLIENT_PID=$4
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
    echo "INTERVAL_SECONDS must be a positive number: $INTERVAL" >&2
    exit 2
fi

case "$CLIENT_PID" in
    ''|*[!0-9]*)
        echo "CLIENT_PID must be a positive integer" >&2
        exit 2
        ;;
esac

if [ "$CLIENT_PID" -le 0 ]; then
    echo "CLIENT_PID must be greater than zero" >&2
    exit 2
fi

if [ ! -r "/proc/$CLIENT_PID/stat" ] || [ ! -r "/proc/$CLIENT_PID/status" ]; then
    echo "API Demo client process does not exist: PID=$CLIENT_PID" >&2
    exit 3
fi

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

read_process_snapshot() {
    process_pid=$1
    if [ ! -r "/proc/$process_pid/stat" ] \
        || [ ! -r "/proc/$process_pid/status" ]; then
        return 1
    fi

    process_start=$(awk '{print $22}' "/proc/$process_pid/stat" 2>/dev/null) || return 1
    process_ticks=$(awk '{print $14 + $15}' "/proc/$process_pid/stat" 2>/dev/null) || return 1
    memory_values=$(awk '
        /^VmRSS:/  { rss = $2 }
        /^VmHWM:/  { hwm = $2 }
        /^VmSize:/ { vmsize = $2 }
        END {
            printf "%d %d %d", rss + 0, hwm + 0, vmsize + 0
        }
    ' "/proc/$process_pid/status" 2>/dev/null) || return 1

    printf '%s %s %s\n' "$process_start" "$process_ticks" "$memory_values"
}

if ! printf '%s\n' \
    'elapsed_s,timestamp_utc,client_pid,client_process_cpu_pct,client_rss_kb,client_hwm_kb,client_vmsize_kb' \
    > "$OUTPUT_CSV"; then
    echo "Unable to initialize output CSV: $OUTPUT_CSV" >&2
    exit 5
fi

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

start_uptime=$(read_uptime)
previous_sample_uptime=$start_uptime
if ! read -r initial_start previous_process_ticks _rss _hwm _vmsize \
    < <(read_process_snapshot "$CLIENT_PID"); then
    echo "Unable to read initial API Demo process snapshot: PID=$CLIENT_PID" >&2
    exit 3
fi

echo \
    "Monitoring API Demo pid=$CLIENT_PID start=$initial_start clk_tck=$clock_ticks_per_second" \
    >&2

while [ "$stopping" -eq 0 ] && [ ! -e "$STOP_FILE" ]; do
    sleep "$INTERVAL" &
    sleep_pid=$!
    wait "$sleep_pid" 2>/dev/null || true
    sleep_pid=""

    if [ "$stopping" -ne 0 ] || [ -e "$STOP_FILE" ]; then
        break
    fi

    # The client exiting is a normal end condition. The caller still decides
    # whether the transfer itself succeeded from the client exit code/output.
    if ! read -r current_start process_ticks rss_kb hwm_kb vmsize_kb \
        < <(read_process_snapshot "$CLIENT_PID"); then
        break
    fi

    if [ "$current_start" != "$initial_start" ]; then
        echo \
            "Client PID was reused during sampling: pid=$CLIENT_PID initial_start=$initial_start current_start=$current_start" \
            >&2
        exit 4
    fi

    current_uptime=$(read_uptime)
    if ! client_process_cpu_pct=$(
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
                    # top-style process CPU. A multi-threaded process may
                    # legitimately exceed 100%% on a multi-core board.
                    printf "%.3f", 100.0 * dp / (elapsed * hz)
                }
            '
    ); then
        echo \
            "Failed to calculate client CPU: pid=$CLIENT_PID p1=$previous_process_ticks p2=$process_ticks u1=$previous_sample_uptime u2=$current_uptime clk_tck=$clock_ticks_per_second" \
            >&2
        exit 4
    fi

    elapsed_s=$(awk -v start="$start_uptime" -v now="$current_uptime" \
        'BEGIN { printf "%.3f", now - start }')
    timestamp_utc=$(date -u '+%Y-%m-%dT%H:%M:%SZ')

    if ! printf '%s,%s,%s,%s,%s,%s,%s\n' \
        "$elapsed_s" \
        "$timestamp_utc" \
        "$CLIENT_PID" \
        "$client_process_cpu_pct" \
        "$rss_kb" \
        "$hwm_kb" \
        "$vmsize_kb" >> "$OUTPUT_CSV"; then
        echo "Unable to append resource sample: $OUTPUT_CSV" >&2
        exit 5
    fi

    previous_process_ticks=$process_ticks
    previous_sample_uptime=$current_uptime
done

exit 0

#!/usr/bin/env bash

set -euo pipefail

if [[ $# -lt 4 || $# -gt 6 ]]; then
    echo "usage: $0 BUILD_DIR CERTIFICATE PRIVATE_KEY PORT [ROUNDS] [SIZE_MIB]" >&2
    exit 2
fi

build_dir=$1
certificate=$2
private_key=$3
port=$4
rounds=${5:-5}
size_mib=${6:-200}

client="${build_dir}/netins_udp_data_client"
server="${build_dir}/netins_udp_data_server"
if [[ ! -x "${client}" || ! -x "${server}" ]]; then
    echo "build the demos with -DNETINS_UDP_BUILD_DATA_DEMO=ON first" >&2
    exit 2
fi
if [[ ! -f "${certificate}" || ! -f "${private_key}" ]]; then
    echo "certificate or private key does not exist" >&2
    exit 2
fi
if ! [[ "${port}" =~ ^[0-9]+$ && "${port}" -gt 0 && "${port}" -le 65535 ]]; then
    echo "invalid port: ${port}" >&2
    exit 2
fi
if ! [[ "${rounds}" =~ ^[0-9]+$ && "${rounds}" -gt 0 ]]; then
    echo "invalid rounds: ${rounds}" >&2
    exit 2
fi
if ! [[ "${size_mib}" =~ ^[0-9]+$ && "${size_mib}" -gt 0 ]]; then
    echo "invalid size: ${size_mib}" >&2
    exit 2
fi

work_dir=$(mktemp -d "${TMPDIR:-/tmp}/netins-udp-api-stress.XXXXXX")
echo "stress artifacts: ${work_dir}"

client_send="${work_dir}/client-send.bin"
server_send="${work_dir}/server-send.bin"
dd if=/dev/zero of="${client_send}" bs=1M count="${size_mib}" status=none
dd if=/dev/zero of="${server_send}" bs=1M count="${size_mib}" status=none
printf '\x43' | dd of="${client_send}" bs=1 seek=0 conv=notrunc status=none
printf '\x53' | dd of="${server_send}" bs=1 seek=0 conv=notrunc status=none

for ((round = 1; round <= rounds; ++round)); do
    server_received="${work_dir}/server-received-${round}.bin"
    client_received="${work_dir}/client-received-${round}.bin"
    server_log="${work_dir}/server-${round}.log"
    client_log="${work_dir}/client-${round}.log"

    timeout 300 "${server}" 127.0.0.1 "${port}" \
        "${certificate}" "${private_key}" \
        "${server_received}" "${server_send}" \
        >"${server_log}" 2>&1 &
    server_pid=$!

    sleep 0.2
    if ! timeout 300 "${client}" 127.0.0.1 "${port}" 127.0.0.1 - \
        "${client_send}" "${client_received}" \
        >"${client_log}" 2>&1
    then
        wait "${server_pid}" || true
        echo "round ${round}: client failed; see ${client_log}" >&2
        exit 1
    fi
    if ! wait "${server_pid}"; then
        echo "round ${round}: server failed; see ${server_log}" >&2
        exit 1
    fi
    cmp "${client_send}" "${server_received}"
    cmp "${server_send}" "${client_received}"
    echo "round ${round}/${rounds}: PASS"
done

echo "NetIns UDP public API stress: PASS"

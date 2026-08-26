#!/bin/sh
set -eu

if [ "$#" -ne 2 ]; then
    echo "Usage: $0 BUILD_DIR OUTPUT_DIR" >&2
    exit 2
fi

build_dir=$1
output_dir=$2
repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
version=$(tr -d '\r\n' < "$repo_dir/VERSION")
stage_dir=$output_dir/netins-mini-api-$version
archive=$output_dir/netins-mini-api-$version.tar.gz

if [ ! -d "$build_dir" ]; then
    echo "Build directory not found: $build_dir" >&2
    exit 1
fi

mkdir -p "$output_dir"
rm -rf "$stage_dir"
cmake --install "$build_dir" --prefix "$stage_dir" --component mini_api

required_files="
include/xquic/netins_udp.h
lib/libxquic-static.a
lib/libxquic.so
bin/netins_udp_mini_api_client
bin/netins_udp_mini_api_server
bin/netins_udp_stream_client
bin/netins_udp_stream_server
bin/mini_stream_protocol_test
bin/mini_stream_reorder_test
bin/netins_udp_stream_api_contract_test
README.md
NETINS_UDP_API_使用说明.md
NETINS_UDP_编译指南.md
MINI_API_测试使用说明.md
NETINS_UDP_STREAM_API_阶段0与1设计.md
NETINS_UDP_STREAM_API_阶段2与3实现.md
NETINS_UDP_STREAM_API_阶段4至7交付与验收.md
tests/stream_api_acceptance.py
scripts/mini_api/check_abi.py
scripts/mini_api/check_stream_isolation.py
scripts/xquic.lds
VERSION
"

for relative_path in $required_files; do
    if [ ! -e "$stage_dir/$relative_path" ]; then
        echo "Missing delivery artifact: $relative_path" >&2
        exit 1
    fi
done

python3 "$repo_dir/scripts/mini_api/check_stream_isolation.py" \
    --root "$repo_dir"
python3 "$repo_dir/scripts/mini_api/check_abi.py" \
    --header "$stage_dir/include/xquic/netins_udp.h" \
    --version-script "$stage_dir/scripts/xquic.lds" \
    --library "$stage_dir/lib/libxquic.so"

rm -f "$archive"
tar -C "$output_dir" -czf "$archive" "netins-mini-api-$version"
sha256sum "$archive" > "$archive.sha256"
echo "Delivery package: $archive"
echo "Checksum: $archive.sha256"

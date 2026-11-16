#!/bin/sh
set -eu

root_dir=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
cd "$root_dir"

mode=${1:-all}
if [ "$#" -gt 0 ]; then
  shift
fi

case "$mode" in
  all)
    "$root_dir/scripts/build.sh" all
    ./build/RingBufferTest
    ./build/log_demo 5 build/log.bin
    ./build/buffer_pool_test
    (
      cd build
      ./ring_pool_example
    )
    ;;
  tests)
    "$root_dir/scripts/build.sh" tests
    ./build/RingBufferTest
    ./build/log_demo 5 build/log.bin
    ./build/buffer_pool_test
    ;;
  demo)
    "$root_dir/scripts/build.sh" cbuf
    ./build/log_demo "${1:-5}" "${2:-build/log.bin}"
    ;;
  example)
    "$root_dir/scripts/build.sh" example
    (
      cd build
      ./ring_pool_example
    )
    ;;
  ring-pool)
    "$root_dir/scripts/build.sh" ring-pool
    ./build/buffer_pool_test
    (
      cd build
      ./ring_pool_example
    )
    ;;
  *)
    printf 'usage: %s [all|tests|demo [seconds] [output-file]|example|ring-pool]\n' "$0" >&2
    exit 2
    ;;
esac

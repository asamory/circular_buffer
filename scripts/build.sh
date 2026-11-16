#!/bin/sh
set -eu

root_dir=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
cd "$root_dir"

case "${1:-all}" in
  all)
    exec make all
    ;;
  cbuf)
    exec make build/RingBufferTest build/log_demo
    ;;
  tests)
    exec make build/RingBufferTest build/log_demo build/buffer_pool_test
    ;;
  ring-pool)
    exec make build/buffer_pool_test build/ring_pool_example
    ;;
  example)
    exec make build/ring_pool_example
    ;;
  *)
    printf 'usage: %s [all|cbuf|tests|ring-pool|example]\n' "$0" >&2
    exit 2
    ;;
esac

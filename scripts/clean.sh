#!/bin/sh
set -eu

root_dir=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
cd "$root_dir"

make clean
rm -f build/fault_capture_*.csv

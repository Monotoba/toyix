#!/usr/bin/env bash
set -euo pipefail

make clean
make test
make test-exception

echo "All smoke checks passed."

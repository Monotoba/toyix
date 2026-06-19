#!/usr/bin/env bash
set -euo pipefail

make clean
make test

echo "All smoke checks passed."

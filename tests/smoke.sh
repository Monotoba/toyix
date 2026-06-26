#!/usr/bin/env bash
set -euo pipefail

make clean
make test
make test-exception
make test-page-fault

echo "All Chapter 18 checks passed."

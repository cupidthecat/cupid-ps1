#!/usr/bin/env bash
# Build and run the cupid-ps1 test binary.  Optional first arg = test filter
# substring (e.g. "Path" runs all tests containing "Path" in suite or name).
set -eu
cd "$(dirname "$0")/.."
make -s test
status=$?
exit $status

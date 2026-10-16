#!/usr/bin/env bash
# Build and run the cupid-ps1 test binary.  Optional first arg = test filter
# substring (e.g. "Path" runs all tests containing "Path" in suite or name).
set -eu
cd "$(dirname "$0")/.."
filter=${1-}
if [ "$#" -gt 0 ]; then
  shift
fi
exec make -s test TEST_FILTER="$filter" "$@"

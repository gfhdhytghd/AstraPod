#!/bin/sh
# Offline Rust protocol, real C serial and screen checks; no device access.
set -eu
cd "$(dirname "$0")/../.."
"${PYTHON:-python3}" tools/tests/run_podlink_rust.py --build "${PODLINK_TEST_BUILD:-build-fu49}"
"${PYTHON:-python3}" -m unittest discover -s tools/tests -p test_podlink.py -v

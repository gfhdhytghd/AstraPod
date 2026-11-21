#!/bin/sh
# Offline Rust protocol, real C serial and screen checks; no device access.
# Client and Python/Rust parity tests live in AstraPod-Desktop.
set -eu
cd "$(dirname "$0")/../.."
"${PYTHON:-python3}" tools/tests/podlink_recording_test.py \
    "${PODLINK_TEST_BUILD:-build-fu49}/host/recording"
"${PYTHON:-python3}" tools/tests/run_podlink_rust.py --build "${PODLINK_TEST_BUILD:-build-fu49}"

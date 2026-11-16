#!/usr/bin/env bash
# Runs the ixian-server test-suite against the mock engine build (tools/server/build-mock by default).
#
#   tools/server/tests/tests.sh                 # everything but the slow tests
#   SLOW_TESTS=1 tools/server/tests/tests.sh    # also the slow ones
#   tools/server/tests/tests.sh -k chat -x      # pytest arguments pass through
#
# IXIAN_SERVER_BIN_PATH overrides the server binary, IXIAN_MODEL_DIR the model package (a real one on the IQ9).
set -eu
cd "$(dirname "${BASH_SOURCE[0]}")"

if [ $# -lt 1 ]; then
    pytest -v -x -m "not slow" -n "${IXIAN_TEST_WORKERS:-4}"
else
    pytest -v -x "$@"
fi

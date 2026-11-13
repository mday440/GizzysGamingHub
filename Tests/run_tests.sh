#!/bin/bash
# Build and run the core regression tests (works on macOS and Linux).
set -e
cd "$(dirname "$0")/.."
cc -std=c11 -D_DEFAULT_SOURCE -O2 -I Sources/MGRCore/include Sources/MGRCore/*.c Tests/core_tests.c -o /tmp/mgr_core_tests -lm
/tmp/mgr_core_tests Tests/fixtures

#!/bin/sh
set -eu
root=$(git rev-parse --show-toplevel)
exec python3 "$root/tools/dev/quality.py" install

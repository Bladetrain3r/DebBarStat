#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
if [ -n "${DEBBARSTAT_PLATFORM:-}" ]; then
  set -- --platform "$DEBBARSTAT_PLATFORM"
else
  set --
fi
docker build "$@" -f Dockerfile.build -t debbarstat-build:local .
docker run --rm "$@" --user "$(id -u):$(id -g)" -v "$(pwd):/src" \
  debbarstat-build:local make clean release
printf 'Built dist/debbarstat\n'

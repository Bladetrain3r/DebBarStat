#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
docker build -f Dockerfile.build -t debbarstat-build:local .
docker run --rm --user "$(id -u):$(id -g)" -v "$(pwd):/src" \
  debbarstat-build:local make release
printf 'Built dist/debbarstat\n'

#!/bin/sh
set -eu
binary=$1
fixture=$(mktemp -d)
trap 'rm -rf "$fixture"' EXIT
printf '1234' > "$fixture/a"
summary=$($binary --summary "$fixture")
printf '%s\n' "$summary" | grep -q '^Bytes: 4$'
printf '%s\n' "$summary" | grep -q '^Files: 1$'
printf '%s\n' "$summary" | grep -q '^Errors: 0$'
$binary --help | grep -q '^Usage:'
printf 'CLI tests passed\n'

#!/bin/sh
set -eu
binary=$1
fixture=$(mktemp -d ./build/debbarstat-cli-XXXXXX)
trap 'rm -rf "$fixture"' EXIT
mkdir "$fixture/sub"
printf '1234' > "$fixture/a"
ln "$fixture/a" "$fixture/sub/a-hardlink"
printf 'abc' > "$fixture/sub/b"
truncate -s 16777216 "$fixture/sparse"
ln -s a "$fixture/link"

allocated=$($binary --summary "$fixture")
apparent=$($binary --apparent-size --summary "$fixture")
allocated_bytes=$(printf '%s\n' "$allocated" | sed -n 's/^Bytes: //p')
apparent_bytes=$(printf '%s\n' "$apparent" | sed -n 's/^Bytes: //p')
du_allocated=$(du -s -B1 "$fixture" | cut -f1)
du_apparent=$(du --apparent-size -s -B1 "$fixture" | cut -f1)
test "$allocated_bytes" = "$du_allocated"
test "$apparent_bytes" = "$du_apparent"
printf '%s\n' "$allocated" | grep -q '^Hard-link copies: 1$'
printf '%s\n' "$allocated" | grep -q '^Errors: 0$'
$binary --help | grep -q '^Usage:'
printf 'CLI tests passed\n'

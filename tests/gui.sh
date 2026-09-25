#!/bin/sh
set -eu
binary=$1
clicker=$2
fixture=$(mktemp -d ./build/debbarstat-gui-XXXXXX)
trap 'rm -rf "$fixture"' EXIT
mkdir "$fixture/data" "$fixture/bin"
printf 'abcd' > "$fixture/data/file.txt"
cat > "$fixture/bin/xdg-open" <<'EOF'
#!/bin/sh
printf '%s\n' "$1" > "$DEBBARSTAT_OPEN_LOG"
EOF
chmod +x "$fixture/bin/xdg-open"
run_case() {
    scan_root=$(realpath "$1")
    expected=$(realpath "$2")
    log=$(realpath -m "$3")
    DEBBARSTAT_OPEN_LOG="$log" PATH="$(realpath "$fixture/bin"):$PATH" \
        xvfb-run -a sh -c '
        "$1" "$2" >/dev/null 2>"$3/app.log" &
        app=$!
        "$4"
        count=0
        while test ! -f "$5" && test "$count" -lt 50; do
            sleep 0.1
            count=$((count + 1))
        done
        kill "$app" 2>/dev/null || true
        wait "$app" 2>/dev/null || true
        test -f "$5"
    ' sh "$(realpath "$binary")" "$scan_root" "$(realpath "$fixture")" \
      "$(realpath "$clicker")" "$log"
    test "$(cat "$log")" = "$expected"
}
run_case "$fixture/data" "$fixture/data" "$fixture/open-file.log"
mkdir "$fixture/foldercase" "$fixture/foldercase/sub"
printf 'abcdef' > "$fixture/foldercase/sub/file.txt"
run_case "$fixture/foldercase" "$fixture/foldercase/sub" "$fixture/open-dir.log"
mkdir "$fixture/exportcase"
printf 'hello' > "$fixture/exportcase/file.txt"
xvfb-run -a sh -c '
    cd "$3"
    "$1" "$2" >/dev/null 2>app.log &
    app=$!
    "$4" export
    count=0
    while ! ls debbarstat-*.csv >/dev/null 2>&1 && test "$count" -lt 50; do
        sleep 0.1
        count=$((count + 1))
    done
    kill "$app" 2>/dev/null || true
    wait "$app" 2>/dev/null || true
    head -n 1 debbarstat-*.csv | grep -q "^path,kind,self_bytes"
' sh "$(realpath "$binary")" "$(realpath "$fixture/exportcase")" \
  "$(realpath "$fixture")" "$(realpath "$clicker")"
printf 'GUI Ctrl+click and export tests passed\n'

# DebBarStat

DebBarStat is a small Debian disk visualizer inspired by WinDirStat. It scans a
directory and shows a size-sorted entry list beside a colored treemap. This is an
independent implementation under the MIT license.

## Build and run

On a machine with Docker:

```sh
./scripts/container-build.sh
./dist/debbarstat "$HOME"
```

The build and tests run inside a Debian Bookworm container. The resulting
`dist/debbarstat` is a single executable with no asset directory. It links to
Debian's standard X11 client library (`libX11.so.6`) and glibc; an X server or
Xwayland display is needed to open the window. It is built for the architecture
of the build host. Run `ldd dist/debbarstat` to inspect runtime libraries.

For a local development build, install `build-essential`, `libx11-dev`, `xvfb`
and `xauth`, then run `make check` or `make release`.

## Use

```sh
debbarstat [--all-filesystems] [--apparent-size] [--summary | --export CSV] [PATH]
```

`PATH` defaults to the current directory. The default scan counts allocated
bytes from `st_blocks`, including directory metadata, like `du -B1`. Each
hard-linked inode is charged once. One path receives its size; the other paths
show `0` bytes and an `=` marker. Which path receives the size depends on scan
order. `--apparent-size` counts logical sizes instead, like
`du --apparent-size -B1`; it still counts hard links once. This option does
not make scanning faster because both size fields come from the same `stat`
call. Symlinks are never followed. By default,
mounted filesystems below the selected directory are skipped; pass
`--all-filesystems` to include other disk filesystems. Kernel and memory backed
filesystems, including `/proc`, `/sys`, `/dev`, and tmpfs mounts, are always
skipped. Starting a scan directly on one of them is rejected. Unreadable or
vanished entries are skipped and counted as errors. `--summary` prints totals
without opening a window and exits with status 3 when the scan has errors.

`--export report.csv` writes CSV without opening a window. The report contains
each path, its kind, its own bytes, its inclusive subtree total, duplicate
hard-link status, size mode, and scan error count. Existing reports are never
overwritten. Use `--export -` to write CSV to standard output. A report with
scan errors is still written, and the command exits with status 3.

Click a directory in the left list to zoom in. Click a treemap tile to select
it; double-click to zoom into its containing directory. Right-click or press
Backspace/Left to go up. The mouse wheel scrolls the entry list; arrow keys,
Home, and End select entries; Enter opens a selected directory. A switches
between allocated and apparent sizes and rescans; R rescans; Q/Escape quits.
Ctrl+click a directory to open it in your file manager, or Ctrl+click a file
to open its containing folder. This uses `xdg-open` from your desktop session.
Press E to save a timestamped CSV report of the full scan in the current working
directory. Tile colors group filenames by extension hash.

## Current scope

The first release supports scan, size sorting, treemap navigation, rescan and
CLI totals and CSV export. The scanner and report writer are separate from the
X11 interface so a future terminal interface can be a second standalone binary.
The desktop app leaves deletion to your file manager.

Copyright (c) 2026 NuCode contributors. MIT; see [LICENSE](LICENSE).

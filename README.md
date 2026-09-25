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
debbarstat [--all-filesystems] [--summary] [PATH]
```

`PATH` defaults to the current directory. The scan counts logical file bytes.
Symlinks are shown as zero-size entries and never followed. By default,
mounted filesystems below the selected directory are skipped; pass
`--all-filesystems` to include them. Unreadable or vanished entries are skipped
and counted as errors. `--summary` prints totals without opening a window and
exits with status 3 when the scan has errors.

Click a directory in the left list to zoom in. Click a treemap tile to select
it; double-click to zoom into its containing directory. Right-click or press
Backspace/Left to go up. The mouse wheel scrolls the entry list; R rescans;
Q/Escape quits. Tile colors group filenames by extension hash.

## Current scope

The first release supports scan, size sorting, treemap navigation, rescan and
CLI totals. Planned follow-ups include an extension legend, keyboard-first
navigation, allocation-size mode, and a terminal interface. It intentionally
has no delete action yet.

Copyright (c) 2026 NuCode contributors. MIT; see [LICENSE](LICENSE).

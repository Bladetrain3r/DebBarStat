CC ?= cc
CFLAGS ?= -O2 -g -Wall -Wextra -Wpedantic -std=c11
CPPFLAGS ?= -D_DEFAULT_SOURCE
LDFLAGS ?=
LDLIBS = -lX11

SRC = src/main.c src/scan.c src/report.c

.PHONY: all clean check release

all: build/debbarstat

build:
	mkdir -p build

build/debbarstat: $(SRC) src/scan.h src/report.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SRC) $(LDFLAGS) $(LDLIBS) -o $@

build/test_scan: tests/test_scan.c src/scan.c src/scan.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -Isrc tests/test_scan.c src/scan.c $(LDFLAGS) -o $@

build/gui_click: tests/gui_click.c | build
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/gui_click.c $(LDFLAGS) $(LDLIBS) -o $@

check: build/debbarstat build/test_scan build/gui_click
	./build/test_scan
	sh tests/cli.sh ./build/debbarstat
	sh tests/gui.sh ./build/debbarstat ./build/gui_click

release: check
	mkdir -p dist
	$(CC) $(CPPFLAGS) -O2 -DNDEBUG -std=c11 $(SRC) -s $(LDLIBS) -o dist/debbarstat

clean:
	rm -rf build dist

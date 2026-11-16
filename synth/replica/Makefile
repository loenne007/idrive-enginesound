# One sound engine; its id is this folder's name. make host: build/es-synth-host (tests, renders);
# make car: the head unit's binary tool/lfb-enginesound/es-synth-<id> (x86-64, glibc <= 2.27, loader in /lib).
CC ?= cc
ZIG ?= $(HOME)/.local/zig/zig-aarch64-macos-0.17.1/zig
SRC = main.c synth.c presets.c out_alsa.c engine.c fx.c wa.c
HDR = synth.h engine.h wa.h out_alsa.h ../common/protect.h
FLAGS = -std=c99 -O2 -Wall -Wextra -Wpedantic -I../common
ENGINE := $(notdir $(CURDIR))
OUT := ../../tool/lfb-enginesound/es-synth-$(ENGINE)
.PHONY: host car clean
host: build/es-synth-host
build/es-synth-host: $(SRC) $(HDR) Makefile
	mkdir -p build
	$(CC) $(FLAGS) $(SRC) -o $@ -lm
car: build/es-synth
	cp build/es-synth $(OUT)
	file $(OUT)
	python3 ../../tools/check_glibc.py $(OUT)
# the head unit has no /lib64: point the ELF interpreter at /lib
build/es-synth: $(SRC) $(HDR) Makefile
	mkdir -p build
	$(ZIG) cc -target x86_64-linux-gnu.2.27 $(FLAGS) -s $(SRC) -o $@ -ldl -lm -Wl,--dynamic-linker=/lib/ld-linux-x86-64.so.2
clean:
	rm -rf build

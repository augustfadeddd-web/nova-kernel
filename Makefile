HOST_OS := $(shell uname -s)
PYTHON ?= python3
ifeq ($(HOST_OS),Darwin)
CC := clang
LD := ld.lld
ARCH_FLAGS := -target x86_64-elf
else
CC := gcc
LD := ld
ARCH_FLAGS := -m64
endif
CFLAGS := $(ARCH_FLAGS) -std=gnu11 -ffreestanding -fno-pie -fno-stack-protector -fno-builtin -fno-asynchronous-unwind-tables -fno-unwind-tables -mno-mmx -msse -msse2 -mno-red-zone -mcmodel=kernel -O2 -Wall -Wextra -Werror -I src/libc
OBJECTS := $(addprefix build/doom/,$(addsuffix .o,$(DOOM_SRC)))
# DOOM is third-party code: warnings are shown but not fatal (-Werror stays on
# Nova's own files). The game is fixed 960x600 and uses our libc headers.
DOOM_CFLAGS := $(ARCH_FLAGS) -std=gnu11 -ffreestanding -fno-pie -fno-stack-protector \
               -fno-builtin -fno-asynchronous-unwind-tables -fno-unwind-tables \
               -mno-mmx -msse -msse2 -mno-red-zone -mcmodel=kernel -O2 -Wall -Wextra \
               -DNORMALUNIX -DLINUX -DSNDSERV -D_DEFAULT_SOURCE \
               -DDOOMGENERIC_RESX=960 -DDOOMGENERIC_RESY=600 \
               -I $(DOOM_DIR) -I src/libc

OBJECTS := build/process.o build/pages.o build/user_arch.o build/visual_assets.o build/boot.o build/kernel.o build/graphics.o build/terminal.o build/input.o  build/system.o build/boot_screen.o build/desktop.o build/apps.o build/network.o  \
           build/ata.o build/fat32.o build/login.o \
           build/minilibc.o build/setjmp.o  build/compiler_rt.o

.PHONY: all clean
all: build/kernel.elf
build:
	mkdir -p build
build/%.o: src/%.c $(wildcard src/*.h) $(wildcard src/libc/*.h) | build
	$(CC) $(CFLAGS) -c $< -o $@
build/%.o: src/%.S | build
	$(CC) $(ARCH_FLAGS) -ffreestanding -fno-pie -I $(CURDIR) -c $< -o $@
build/visual_assets.o: assets/font-aa.bin assets/au-logo.bin assets/topo-aa.bin
build/kernel.elf: $(OBJECTS) linker.ld
	$(LD) -m elf_x86_64 --build-id=none -T linker.ld -o $@ $(OBJECTS)
	$(PYTHON) scripts/check_multiboot.py --is-x86-multiboot $@
clean:
	rm -rf build

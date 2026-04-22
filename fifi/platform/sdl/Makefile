CC    := gcc
OUT   := fifi-compositor-sdl
BUILD := ../../../build-sdl

SDL_CFLAGS := $(shell sdl2-config --cflags)
SDL_LIBS   := $(shell sdl2-config --libs)

INCLUDES := \
    -I../linux/include \
    -I../../../kernel/include

CFLAGS := \
    -std=c11 -O3 -march=native -pipe -Wall -Wextra \
    -Wno-unused-parameter -Wno-misleading-indentation \
    -D_GNU_SOURCE -DVFS_ROOT='"fifi-data"' \
    $(SDL_CFLAGS) \
    $(INCLUDES)

OBJS := \
    $(BUILD)/sdl/main_sdl.o \
    $(BUILD)/sdl/input_sdl.o \
    $(BUILD)/sdl/platform.o \
    $(BUILD)/sdl/pty.o \
    $(BUILD)/sdl/vfs_posix.o \
    $(BUILD)/sdl/gui.o \
    $(BUILD)/sdl/console.o

.PHONY: all clean

all: $(BUILD)/sdl $(BUILD)/$(OUT)

$(BUILD)/sdl:
	mkdir -p $@

$(BUILD)/sdl/main_sdl.o: main_sdl.c | $(BUILD)/sdl
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/sdl/input_sdl.o: input_sdl.c | $(BUILD)/sdl
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/sdl/platform.o: ../linux/platform.c | $(BUILD)/sdl
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/sdl/pty.o: ../linux/pty.c | $(BUILD)/sdl
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/sdl/vfs_posix.o: ../linux/vfs_posix.c | $(BUILD)/sdl
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/sdl/gui.o: ../../../kernel/src/gui.c | $(BUILD)/sdl
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/sdl/console.o: ../../../kernel/src/console.c | $(BUILD)/sdl
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/$(OUT): $(OBJS)
	$(CC) $(OBJS) $(SDL_LIBS) -o $@
	@echo "[sdl] built: $@"

clean:
	rm -rf $(BUILD)/sdl $(BUILD)/$(OUT)

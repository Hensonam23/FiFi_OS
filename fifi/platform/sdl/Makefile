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
    $(BUILD)/sdl/audio_sdl_stub.o \
    $(BUILD)/sdl/services_sdl_stub.o \
    $(BUILD)/sdl/gui.o \
    $(BUILD)/sdl/gui_render.o \
    $(BUILD)/sdl/gui_window.o \
    $(BUILD)/sdl/gui_taskbar.o \
    $(BUILD)/sdl/gui_widgets.o \
    $(BUILD)/sdl/gui_settings.o \
    $(BUILD)/sdl/gui_files.o \
    $(BUILD)/sdl/gui_text.o \
    $(BUILD)/sdl/gui_text_util.o \
    $(BUILD)/sdl/png.o \
    $(BUILD)/sdl/font_ttf.o \
    $(BUILD)/sdl/lodepng.o \
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

$(BUILD)/sdl/audio_sdl_stub.o: audio_sdl_stub.c | $(BUILD)/sdl
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/sdl/services_sdl_stub.o: services_sdl_stub.c | $(BUILD)/sdl
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/sdl/gui.o: ../../../kernel/src/gui.c | $(BUILD)/sdl
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/sdl/gui_%.o: ../../../kernel/src/gui_%.c | $(BUILD)/sdl
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/sdl/png.o: ../linux/png.c | $(BUILD)/sdl
	$(CC) $(CFLAGS) -I../linux/vendor -c $< -o $@

$(BUILD)/sdl/font_ttf.o: ../linux/font_ttf.c | $(BUILD)/sdl
	$(CC) $(CFLAGS) -I../linux/vendor -c $< -o $@

$(BUILD)/sdl/lodepng.o: ../linux/vendor/lodepng.c | $(BUILD)/sdl
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/sdl/console.o: ../../../kernel/src/console.c | $(BUILD)/sdl
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/$(OUT): $(OBJS)
	$(CC) $(OBJS) $(SDL_LIBS) -lm -o $@
	@echo "[sdl] built: $@"

clean:
	rm -rf $(BUILD)/sdl $(BUILD)/$(OUT)

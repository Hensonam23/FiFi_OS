CC      := gcc
OUT     := fifi-compositor
BUILD   := ../../build-linux
.DEFAULT_GOAL := all

# Shadow headers must come FIRST to override kernel headers
INCLUDES := \
    -I../platform/linux/include \
    -I../../kernel/include \
    -I../../kernel/src

CFLAGS := \
    -std=c11 -O2 -march=x86-64 -pipe -Wall -Wextra \
    -Wno-unused-parameter \
    -Wno-misleading-indentation \
    -D_GNU_SOURCE \
    $(INCLUDES)

LIBINPUT_CFLAGS := $(shell pkg-config --cflags libinput 2>/dev/null)
LIBINPUT_LIBS   := $(shell pkg-config --libs libinput 2>/dev/null)
CFLAGS          += $(LIBINPUT_CFLAGS)

SRCS := \
    main.c \
    ../platform/linux/platform.c \
    ../platform/linux/input.c \
    ../platform/linux/pty.c \
    ../platform/linux/vfs_posix.c \
    ../platform/linux/drm.c \
    ../platform/linux/audio.c \
    ../platform/linux/ipc.c \
    ../platform/linux/wayland.c \
    ../platform/linux/xwm.c \
    ../platform/linux/png.c \
    ../platform/linux/font_ttf.c \
    ../platform/linux/vendor/lodepng.c \
    ../../kernel/src/gui.c \
    ../../kernel/src/gui_text_util.c \
    ../../kernel/src/gui_window.c \
    ../../kernel/src/gui_taskbar.c \
    ../../kernel/src/gui_widgets.c \
    ../../kernel/src/gui_settings.c \
    ../../kernel/src/gui_files.c \
    ../../kernel/src/gui_text.c \
    ../../kernel/src/gui_render.c \
    ../../kernel/src/console.c

OBJS := $(patsubst %.c,$(BUILD)/comp/%.o,$(notdir $(SRCS)))

$(OBJS): ../shared/theme.h

.PHONY: all clean

all: $(BUILD)/$(OUT)

$(BUILD)/comp:
	mkdir -p $@

# Pattern rule for compositor source files
$(BUILD)/comp/%.o: %.c | $(BUILD)/comp
	$(CC) $(CFLAGS) -c $< -o $@

# Platform files
$(BUILD)/comp/platform.o: ../platform/linux/platform.c | $(BUILD)/comp
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/comp/input.o: ../platform/linux/input.c ../platform/linux/include/touchpad_axis.h ../platform/linux/include/touchpad_motion.h | $(BUILD)/comp
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/comp/vfs_posix.o: ../platform/linux/vfs_posix.c | $(BUILD)/comp
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/comp/pty.o: ../platform/linux/pty.c | $(BUILD)/comp
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/comp/drm.o: ../platform/linux/drm.c | $(BUILD)/comp
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/comp/audio.o: ../platform/linux/audio.c | $(BUILD)/comp
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/comp/ipc.o: ../platform/linux/ipc.c ../shared/ipc.h | $(BUILD)/comp
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/comp/wayland.o: ../platform/linux/wayland.c | $(BUILD)/comp
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/comp/xwm.o: ../platform/linux/xwm.c | $(BUILD)/comp
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/comp/png.o: ../platform/linux/png.c | $(BUILD)/comp
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/comp/lodepng.o: ../platform/linux/vendor/lodepng.c | $(BUILD)/comp
	$(CC) $(CFLAGS) -Wno-all -c $< -o $@

# stb_truetype implementation lives here — silence its internal warnings
$(BUILD)/comp/font_ttf.o: ../platform/linux/font_ttf.c | $(BUILD)/comp
	$(CC) $(CFLAGS) -Wno-all -Wno-extra -c $< -o $@

# Kernel source files (compiled with Linux stubs)
$(BUILD)/comp/gui.o: ../../kernel/src/gui.c ../shared/theme.h | $(BUILD)/comp
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/comp/gui_text_util.o: ../../kernel/src/gui_text_util.c | $(BUILD)/comp
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/comp/gui_window.o: ../../kernel/src/gui_window.c | $(BUILD)/comp
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/comp/gui_taskbar.o: ../../kernel/src/gui_taskbar.c | $(BUILD)/comp
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/comp/gui_widgets.o: ../../kernel/src/gui_widgets.c | $(BUILD)/comp
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/comp/gui_settings.o: ../../kernel/src/gui_settings.c | $(BUILD)/comp
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/comp/gui_files.o: ../../kernel/src/gui_files.c | $(BUILD)/comp
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/comp/gui_text.o: ../../kernel/src/gui_text.c | $(BUILD)/comp
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/comp/gui_render.o: ../../kernel/src/gui_render.c | $(BUILD)/comp
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/comp/console.o: ../../kernel/src/console.c | $(BUILD)/comp
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/$(OUT): $(OBJS)
	@test -n "$(LIBINPUT_LIBS)" || { echo "libinput development files are required" >&2; exit 1; }
	$(CC) $(OBJS) -o $@ $(LIBINPUT_LIBS) -lm -lpthread
	@echo "[compositor] Built: $@ (libinput)"

clean:
	rm -rf $(BUILD)/comp $(BUILD)/$(OUT)

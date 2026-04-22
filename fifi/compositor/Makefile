CC      := gcc
OUT     := fifi-compositor
BUILD   := ../../build-linux

# Shadow headers must come FIRST to override kernel headers
INCLUDES := \
    -I../platform/linux/include \
    -I../../kernel/include

CFLAGS := \
    -std=c11 -O3 -march=native -pipe -Wall -Wextra \
    -Wno-unused-parameter \
    -Wno-misleading-indentation \
    -D_GNU_SOURCE \
    $(INCLUDES)

SRCS := \
    main.c \
    ../platform/linux/platform.c \
    ../platform/linux/input.c \
    ../platform/linux/pty.c \
    ../platform/linux/vfs_posix.c \
    ../platform/linux/drm.c \
    ../platform/linux/audio.c \
    ../platform/linux/ipc.c \
    ../../kernel/src/gui.c \
    ../../kernel/src/console.c

OBJS := $(patsubst %.c,$(BUILD)/comp/%.o,$(notdir $(SRCS)))

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

$(BUILD)/comp/input.o: ../platform/linux/input.c | $(BUILD)/comp
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/comp/vfs_posix.o: ../platform/linux/vfs_posix.c | $(BUILD)/comp
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/comp/pty.o: ../platform/linux/pty.c | $(BUILD)/comp
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/comp/drm.o: ../platform/linux/drm.c | $(BUILD)/comp
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/comp/audio.o: ../platform/linux/audio.c | $(BUILD)/comp
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/comp/ipc.o: ../platform/linux/ipc.c | $(BUILD)/comp
	$(CC) $(CFLAGS) -c $< -o $@

# Kernel source files (compiled with Linux stubs)
$(BUILD)/comp/gui.o: ../../kernel/src/gui.c | $(BUILD)/comp
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/comp/console.o: ../../kernel/src/console.c | $(BUILD)/comp
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/$(OUT): $(OBJS)
	$(CC) -static $(OBJS) -o $@
	@echo "[compositor] Built: $@ (static)"

clean:
	rm -rf $(BUILD)/comp $(BUILD)/$(OUT)

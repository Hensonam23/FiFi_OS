CC  := gcc
OUT := fifi-gamepad

all: $(OUT)

$(OUT): gamepad.c ../../shared/ipc.h ../../shared/app_ipc.h
	$(CC) -O2 -static -o $@ $< -lm
	@echo "[gamepad] built: $@"

clean:
	rm -f $(OUT)

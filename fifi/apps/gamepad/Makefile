CC  := gcc
OUT := fifi-gamepad

all: $(OUT)

$(OUT): gamepad.c
	$(CC) -O2 -static -o $@ $< -lm
	@echo "[gamepad] built: $@"

clean:
	rm -f $(OUT)

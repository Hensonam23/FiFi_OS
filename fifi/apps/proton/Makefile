CC  := gcc
OUT := fifi-proton

all: $(OUT)

$(OUT): proton.c ../../shared/ipc.h ../../shared/app_ipc.h
	$(CC) -O2 -static -Wall -Wextra -Wno-unused-parameter -o $@ $< -s
	@echo "[proton] built: $@"

clean:
	rm -f $(OUT)

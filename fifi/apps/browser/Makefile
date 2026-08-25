CC  := gcc
OUT := fifi-browser

all: $(OUT)

$(OUT): browser.c ../../shared/ipc.h ../../shared/app_ipc.h ../../shared/app_ui.h
	$(CC) -O2 -static -Wall -Wextra -Wno-unused-parameter -o $@ $< -s
	@echo "[browser] built: $@"

clean:
	rm -f $(OUT)

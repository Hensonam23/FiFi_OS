CC  := gcc
OUT := fifi-installer

all: $(OUT)

$(OUT): installer.c ../../shared/ipc.h ../../shared/app_ipc.h ../../shared/app_ui.h
	$(CC) -O2 -static -Wall -Wextra -Wno-unused-parameter -o $@ $< -s
	@echo "[installer] built: $@"

clean:
	rm -f $(OUT)

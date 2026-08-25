CC  := gcc
OUT := fifi-imageviewer

all: $(OUT)

$(OUT): imageviewer.c ../../shared/ipc.h ../../shared/app_ipc.h ../../shared/app_ui.h
	$(CC) -O2 -static -Wall -Wextra -Wno-unused-parameter -o $@ $< -s
	@echo "[imageviewer] built: $@"

clean:
	rm -f $(OUT)

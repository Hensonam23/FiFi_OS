CC  := gcc
OUT := fifi-imageviewer

all: $(OUT)

$(OUT): imageviewer.c ../../shared/ipc.h ../../shared/app_ipc.h ../../shared/app_ui.h
	$(CC) -O2 -Wall -Wextra -Wno-unused-parameter -o $@ $< -lpng -ljpeg -lz -s
	@echo "[imageviewer] built: $@"

clean:
	rm -f $(OUT)

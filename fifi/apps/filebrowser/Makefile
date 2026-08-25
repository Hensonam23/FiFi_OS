CC  := gcc
OUT := fifi-filebrowser

all: $(OUT)

$(OUT): filebrowser.c ../../shared/ipc.h ../../shared/app_ipc.h ../../shared/app_ui.h
	$(CC) -O2 -static -o $@ $<
	@echo "[filebrowser] built: $@"

clean:
	rm -f $(OUT)

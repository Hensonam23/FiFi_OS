CC  := gcc
OUT := fifi-filebrowser

all: $(OUT)

$(OUT): filebrowser.c
	$(CC) -O2 -static -o $@ $<
	@echo "[filebrowser] built: $@"

clean:
	rm -f $(OUT)

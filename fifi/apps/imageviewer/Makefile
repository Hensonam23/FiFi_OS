CC  := gcc
OUT := fifi-imageviewer

all: $(OUT)

$(OUT): imageviewer.c
	$(CC) -O2 -static -Wall -Wextra -Wno-unused-parameter -o $@ $< -s
	@echo "[imageviewer] built: $@"

clean:
	rm -f $(OUT)

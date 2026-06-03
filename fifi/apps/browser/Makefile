CC  := gcc
OUT := fifi-browser

all: $(OUT)

$(OUT): browser.c
	$(CC) -O2 -static -Wall -Wextra -Wno-unused-parameter -o $@ $< -s
	@echo "[browser] built: $@"

clean:
	rm -f $(OUT)

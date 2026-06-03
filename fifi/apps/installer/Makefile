CC  := gcc
OUT := fifi-installer

all: $(OUT)

$(OUT): installer.c
	$(CC) -O2 -static -Wall -Wextra -Wno-unused-parameter -o $@ $< -s
	@echo "[installer] built: $@"

clean:
	rm -f $(OUT)

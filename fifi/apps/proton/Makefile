CC  := gcc
OUT := fifi-proton

all: $(OUT)

$(OUT): proton.c
	$(CC) -O2 -static -Wall -Wextra -Wno-unused-parameter -o $@ $< -s
	@echo "[proton] built: $@"

clean:
	rm -f $(OUT)

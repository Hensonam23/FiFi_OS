CC  := gcc
OUT := fifi-calc

all: $(OUT)

$(OUT): calc.c
	$(CC) -O2 -static -Wall -Wextra -Wno-unused-parameter -o $@ $< -lm -s
	@echo "[calc] built: $@"

clean:
	rm -f $(OUT)

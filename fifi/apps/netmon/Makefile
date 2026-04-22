CC  := gcc
OUT := fifi-netmon

all: $(OUT)

$(OUT): netmon.c
	$(CC) -O2 -static -Wall -o $@ $<
	@echo "[netmon] built: $@"

clean:
	rm -f $(OUT)

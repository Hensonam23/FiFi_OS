CC  := gcc
OUT := fifi-settings

all: $(OUT)

$(OUT): settings.c
	$(CC) -O2 -static -o $@ $<
	@echo "[settings] built: $@"

clean:
	rm -f $(OUT)

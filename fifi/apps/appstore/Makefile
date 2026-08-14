CC  := gcc
OUT := fifi-appstore

all: $(OUT)

LODEPNG := ../../platform/linux/vendor/lodepng.c

$(OUT): appstore.c $(LODEPNG) ../../shared/ipc.h ../../shared/app_ipc.h
	$(CC) -O2 -static -o $@ $< $(LODEPNG)
	@echo "[appstore] built: $@"

clean:
	rm -f $(OUT)

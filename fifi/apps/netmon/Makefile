CC  := gcc
OUT := fifi-netmon

all: $(OUT)

$(OUT): netmon.c ../../shared/ipc.h ../../shared/app_ipc.h
	$(CC) -O2 -static -Wall -o $@ $<
	@echo "[netmon] built: $@"

clean:
	rm -f $(OUT)

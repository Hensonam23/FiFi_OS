CC  := gcc
OUT := fifi-sysmon

all: $(OUT)

$(OUT): sysmon.c ../../shared/ipc.h ../../shared/app_ipc.h ../../shared/app_ui.h
	$(CC) -O2 -static -Wall -o $@ $<
	@echo "[sysmon] built: $@"

clean:
	rm -f $(OUT)

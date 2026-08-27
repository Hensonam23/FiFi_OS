CC  := gcc
OUT := fifi-settings

all: $(OUT)

$(OUT): settings.c ../../shared/ipc.h ../../shared/app_ipc.h ../../shared/app_ui.h ../../shared/theme.h
	$(CC) -O2 -static -I../../platform/linux/vendor -o $@ $< -lm
	@echo "[settings] built: $@"

clean:
	rm -f $(OUT)

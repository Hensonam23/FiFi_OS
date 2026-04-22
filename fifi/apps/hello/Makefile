CC  := gcc
OUT := hello-fifi-app

all: $(OUT)

$(OUT): hello.c
	$(CC) -O2 -static -o $@ $<
	@echo "[hello-app] built: $@"

clean:
	rm -f $(OUT)

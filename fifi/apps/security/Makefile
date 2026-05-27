CC = gcc
CFLAGS = -O2 -static -Wall -Wextra -Wno-unused-parameter -s

all: fifi-security

fifi-security: security.c
	$(CC) $(CFLAGS) -o fifi-security security.c

clean:
	rm -f fifi-security

CC      ?= cc
CFLAGS  ?= -O2 -g
CFLAGS  += -std=c11 -Wall -Wextra -Wpedantic -Wshadow

all: sip-cnf

sip-cnf: src/server.c src/sip_resp.c src/sip_resp.h
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ src/server.c src/sip_resp.c

test_resp: tests/test_resp.c src/sip_resp.c src/sip_resp.h
	$(CC) $(CFLAGS) -o $@ src/sip_resp.c tests/test_resp.c

test_lifecycle: tests/test_lifecycle.c
	$(CC) $(CFLAGS) -o $@ tests/test_lifecycle.c

test: sip-cnf test_resp test_lifecycle
	./test_resp
	./test_lifecycle

asan: CFLAGS += -fsanitize=address,undefined -fno-omit-frame-pointer
asan: clean test

# fully static binary for a scratch container image
static: LDFLAGS += -static
static: clean sip-cnf

clean:
	rm -f sip-cnf test_resp test_lifecycle

.PHONY: all test asan static clean

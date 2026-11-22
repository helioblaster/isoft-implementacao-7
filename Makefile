CC = cc
CFLAGS = -D_POSIX_C_SOURCE=200809L -std=c11 -Wall -Wextra -Wpedantic -Werror -O2
LDLIBS = -pthread

.PHONY: all clean

all: vm

vm: vm.c
	$(CC) $(CFLAGS) vm.c -o vm $(LDLIBS)

clean:
	rm -f vm correct.txt

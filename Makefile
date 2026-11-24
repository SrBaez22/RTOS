CC ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic -O2
LDLIBS ?= -pthread

.PHONY: all clean

all: lanchonete

lanchonete: lanchonete.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

clean:
	rm -f lanchonete

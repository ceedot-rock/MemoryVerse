CC = gcc
CFLAGS = -std=c11 -Wall -Wextra -O2 -g -D_POSIX_C_SOURCE=200809L
SRC = src/sha256.c src/journal.c src/region.c src/isa.c
OBJ = $(SRC:.c=.o)

all: libpmcp.a

libpmcp.a: $(OBJ)
	ar rcs $@ $^

%.o: %.c src/pmcp.h src/pmcp_internal.h
	$(CC) $(CFLAGS) -c $< -o $@

tests/test_pmcp: tests/test_pmcp.c libpmcp.a
	$(CC) $(CFLAGS) -o $@ $< -L. -lpmcp

test: tests/test_pmcp
	cd tests && ./test_pmcp

clean:
	rm -f $(OBJ) libpmcp.a tests/test_pmcp
	rm -f tests/*.pmcp tests/*.pmcp.journal
	rm -rf tests/crashwork

.PHONY: all test clean

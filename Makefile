CC=gcc
CFLAGS=-Werror -Wall -O1 -g3
OBJECT=bank_test

all: $(OBJECT)

*_test: *_test.c
	$(CC) $(CFLAGS) -o $@ $?

clean:
	rm -f $(OBJECT)

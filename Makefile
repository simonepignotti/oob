SHELL := /bin/bash

.PHONY: all clean test

all:
	@gcc -w -o c client.c
	@gcc -w -pthread -o s supervisor.c

test:
	@./test.sh

clean:
	@-rm -f *~ OOB-* s c supser* clio* form*


.PHONY: all clean

all: output.o

output.o: input.txt slow-copy

	./slow-copy input.txt -o output.o

slow-copy: slow_copy.c

	$(CC) -O2 slow_copy.c -o slow-copy

clean:

	rm -f output.o

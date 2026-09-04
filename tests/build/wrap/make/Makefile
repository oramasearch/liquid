CC ?= clang
CFLAGS ?= -O0 -Iinclude

objects = build/main.o build/left.o build/right.o

all: build/wrapped

build/wrapped: $(objects)
	$(CC) $(objects) -o $@

build/%.o: %.c include/value.h
	@mkdir -p build
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -rf build

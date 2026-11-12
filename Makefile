CC      ?= clang
CFLAGS  ?= -std=c99 -O2 -Wall -Wextra
LDFLAGS ?= -pthread -lX11 -ldl -lm

SRCS = src/arena.c src/lexer_parser.c src/ir_opt.c src/jit_x64.c src/runtime.c src/main.c
TARGET = vortex

.PHONY: all headless clean test

all: $(TARGET)

$(TARGET): $(SRCS) src/vortex.h
	$(CC) $(CFLAGS) $(SRCS) -o $(TARGET) $(LDFLAGS)

headless: $(SRCS) src/vortex.h
	$(CC) $(CFLAGS) -DVORTEX_NO_X11 $(SRCS) -o $(TARGET) -pthread -ldl -lm

test: $(TARGET)
	./tests/run_tests.sh

clean:
	rm -f $(TARGET) *.o

CC      ?= clang
CFLAGS  ?= -std=c99 -O2 -Wall -Wextra
UNAME_S := $(shell uname -s)

SRCS = src/arena.c src/lexer_parser.c src/ir_opt.c src/jit_x64.c src/jit_arm64.c src/debug_info.c src/pkg.c src/lsp.c src/runtime.c src/main.c
TARGET ?= vortex
VORTEX_VER := $(shell tr -d ' \r\n' < VERSION 2>/dev/null || echo 1.0.1)
VORTEX_VER_FLAG := -DVORTEX_VERSION=\"$(VORTEX_VER)\"

ifeq ($(UNAME_S),Darwin)
    X11_HEADER := $(firstword $(wildcard /opt/X11/include/X11/Xlib.h /usr/X11/include/X11/Xlib.h))
    ifeq ($(and $(filter 1,$(X11)),$(X11_HEADER)),)
        VORTEX_GUI_FLAGS ?= -DVORTEX_NO_X11 -DVORTEX_HEADLESS
        LDFLAGS ?= -pthread -lm
    else
        VORTEX_GUI_FLAGS ?= -DVORTEX_ENABLE_X11 -I$(dir $(dir $(X11_HEADER)))
        LDFLAGS ?= -pthread -L$(dir $(dir $(dir $(X11_HEADER))))/lib -lX11 -lm
    endif
    RELEASE_CFLAGS ?= -std=c99 -Os -Wall -Wextra -flto
    POST_RELEASE = strip -x $(TARGET)
else ifeq ($(UNAME_S),Linux)
    X11_HEADER := $(wildcard /usr/include/X11/Xlib.h)
    ifeq ($(or $(filter 1,$(HEADLESS)),$(if $(X11_HEADER),,1)),)
        VORTEX_GUI_FLAGS ?=
        LDFLAGS ?= -pthread -lX11 -ldl -lm
    else
        VORTEX_GUI_FLAGS ?= -DVORTEX_NO_X11 -DVORTEX_HEADLESS
        LDFLAGS ?= -pthread -ldl -lm
    endif
    RELEASE_CFLAGS ?= -std=c99 -Os -Wall -Wextra -ffunction-sections -fdata-sections -Wl,--gc-sections
    POST_RELEASE = strip --strip-all $(TARGET)
else
    VORTEX_GUI_FLAGS ?= -DVORTEX_NO_X11 -DVORTEX_HEADLESS
    LDFLAGS ?= -pthread -lm
    RELEASE_CFLAGS ?= -std=c99 -Os -Wall -Wextra
    POST_RELEASE = strip $(TARGET)
endif

.PHONY: all headless release clean test

all: $(TARGET)

$(TARGET): $(SRCS) src/vortex.h
	$(CC) $(CFLAGS) $(VORTEX_VER_FLAG) $(VORTEX_GUI_FLAGS) $(SRCS) -o $(TARGET) $(LDFLAGS)

headless: $(SRCS) src/vortex.h
	$(CC) $(CFLAGS) $(VORTEX_VER_FLAG) -DVORTEX_NO_X11 -DVORTEX_HEADLESS $(SRCS) -o $(TARGET) $(if $(filter Darwin,$(UNAME_S)),-pthread -lm,-pthread -ldl -lm)

release: $(SRCS) src/vortex.h
	$(CC) $(RELEASE_CFLAGS) $(VORTEX_VER_FLAG) $(VORTEX_GUI_FLAGS) $(SRCS) -o $(TARGET) $(LDFLAGS)
	$(POST_RELEASE)

test: $(TARGET)
	./tests/run_tests.sh

clean:
	rm -f $(TARGET) *.o

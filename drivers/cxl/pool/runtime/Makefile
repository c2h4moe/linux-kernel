CC ?= gcc
CFLAGS ?= -O2 -g -Wall -Wextra -std=gnu11 -pthread

TARGETS := cxl_macro_demo

all: $(TARGETS)

cxl_macro_demo: cxl_macro_demo.c cxl_macro_runtime.c cxl_macro_runtime.h \
	cxl_macro_protocol.h ../cxl_pool.h
	$(CC) $(CFLAGS) -o $@ cxl_macro_demo.c cxl_macro_runtime.c

clean:
	rm -f $(TARGETS)

.PHONY: all clean

CC ?= gcc
CFLAGS ?= -O2 -g -Wall -Wextra -std=gnu11 -pthread

TARGETS := cxl_macro_demo cxl_mutex_demo cxl_mutex_bench pthread_mutex_bench \
	cxl_nolock_bench dram_spin_bench redis_incr_bench redis_lock_bench

all: $(TARGETS)

cxl_macro_demo: cxl_macro_demo.c cxl_app.c cxl_app.h cxl_macro_runtime.c \
	cxl_macro_runtime.h cxl_macro_protocol.h ../cxl_pool.h
	$(CC) $(CFLAGS) -o $@ cxl_macro_demo.c cxl_app.c cxl_macro_runtime.c

cxl_mutex_demo: cxl_mutex_demo.c cxl_app.c cxl_app.h cxl_macro_runtime.c \
	cxl_macro_runtime.h cxl_macro_protocol.h ../cxl_pool.h
	$(CC) $(CFLAGS) -o $@ cxl_mutex_demo.c cxl_app.c cxl_macro_runtime.c

cxl_mutex_bench: cxl_mutex_bench.c cxl_app.c cxl_app.h cxl_macro_runtime.c \
	cxl_macro_runtime.h cxl_macro_protocol.h ../cxl_pool.h
	$(CC) $(CFLAGS) -o $@ cxl_mutex_bench.c cxl_app.c cxl_macro_runtime.c

pthread_mutex_bench: pthread_mutex_bench.c
	$(CC) $(CFLAGS) -o $@ pthread_mutex_bench.c

cxl_nolock_bench: cxl_nolock_bench.c cxl_app.c cxl_app.h cxl_macro_runtime.c \
	cxl_macro_runtime.h cxl_macro_protocol.h ../cxl_pool.h
	$(CC) $(CFLAGS) -o $@ cxl_nolock_bench.c cxl_app.c cxl_macro_runtime.c

dram_spin_bench: dram_spin_bench.c
	$(CC) $(CFLAGS) -o $@ dram_spin_bench.c

clean:
	rm -f $(TARGETS)

.PHONY: all clean

redis_incr_bench: redis_incr_bench.c
	$(CC) $(CFLAGS) -o $@ redis_incr_bench.c

redis_lock_bench: redis_lock_bench.c
	$(CC) $(CFLAGS) -o $@ redis_lock_bench.c

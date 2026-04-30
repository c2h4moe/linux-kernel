#define _GNU_SOURCE

#include "cxl_app.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define BENCH_DEFAULT_MACHINES 1
#define BENCH_DEFAULT_TOTAL_OPS 5000
#define BENCH_WINDOW_BASE 0x4000000000ULL
#define BENCH_WINDOW_SIZE (256ULL * 1024 * 1024)
#define BENCH_TIMEOUT_MS 5000

struct bench_shared {
	unsigned long long counter;
};

static int bench_total_ops = BENCH_DEFAULT_TOTAL_OPS;

static uint64_t bench_now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static int bench_main(void)
{
	struct bench_shared *shared;
	uint64_t start_ns;
	uint64_t end_ns;
	int i;

	if (bench_total_ops <= 0) {
		fprintf(stderr, "invalid total_ops=%d\n", bench_total_ops);
		return 1;
	}

	shared = cxl_malloc_current(sizeof(*shared));
	if (!shared) {
		perror("cxl_malloc");
		return 1;
	}

	memset(shared, 0, sizeof(*shared));
	start_ns = bench_now_ns();
	for (i = 0; i < bench_total_ops; i++)
		shared->counter++;
	end_ns = bench_now_ns();

	if (shared->counter != (unsigned long long)bench_total_ops) {
		fprintf(stderr, "wrong counter expected=%d got=%llu\n",
			bench_total_ops, shared->counter);
		return 1;
	}

	printf("mode=cxl_nolock threads=1 machines=1 total_ops=%d elapsed_ns=%" PRIu64
	       " avg_ns_per_inc=%.3f counter=%llu\n",
	       bench_total_ops, end_ns - start_ns,
	       (double)(end_ns - start_ns) / (double)bench_total_ops,
	       shared->counter);

	if (cxl_free_current(shared)) {
		fprintf(stderr, "cxl_free(shared) failed\n");
		return 1;
	}

	return 0;
}

int main(int argc, char **argv)
{
	struct cxl_app_cfg cfg;

	memset(&cfg, 0, sizeof(cfg));
	cfg.nr_machines = BENCH_DEFAULT_MACHINES;
	cfg.window_base = BENCH_WINDOW_BASE;
	cfg.window_size = BENCH_WINDOW_SIZE;
	cfg.timeout_ms = BENCH_TIMEOUT_MS;

	if (argc >= 2)
		bench_total_ops = atoi(argv[1]);
	if (bench_total_ops <= 0) {
		fprintf(stderr, "usage: %s [total_ops]\n", argv[0]);
		return 1;
	}

	if (cxl_run(&cfg, bench_main)) {
		printf("FAIL: cxl_nolock_bench failed\n");
		return 1;
	}

	return 0;
}

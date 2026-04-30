#define _GNU_SOURCE

#include "cxl_app.h"

#include <errno.h>
#include <inttypes.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define BENCH_DEFAULT_MACHINES 1
#define BENCH_DEFAULT_THREADS 1
#define BENCH_DEFAULT_TOTAL_OPS 5000
#define BENCH_WINDOW_BASE 0x4000000000ULL
#define BENCH_WINDOW_SIZE (256ULL * 1024 * 1024)
#define BENCH_TIMEOUT_MS 5000
#define BENCH_SYNC_TIMEOUT_MS 180000
#define BENCH_MAX_MACHINES 8

struct bench_shared {
	struct cxl_mutex lock;
	unsigned long long counter;
	atomic_int ready;
	atomic_int start;
	atomic_int done;
	atomic_int errors;
	int total_threads;
};

struct bench_arg {
	struct bench_shared *shared;
	int loops;
};

static int bench_threads = BENCH_DEFAULT_THREADS;
static int bench_total_ops = BENCH_DEFAULT_TOTAL_OPS;

static uint64_t bench_now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static int wait_atomic_at_least(const atomic_int *value, int target, int timeout_ms)
{
	uint64_t start_ns = bench_now_ns();
	uint64_t timeout_ns = (timeout_ms < 0) ? UINT64_MAX :
					       (uint64_t)timeout_ms * 1000000ULL;

	for (;;) {
		if (atomic_load(value) >= target)
			return 0;
		if (timeout_ns != UINT64_MAX &&
		    bench_now_ns() - start_ns > timeout_ns)
			return -ETIMEDOUT;
		usleep(1000);
	}
}

static int bench_worker(void *arg)
{
	struct bench_arg *wa = arg;
	int i;

	if (!wa || !wa->shared)
		return 1;

	atomic_fetch_add(&wa->shared->ready, 1);
	while (!atomic_load(&wa->shared->start))
		;

	for (i = 0; i < wa->loops; i++) {
		if (cxl_mutex_lock(&wa->shared->lock)) {
			atomic_fetch_add(&wa->shared->errors, 1);
			break;
		}
		wa->shared->counter++;
		if (cxl_mutex_unlock(&wa->shared->lock)) {
			atomic_fetch_add(&wa->shared->errors, 1);
			break;
		}
	}

	atomic_fetch_add(&wa->shared->done, 1);
	return 0;
}

CXL_WORKER(bench_worker);

static void wait_forever(void)
{
	for (;;)
		pause();
}

static int bench_main(void)
{
	int nr_machines = cxl_current_machine_count();
	int machine = cxl_current_machine();
	struct bench_shared *shared;
	struct bench_arg *args;
	uint64_t start_ns;
	uint64_t end_ns;
	int base_loops;
	int remainder;
	int i;
	int rc;

	if (machine != 0)
		wait_forever();

	if (bench_threads <= 0 || bench_total_ops <= 0) {
		fprintf(stderr, "machine0: invalid threads=%d total_ops=%d\n",
			bench_threads, bench_total_ops);
		return 1;
	}

	shared = cxl_malloc_current(sizeof(*shared));
	args = cxl_malloc_current(sizeof(*args) * (size_t)bench_threads);
	if (!shared || !args) {
		perror("machine0: cxl_malloc");
		return 1;
	}

	memset(shared, 0, sizeof(*shared));
	memset(args, 0, sizeof(*args) * (size_t)bench_threads);
	rc = cxl_mutex_init(&shared->lock);
	if (rc) {
		fprintf(stderr, "machine0: cxl_mutex_init failed: %d\n", rc);
		return 1;
	}

	shared->total_threads = bench_threads;
	base_loops = bench_total_ops / bench_threads;
	remainder = bench_total_ops % bench_threads;

	for (i = 0; i < bench_threads; i++) {
		args[i].shared = shared;
		args[i].loops = base_loops + (i == 0 ? remainder : 0);
		rc = cxl_spawn(bench_worker, i % nr_machines, &args[i]);
		if (rc) {
			fprintf(stderr, "machine0: cxl_spawn(%d) failed: %d\n", i, rc);
			return 1;
		}
	}

	rc = wait_atomic_at_least(&shared->ready, bench_threads,
				  BENCH_SYNC_TIMEOUT_MS);
	if (rc) {
		fprintf(stderr, "machine0: timeout waiting ready=%d/%d\n",
			atomic_load(&shared->ready), bench_threads);
		return 1;
	}

	start_ns = bench_now_ns();
	atomic_store(&shared->start, 1);

	rc = wait_atomic_at_least(&shared->done, bench_threads,
				  BENCH_SYNC_TIMEOUT_MS);
	end_ns = bench_now_ns();
	if (rc) {
		fprintf(stderr,
			"machine0: timeout done=%d/%d counter=%llu errors=%d\n",
			atomic_load(&shared->done), bench_threads, shared->counter,
			atomic_load(&shared->errors));
		return 1;
	}

	if (atomic_load(&shared->errors) != 0) {
		fprintf(stderr, "machine0: worker errors=%d\n",
			atomic_load(&shared->errors));
		return 1;
	}
	if (shared->counter != (unsigned long long)bench_total_ops) {
		fprintf(stderr, "machine0: wrong counter expected=%d got=%llu\n",
			bench_total_ops, shared->counter);
		return 1;
	}

	printf("mode=cxl threads=%d machines=%d total_ops=%d elapsed_ns=%" PRIu64
	       " avg_ns_per_inc=%.3f counter=%llu\n",
	       bench_threads, nr_machines, bench_total_ops, end_ns - start_ns,
	       (double)(end_ns - start_ns) / (double)bench_total_ops,
	       shared->counter);

	rc = cxl_mutex_destroy(&shared->lock);
	if (rc) {
		fprintf(stderr, "machine0: cxl_mutex_destroy failed: %d\n", rc);
		return 1;
	}
	if (cxl_free_current(args)) {
		fprintf(stderr, "machine0: cxl_free(args) failed\n");
		return 1;
	}
	if (cxl_free_current(shared)) {
		fprintf(stderr, "machine0: cxl_free(shared) failed\n");
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
		cfg.nr_machines = atoi(argv[1]);
	if (argc >= 3)
		bench_threads = atoi(argv[2]);
	if (argc >= 4)
		bench_total_ops = atoi(argv[3]);

	if (cfg.nr_machines <= 0 || cfg.nr_machines > BENCH_MAX_MACHINES ||
	    bench_threads <= 0 || bench_total_ops <= 0) {
		fprintf(stderr,
			"usage: %s [nr_machines>=1 and <=8] [threads] [total_ops]\n",
			argv[0]);
		return 1;
	}

	if (cxl_run(&cfg, bench_main)) {
		printf("FAIL: cxl_mutex_bench failed\n");
		return 1;
	}

	return 0;
}

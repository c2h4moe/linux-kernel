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

#define DEMO_DEFAULT_MACHINES 2
#define DEMO_WINDOW_BASE 0x4000000000ULL
#define DEMO_WINDOW_SIZE (256ULL * 1024 * 1024)
#define DEMO_TIMEOUT_MS 5000
#define DEMO_DONE_TIMEOUT_MS 60000
#define DEMO_WORKERS_PER_MACHINE 2
#define DEMO_ITERATIONS 2000

struct mutex_demo_state {
	struct cxl_mutex lock;
	atomic_uint_fast64_t counter;
	atomic_int done;
	atomic_int errors;
	int iterations;
	int total_workers;
};

static uint64_t demo_now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static int wait_done(atomic_int *done, int target, int timeout_ms)
{
	uint64_t start_ns = demo_now_ns();
	uint64_t timeout_ns = (timeout_ms < 0) ? UINT64_MAX :
					       (uint64_t)timeout_ms * 1000000ULL;

	for (;;) {
		if (atomic_load(done) >= target)
			return 0;
		if (timeout_ns != UINT64_MAX &&
		    demo_now_ns() - start_ns > timeout_ns)
			return -ETIMEDOUT;
		usleep(1000);
	}
}

static int counter_worker(void *arg)
{
	struct mutex_demo_state *shared = arg;
	int i;

	if (!shared)
		return 1;

	for (i = 0; i < shared->iterations; i++) {
		if (cxl_mutex_lock(&shared->lock)) {
			atomic_fetch_add(&shared->errors, 1);
			atomic_fetch_add(&shared->done, 1);
			return 1;
		}
		atomic_fetch_add(&shared->counter, 1);
		if (cxl_mutex_unlock(&shared->lock)) {
			atomic_fetch_add(&shared->errors, 1);
			atomic_fetch_add(&shared->done, 1);
			return 1;
		}
	}

	atomic_fetch_add(&shared->done, 1);
	return 0;
}

CXL_WORKER(counter_worker);

static void wait_forever(void)
{
	for (;;)
		pause();
}

static int run_basic_mutex_selftest(void)
{
	struct cxl_mutex *lock;
	int rc;

	lock = cxl_mutex_create();
	if (!lock) {
		perror("machine0: cxl_mutex_create");
		return 1;
	}

	rc = cxl_mutex_lock(lock);
	if (rc) {
		fprintf(stderr, "machine0: cxl_mutex_lock(create) failed: %d\n", rc);
		cxl_mutex_free(lock);
		return 1;
	}

	rc = cxl_mutex_trylock(lock);
	if (rc != -EBUSY) {
		fprintf(stderr,
			"machine0: expected trylock(create)=-EBUSY, got %d\n", rc);
		cxl_mutex_unlock(lock);
		cxl_mutex_free(lock);
		return 1;
	}

	rc = cxl_mutex_unlock(lock);
	if (rc) {
		fprintf(stderr,
			"machine0: cxl_mutex_unlock(create) failed: %d\n", rc);
		cxl_mutex_free(lock);
		return 1;
	}

	rc = cxl_mutex_trylock(lock);
	if (rc) {
		fprintf(stderr,
			"machine0: cxl_mutex_trylock(unlocked) failed: %d\n", rc);
		cxl_mutex_free(lock);
		return 1;
	}

	rc = cxl_mutex_unlock(lock);
	if (rc) {
		fprintf(stderr,
			"machine0: cxl_mutex_unlock(trylock) failed: %d\n", rc);
		cxl_mutex_free(lock);
		return 1;
	}

	rc = cxl_mutex_free(lock);
	if (rc) {
		fprintf(stderr, "machine0: cxl_mutex_free failed: %d\n", rc);
		return 1;
	}

	return 0;
}

static int demo_main(void)
{
	struct mutex_demo_state *shared;
	int machine = cxl_current_machine();
	int nr_machines = cxl_current_machine_count();
	int total_workers;
	int expected;
	int rc;
	int i;
	int target;

	if (machine != 0)
		wait_forever();

	if (run_basic_mutex_selftest())
		return 1;

	shared = cxl_malloc_current(sizeof(*shared));
	if (!shared) {
		perror("machine0: cxl_malloc");
		return 1;
	}

	memset(shared, 0, sizeof(*shared));
	rc = cxl_mutex_init(&shared->lock);
	if (rc) {
		fprintf(stderr, "machine0: cxl_mutex_init failed: %d\n", rc);
		cxl_free_current(shared);
		return 1;
	}

	total_workers = nr_machines * DEMO_WORKERS_PER_MACHINE;
	shared->iterations = DEMO_ITERATIONS;
	shared->total_workers = total_workers;
	atomic_store(&shared->counter, 0);
	atomic_store(&shared->done, 0);
	atomic_store(&shared->errors, 0);

	for (target = 0; target < nr_machines; target++) {
		for (i = 0; i < DEMO_WORKERS_PER_MACHINE; i++) {
			rc = cxl_spawn(counter_worker, target, shared);
			if (rc) {
				fprintf(stderr,
					"machine0: cxl_spawn(machine=%d) failed: %d\n",
					target, rc);
				cxl_mutex_destroy(&shared->lock);
				cxl_free_current(shared);
				return 1;
			}
		}
	}

	rc = wait_done(&shared->done, total_workers, DEMO_DONE_TIMEOUT_MS);
	if (rc) {
		fprintf(stderr,
			"machine0: timeout waiting workers rc=%d done=%d/%d counter=%" PRIuFAST64
			" errors=%d\n",
			rc, atomic_load(&shared->done), total_workers,
			atomic_load(&shared->counter), atomic_load(&shared->errors));
		cxl_mutex_destroy(&shared->lock);
		cxl_free_current(shared);
		return 1;
	}

	if (atomic_load(&shared->errors) != 0) {
		fprintf(stderr, "machine0: worker errors=%d\n",
			atomic_load(&shared->errors));
		cxl_mutex_destroy(&shared->lock);
		cxl_free_current(shared);
		return 1;
	}

	expected = total_workers * DEMO_ITERATIONS;
	if (atomic_load(&shared->counter) != (uint64_t)expected) {
		fprintf(stderr,
			"machine0: expected counter=%d got=%" PRIuFAST64 "\n",
			expected, atomic_load(&shared->counter));
		cxl_mutex_destroy(&shared->lock);
		cxl_free_current(shared);
		return 1;
	}

	rc = cxl_mutex_trylock(&shared->lock);
	if (rc) {
		fprintf(stderr,
			"machine0: cxl_mutex_trylock(final) failed: %d\n", rc);
		cxl_mutex_destroy(&shared->lock);
		cxl_free_current(shared);
		return 1;
	}

	rc = cxl_mutex_unlock(&shared->lock);
	if (rc) {
		fprintf(stderr,
			"machine0: cxl_mutex_unlock(final) failed: %d\n", rc);
		cxl_mutex_destroy(&shared->lock);
		cxl_free_current(shared);
		return 1;
	}

	printf("machine0: mutex counter=%" PRIuFAST64 " workers=%d iterations=%d\n",
	       atomic_load(&shared->counter), total_workers, DEMO_ITERATIONS);

	rc = cxl_mutex_destroy(&shared->lock);
	if (rc) {
		fprintf(stderr, "machine0: cxl_mutex_destroy failed: %d\n", rc);
		cxl_free_current(shared);
		return 1;
	}

	rc = cxl_free_current(shared);
	if (rc) {
		fprintf(stderr, "machine0: cxl_free failed: %d\n", rc);
		return 1;
	}

	return 0;
}

int main(int argc, char **argv)
{
	struct cxl_app_cfg cfg;

	memset(&cfg, 0, sizeof(cfg));
	cfg.nr_machines = DEMO_DEFAULT_MACHINES;
	cfg.window_base = DEMO_WINDOW_BASE;
	cfg.window_size = DEMO_WINDOW_SIZE;
	cfg.timeout_ms = DEMO_TIMEOUT_MS;

	if (argc >= 2) {
		cfg.nr_machines = atoi(argv[1]);
		if (cfg.nr_machines < 2 || cfg.nr_machines > 8) {
			fprintf(stderr, "usage: %s [nr_machines>=2 and <=8]\n",
				argv[0]);
			return 1;
		}
	}

	if (cxl_run(&cfg, demo_main)) {
		printf("FAIL: cxl_mutex_demo failed\n");
		return 1;
	}

	printf("PASS: cxl_mutex_demo succeeded\n");
	return 0;
}

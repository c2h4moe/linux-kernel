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
#define DEMO_ALLOC_TIMEOUT_MS 5000
#define DEMO_DONE_TIMEOUT_MS 10000

struct demo_shared_state {
	atomic_uint_fast64_t counter;
	atomic_int done;
	atomic_int status;
	atomic_int worker_machine;
	char payload[32];
};

static int demo_worker(void *arg)
{
	struct demo_shared_state *shared = arg;
	int machine = cxl_current_machine();
	int rc = 0;

	if (!shared)
		return 1;

	atomic_store(&shared->worker_machine, machine);
	atomic_fetch_add(&shared->counter, 1);
	snprintf(shared->payload, sizeof(shared->payload), "worker-on-%d",
		 machine);
	atomic_store(&shared->status, rc);
	atomic_store(&shared->done, 1);
	return rc;
}

CXL_WORKER(demo_worker);

static void wait_forever(void)
{
	for (;;)
		pause();
}

static uint64_t demo_now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static int wait_shared_done(struct demo_shared_state *shared, int timeout_ms)
{
	uint64_t start_ns = demo_now_ns();
	uint64_t timeout_ns = (timeout_ms < 0) ? UINT64_MAX :
					       (uint64_t)timeout_ms * 1000000ULL;

	for (;;) {
		if (atomic_load(&shared->done))
			return 0;
		if (timeout_ns != UINT64_MAX &&
		    demo_now_ns() - start_ns > timeout_ns)
			return -110;
		usleep(1000);
	}
}

static int demo_main(void)
{
	struct demo_shared_state *shared;
	int machine = cxl_current_machine();
	int rc;

	if (machine != 0)
		wait_forever();

	shared = cxl_malloc_current(sizeof(*shared));
	if (!shared) {
		perror("machine0: cxl_malloc");
		return 1;
	}

	atomic_store(&shared->counter, 41);
	atomic_store(&shared->done, 0);
	atomic_store(&shared->status, -1);
	atomic_store(&shared->worker_machine, -1);
	snprintf(shared->payload, sizeof(shared->payload), "hello-from-0");

	rc = cxl_spawn(demo_worker, 1, shared);
	if (rc) {
		fprintf(stderr, "machine0: cxl_spawn failed: %d\n", rc);
		return 1;
	}

	rc = wait_shared_done(shared, DEMO_DONE_TIMEOUT_MS);
	if (rc) {
		fprintf(stderr, "machine0: timeout waiting remote worker\n");
		return 1;
	}
	if (atomic_load(&shared->counter) != 42) {
		fprintf(stderr,
			"machine0: expected shared counter 42, got %" PRIuFAST64 "\n",
			atomic_load(&shared->counter));
		return 1;
	}
	if (atomic_load(&shared->status) != 0) {
		fprintf(stderr, "machine0: worker status=%d\n",
			atomic_load(&shared->status));
		return 1;
	}
	if (atomic_load(&shared->worker_machine) != 1) {
		fprintf(stderr, "machine0: worker ran on machine %d\n",
			atomic_load(&shared->worker_machine));
		return 1;
	}
	if (strcmp(shared->payload, "worker-on-1") != 0) {
		fprintf(stderr, "machine0: unexpected payload: %s\n",
			shared->payload);
		return 1;
	}

	printf("machine0: remote worker updated counter=%" PRIuFAST64
	       ", payload=%s\n",
	       atomic_load(&shared->counter), shared->payload);
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
	cfg.timeout_ms = DEMO_ALLOC_TIMEOUT_MS;

	if (argc >= 2) {
		cfg.nr_machines = atoi(argv[1]);
		if (cfg.nr_machines < 2 || cfg.nr_machines > 8) {
			fprintf(stderr, "usage: %s [nr_machines>=2 and <=8]\n",
				argv[0]);
			return 1;
		}
	}

	if (cxl_run(&cfg, demo_main)) {
		printf("FAIL: cxl_macro_demo failed\n");
		return 1;
	}

	printf("PASS: cxl_macro_demo succeeded\n");
	return 0;
}

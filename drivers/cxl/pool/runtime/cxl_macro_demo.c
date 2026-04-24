#define _GNU_SOURCE

#include "cxl_app.h"

#include <errno.h>
#include <inttypes.h>
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
#define DEMO_PUBLISH_TIMEOUT_MS 30000
#define DEMO_DONE_TIMEOUT_MS 10000

struct demo_shared_state {
	atomic_uint_fast64_t counter;
	char payload[32];
};

static uint64_t demo_now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static int wait_mailbox_nonzero(struct cxl_app *app, unsigned int slot,
				uint32_t *value_out, int timeout_ms)
{
	uint64_t start_ns = demo_now_ns();
	uint64_t timeout_ns = (timeout_ms < 0) ? UINT64_MAX :
					       (uint64_t)timeout_ms * 1000000ULL;

	for (;;) {
		uint32_t value = cxl_app_load_u32(app, slot);

		if (value != 0) {
			if (value_out)
				*value_out = value;
			return 0;
		}
		if (timeout_ns != UINT64_MAX &&
		    demo_now_ns() - start_ns > timeout_ns)
			return -110;
		usleep(1000);
	}
}

static int demo_main(struct cxl_app *app)
{
	struct demo_shared_state *shared;
	uint32_t published;
	int machine = cxl_machine_id(app);
	int rc;

	if (machine == 0) {
		shared = cxl_malloc(app, sizeof(*shared));
		if (!shared) {
			perror("machine0: cxl_malloc");
			return 1;
		}

		atomic_store(&shared->counter, 41);
		snprintf(shared->payload, sizeof(shared->payload), "hello-from-0");
		rc = cxl_app_store_ptr(app, 0, shared);
		if (rc) {
			fprintf(stderr, "machine0: pointer publish failed: %d\n", rc);
			return 1;
		}
	}

	rc = wait_mailbox_nonzero(app, 0, &published, DEMO_PUBLISH_TIMEOUT_MS);
	if (rc) {
		fprintf(stderr, "machine %d: timeout waiting shared pointer\n",
			machine);
		return 1;
	}
	shared = cxl_app_load_ptr(app, 0);
	if (!shared) {
		fprintf(stderr, "machine %d: shared pointer decode failed\n",
			machine);
		return 1;
	}

	if (machine == 1) {
		atomic_fetch_add(&shared->counter, 1);
		rc = cxl_free(app, shared);
		if (rc) {
			fprintf(stderr, "machine1: remote cxl_free failed: %d\n", rc);
			return 1;
		}
		rc = cxl_app_store_u32(app, 1, 1);
		if (rc) {
			fprintf(stderr, "machine1: mailbox store failed: %d\n", rc);
			return 1;
		}
		return 0;
	}

	rc = wait_mailbox_nonzero(app, 1, NULL, DEMO_DONE_TIMEOUT_MS);
	if (rc) {
		fprintf(stderr, "machine0: timeout waiting remote completion\n");
		return 1;
	}

	if (machine == 0) {
		struct demo_shared_state *again;

		if (atomic_load(&shared->counter) != 42) {
			fprintf(stderr,
				"machine0: expected shared counter 42, got %" PRIuFAST64 "\n",
				atomic_load(&shared->counter));
			return 1;
		}

		again = cxl_malloc(app, sizeof(*again));
		if (!again) {
			perror("machine0: cxl_malloc second");
			return 1;
		}
		if (again != shared) {
			fprintf(stderr,
				"machine0: expected allocator reuse after remote free\n");
			return 1;
		}
		atomic_store(&again->counter, 99);
		printf("machine0: allocator reused %p, counter=%" PRIuFAST64
		       ", payload=%s\n",
		       (void *)again, atomic_load(&again->counter), again->payload);
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

	if (cxl_app_run(&cfg, demo_main)) {
		printf("FAIL: cxl_macro_demo failed\n");
		return 1;
	}

	printf("PASS: cxl_macro_demo succeeded\n");
	return 0;
}

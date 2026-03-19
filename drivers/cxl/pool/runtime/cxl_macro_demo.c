#define _GNU_SOURCE

#include "cxl_macro_runtime.h"

#include "../cxl_pool.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define DEMO_ENTRY_INC 1
#define DEMO_DEFAULT_MACHINES 2
#define DEMO_MAX_MACHINES CXL_MACRO_MAX_MACHINES
#define DEMO_WINDOW_BASE 0x4000000000ULL
#define DEMO_WINDOW_SIZE (256ULL * 1024 * 1024)
#define CXL_DEV_PATH "/dev/cxl_pool"

struct demo_shared_state {
	atomic_uint_fast64_t counter;
};

struct demo_child_cfg {
	int machine_id;
	int nr_machines;
	uint64_t membership_mask;
	uint64_t control_alloc_id;
	uint64_t control_size;
	uint64_t window_base;
	uint64_t window_size;
};

static void *demo_remote_inc(void *arg)
{
	struct demo_shared_state *shared = arg;

	atomic_fetch_add(&shared->counter, 1);
	return NULL;
}

static int run_child(const struct demo_child_cfg *ccfg)
{
	struct cxl_macro_cfg cfg;
	struct cxl_macro_runtime *rt = NULL;
	struct demo_shared_state *shared = NULL;
	int rc;

	memset(&cfg, 0, sizeof(cfg));
	cfg.machine_id = ccfg->machine_id;
	cfg.nr_machines = ccfg->nr_machines;
	cfg.macro_id = 0;
	cfg.membership_mask = ccfg->membership_mask;
	cfg.control_alloc_id = ccfg->control_alloc_id;
	cfg.control_size = ccfg->control_size;
	cfg.window_base = ccfg->window_base;
	cfg.window_size = ccfg->window_size;

	rc = cxl_macro_init(&rt, &cfg);
	if (rc) {
		fprintf(stderr, "machine %d: cxl_macro_init failed: %d\n",
			ccfg->machine_id, rc);
		return 2;
	}

	rc = cxl_macro_register_entry(rt, DEMO_ENTRY_INC, demo_remote_inc,
				      "demo_remote_inc");
	if (rc) {
		fprintf(stderr, "machine %d: register entry failed: %d\n",
			ccfg->machine_id, rc);
		cxl_macro_destroy(rt);
		return 2;
	}

	cxl_macro_set_ready(rt);

	if (ccfg->machine_id == 0) {
		uint64_t remote_tid = 0;
		uint64_t start;

		rc = cxl_macro_wait_ready(rt, ccfg->membership_mask, 10000);
		if (rc) {
			fprintf(stderr, "machine0: wait_ready failed: %d\n", rc);
			cxl_macro_request_stop(rt);
			cxl_macro_destroy(rt);
			return 2;
		}

		shared = cxl_macro_malloc(rt, sizeof(*shared), 5000);
		if (!shared) {
			perror("machine0: cxl_macro_malloc");
			cxl_macro_request_stop(rt);
			cxl_macro_destroy(rt);
			return 2;
		}
		atomic_store(&shared->counter, 0);

		rc = cxl_spawn_remote(rt, 1, DEMO_ENTRY_INC,
				      (uint64_t)(uintptr_t)shared, &remote_tid,
				      5000);
		if (rc) {
			fprintf(stderr, "machine0: spawn_remote failed: %d\n", rc);
			cxl_macro_request_stop(rt);
			cxl_macro_destroy(rt);
			return 2;
		}

		start = (uint64_t)time(NULL);
		while (atomic_load(&shared->counter) != 1) {
			if ((uint64_t)time(NULL) - start > 5) {
				fprintf(stderr,
					"machine0: timeout waiting remote increment\n");
				cxl_macro_request_stop(rt);
				cxl_macro_destroy(rt);
				return 2;
			}
			usleep(1000);
		}

		printf("machine0: remote thread %" PRIu64
		       " succeeded, shared counter=%" PRIuFAST64 "\n",
		       remote_tid, atomic_load(&shared->counter));

		rc = cxl_macro_free(rt, shared, 5000);
		if (rc) {
			fprintf(stderr, "machine0: cxl_macro_free failed: %d\n",
				rc);
			cxl_macro_request_stop(rt);
			cxl_macro_destroy(rt);
			return 2;
		}

		cxl_macro_request_stop(rt);
		cxl_macro_destroy(rt);
		return 0;
	}

	rc = cxl_macro_wait_stop(rt, 15000);
	if (rc) {
		fprintf(stderr, "machine %d: wait_stop failed: %d\n",
			ccfg->machine_id, rc);
		cxl_macro_destroy(rt);
		return 2;
	}

	cxl_macro_destroy(rt);
	return 0;
}

int main(int argc, char **argv)
{
	int nr_machines = DEMO_DEFAULT_MACHINES;
	int fd = -1;
	uint64_t control_alloc_id = 0;
	uint64_t control_size = 0;
	struct cxl_control_plane *ctl = NULL;
	struct cxl_free_req free_req;
	pid_t pids[DEMO_MAX_MACHINES];
	int i;
	int exit_code = 0;
	uint64_t membership_mask = 0;

	if (argc >= 2) {
		nr_machines = atoi(argv[1]);
		if (nr_machines <= 1 || nr_machines > DEMO_MAX_MACHINES) {
			fprintf(stderr, "usage: %s [nr_machines>=2 and <=%d]\n",
				argv[0], DEMO_MAX_MACHINES);
			return 1;
		}
	}

	for (i = 0; i < nr_machines; i++)
		membership_mask |= (1ULL << i);

	fd = open(CXL_DEV_PATH, O_RDWR);
	if (fd < 0) {
		perror("open /dev/cxl_pool");
		return 1;
	}

	if (cxl_macro_control_create(fd, nr_machines, DEMO_WINDOW_BASE,
				     DEMO_WINDOW_SIZE, 3000000000ULL,
				     &control_alloc_id, &control_size, &ctl)) {
		perror("cxl_macro_control_create");
		close(fd);
		return 1;
	}

	printf("launcher: control_alloc_id=%" PRIu64 ", control_size=%" PRIu64
	       ", machines=%d\n",
	       control_alloc_id, control_size, nr_machines);

	for (i = 0; i < nr_machines; i++) {
		pid_t pid = fork();
		struct demo_child_cfg ccfg;

		if (pid < 0) {
			perror("fork");
			exit_code = 1;
			break;
		}
		if (pid == 0) {
			memset(&ccfg, 0, sizeof(ccfg));
			ccfg.machine_id = i;
			ccfg.nr_machines = nr_machines;
			ccfg.membership_mask = membership_mask;
			ccfg.control_alloc_id = control_alloc_id;
			ccfg.control_size = control_size;
			ccfg.window_base = DEMO_WINDOW_BASE;
			ccfg.window_size = DEMO_WINDOW_SIZE;
			_exit(run_child(&ccfg));
		}
		pids[i] = pid;
	}

	for (i = 0; i < nr_machines; i++) {
		int st;
		if (pids[i] <= 0)
			continue;
		if (waitpid(pids[i], &st, 0) < 0) {
			perror("waitpid");
			exit_code = 1;
			continue;
		}
		if (!WIFEXITED(st) || WEXITSTATUS(st) != 0)
			exit_code = 1;
	}

	munmap(ctl, control_size);
	memset(&free_req, 0, sizeof(free_req));
	free_req.alloc_id = control_alloc_id;
	if (ioctl(fd, CXL_FREE, &free_req) < 0) {
		perror("free control region");
		exit_code = 1;
	}
	close(fd);

	if (exit_code == 0) {
		printf("PASS: cxl_macro_demo succeeded\n");
		return 0;
	}

	printf("FAIL: cxl_macro_demo failed\n");
	return 1;
}

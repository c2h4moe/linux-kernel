#define _GNU_SOURCE

#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define BENCH_DEFAULT_THREADS 1
#define BENCH_DEFAULT_TOTAL_OPS 5000
#define BENCH_SYNC_TIMEOUT_US 1000

static long long counter;
static pthread_mutex_t counter_mutex = PTHREAD_MUTEX_INITIALIZER;
static atomic_int ready_threads;
static atomic_int start_flag;
static atomic_int done_threads;
static atomic_int error_flag;

struct thread_arg {
	int loops;
};

static uint64_t bench_now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static void *thread_func(void *arg)
{
	struct thread_arg *ta = arg;
	int i;

	if (!ta) {
		atomic_store(&error_flag, 1);
		return (void *)1;
	}

	atomic_fetch_add(&ready_threads, 1);
	while (!atomic_load(&start_flag))
		sched_yield();

	for (i = 0; i < ta->loops; i++) {
		if (pthread_mutex_lock(&counter_mutex) != 0) {
			atomic_store(&error_flag, 1);
			break;
		}
		counter++;
		if (pthread_mutex_unlock(&counter_mutex) != 0) {
			atomic_store(&error_flag, 1);
			break;
		}
	}

	atomic_fetch_add(&done_threads, 1);
	return NULL;
}

static int wait_atomic_at_least(const atomic_int *value, int target)
{
	for (;;) {
		if (atomic_load(value) >= target)
			return 0;
		usleep(BENCH_SYNC_TIMEOUT_US);
	}
}

int main(int argc, char **argv)
{
	int nr_threads = BENCH_DEFAULT_THREADS;
	int total_ops = BENCH_DEFAULT_TOTAL_OPS;
	pthread_t *threads;
	struct thread_arg *args;
	uint64_t start_ns;
	uint64_t end_ns;
	int base_loops;
	int remainder;
	int i;
	int rc;

	if (argc >= 2)
		nr_threads = atoi(argv[1]);
	if (argc >= 3)
		total_ops = atoi(argv[2]);
	if (nr_threads <= 0 || total_ops <= 0) {
		fprintf(stderr, "usage: %s [threads] [total_ops]\n", argv[0]);
		return 1;
	}

	threads = calloc((size_t)nr_threads, sizeof(*threads));
	args = calloc((size_t)nr_threads, sizeof(*args));
	if (!threads || !args) {
		perror("calloc");
		return 1;
	}

	counter = 0;
	atomic_store(&ready_threads, 0);
	atomic_store(&start_flag, 0);
	atomic_store(&done_threads, 0);
	atomic_store(&error_flag, 0);

	rc = pthread_mutex_init(&counter_mutex, NULL);
	if (rc != 0) {
		fprintf(stderr, "pthread_mutex_init failed: %d\n", rc);
		return 1;
	}

	base_loops = total_ops / nr_threads;
	remainder = total_ops % nr_threads;
	for (i = 0; i < nr_threads; i++) {
		args[i].loops = base_loops + (i == 0 ? remainder : 0);
		rc = pthread_create(&threads[i], NULL, thread_func, &args[i]);
		if (rc != 0) {
			fprintf(stderr, "pthread_create failed: %d\n", rc);
			return 1;
		}
	}

	wait_atomic_at_least(&ready_threads, nr_threads);
	start_ns = bench_now_ns();
	atomic_store(&start_flag, 1);
	wait_atomic_at_least(&done_threads, nr_threads);
	end_ns = bench_now_ns();

	for (i = 0; i < nr_threads; i++) {
		rc = pthread_join(threads[i], NULL);
		if (rc != 0) {
			fprintf(stderr, "pthread_join failed: %d\n", rc);
			return 1;
		}
	}

	if (atomic_load(&error_flag) != 0) {
		fprintf(stderr, "worker error detected\n");
		return 1;
	}
	if (counter != total_ops) {
		fprintf(stderr, "wrong counter expected=%d got=%lld\n", total_ops,
			counter);
		return 1;
	}

	printf("mode=pthread threads=%d machines=1 total_ops=%d elapsed_ns=%" PRIu64
	       " avg_ns_per_inc=%.3f counter=%lld\n",
	       nr_threads, total_ops, end_ns - start_ns,
	       (double)(end_ns - start_ns) / (double)total_ops, counter);

	pthread_mutex_destroy(&counter_mutex);
	free(args);
	free(threads);
	return 0;
}

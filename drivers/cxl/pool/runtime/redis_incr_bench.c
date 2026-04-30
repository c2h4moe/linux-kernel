#include <arpa/inet.h>
#include <errno.h>
#include <inttypes.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define BENCH_DEFAULT_TOTAL_OPS 5000
#define BENCH_DEFAULT_THREADS 1

struct worker_ctx {
	const char *host;
	int port;
	const char *key;
	int loops;
	pthread_barrier_t *start_barrier;
	int rc;
};

static uint64_t now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static int write_all(int fd, const char *buf, size_t len)
{
	size_t off = 0;
	while (off < len) {
		ssize_t n = send(fd, buf + off, len - off, 0);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -errno;
		}
		off += (size_t)n;
	}
	return 0;
}

static int read_exact(int fd, char *buf, size_t len)
{
	size_t off = 0;
	while (off < len) {
		ssize_t n = recv(fd, buf + off, len - off, 0);
		if (n == 0)
			return -ECONNRESET;
		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -errno;
		}
		off += (size_t)n;
	}
	return 0;
}

static int read_line(int fd, char *buf, size_t cap)
{
	size_t off = 0;
	char c = '\0';
	while (off + 1 < cap) {
		int rc = read_exact(fd, &c, 1);
		if (rc)
			return rc;
		buf[off++] = c;
		if (c == '\n')
			break;
	}
	buf[off] = '\0';
	if (off < 2 || buf[off - 2] != '\r' || buf[off - 1] != '\n')
		return -EPROTO;
	return 0;
}

static int read_resp_integer(int fd, long long *out)
{
	char line[128];
	int rc = read_line(fd, line, sizeof(line));
	if (rc)
		return rc;
	if (line[0] == '-')
		return -EIO;
	if (line[0] != ':')
		return -EPROTO;
	* out = strtoll(line + 1, NULL, 10);
	return 0;
}

static int read_resp_simple(int fd)
{
	char line[256];
	int rc = read_line(fd, line, sizeof(line));
	if (rc)
		return rc;
	if (line[0] == '+')
		return 0;
	if (line[0] == '-')
		return -EIO;
	return -EPROTO;
}

static int redis_connect(const char *host, int port)
{
	int fd;
	struct sockaddr_in addr;

	fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0)
		return -errno;

	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons((uint16_t)port);
	if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
		close(fd);
		return -EINVAL;
	}
	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		int e = errno;
		close(fd);
		return -e;
	}
	return fd;
}

static int redis_send_set_zero(int fd, const char *key)
{
	char req[1024];
	int n = snprintf(req, sizeof(req),
		"*3\r\n$3\r\nSET\r\n$%zu\r\n%s\r\n$1\r\n0\r\n",
		strlen(key), key);
	if (n <= 0 || (size_t)n >= sizeof(req))
		return -EINVAL;
	if (write_all(fd, req, (size_t)n))
		return -EIO;
	return read_resp_simple(fd);
}

static int redis_send_get_int(int fd, const char *key, long long *out)
{
	char req[1024];
	char line[128];
	long len;
	int rc;
	int n = snprintf(req, sizeof(req),
		"*2\r\n$3\r\nGET\r\n$%zu\r\n%s\r\n",
		strlen(key), key);
	if (n <= 0 || (size_t)n >= sizeof(req))
		return -EINVAL;
	if (write_all(fd, req, (size_t)n))
		return -EIO;

	rc = read_line(fd, line, sizeof(line));
	if (rc)
		return rc;
	if (line[0] == '$') {
		len = strtol(line + 1, NULL, 10);
		if (len < 0 || len > 64)
			return -EPROTO;
		rc = read_exact(fd, line, (size_t)len + 2);
		if (rc)
			return rc;
		line[len] = '\0';
		*out = strtoll(line, NULL, 10);
		return 0;
	}
	if (line[0] == '-')
		return -EIO;
	return -EPROTO;
}

static int redis_send_incr(int fd, const char *key)
{
	char req[1024];
	long long tmp;
	int n = snprintf(req, sizeof(req),
		"*2\r\n$4\r\nINCR\r\n$%zu\r\n%s\r\n",
		strlen(key), key);
	if (n <= 0 || (size_t)n >= sizeof(req))
		return -EINVAL;
	if (write_all(fd, req, (size_t)n))
		return -EIO;
	return read_resp_integer(fd, &tmp);
}

static void *worker_main(void *arg)
{
	struct worker_ctx *ctx = (struct worker_ctx *)arg;
	int fd;
	int i;
	int rc;

	fd = redis_connect(ctx->host, ctx->port);
	if (fd < 0) {
		ctx->rc = fd;
		return NULL;
	}

	pthread_barrier_wait(ctx->start_barrier);

	for (i = 0; i < ctx->loops; i++) {
		rc = redis_send_incr(fd, ctx->key);
		if (rc) {
			ctx->rc = rc;
			close(fd);
			return NULL;
		}
	}

	close(fd);
	ctx->rc = 0;
	return NULL;
}

int main(int argc, char **argv)
{
	int nr_threads = BENCH_DEFAULT_THREADS;
	int total_ops = BENCH_DEFAULT_TOTAL_OPS;
	const char *host = "127.0.0.1";
	int port = 6379;
	const char *key = "cxl:bench:counter";
	pthread_t *threads;
	struct worker_ctx *workers;
	pthread_barrier_t start_barrier;
	int base_loops;
	int remainder;
	int i;
	uint64_t start_ns;
	uint64_t end_ns;
	long long counter = -1;
	int rc;
	int fd;

	if (argc > 1)
		nr_threads = atoi(argv[1]);
	if (argc > 2)
		total_ops = atoi(argv[2]);
	if (argc > 3)
		host = argv[3];
	if (argc > 4)
		port = atoi(argv[4]);
	if (argc > 5)
		key = argv[5];

	if (nr_threads <= 0 || total_ops <= 0 || port <= 0) {
		fprintf(stderr,
			"usage: %s [threads] [total_ops] [host] [port] [key]\n",
			argv[0]);
		return 1;
	}

	fd = redis_connect(host, port);
	if (fd < 0) {
		fprintf(stderr, "redis connect failed host=%s port=%d rc=%d\n",
			host, port, fd);
		return 1;
	}
	rc = redis_send_set_zero(fd, key);
	if (rc) {
		fprintf(stderr, "redis SET failed rc=%d\n", rc);
		close(fd);
		return 1;
	}
	close(fd);

	threads = calloc((size_t)nr_threads, sizeof(*threads));
	workers = calloc((size_t)nr_threads, sizeof(*workers));
	if (!threads || !workers) {
		fprintf(stderr, "calloc failed\n");
		free(threads);
		free(workers);
		return 1;
	}

	pthread_barrier_init(&start_barrier, NULL, (unsigned)nr_threads + 1);
	base_loops = total_ops / nr_threads;
	remainder = total_ops % nr_threads;

	for (i = 0; i < nr_threads; i++) {
		workers[i].host = host;
		workers[i].port = port;
		workers[i].key = key;
		workers[i].loops = base_loops + (i < remainder ? 1 : 0);
		workers[i].start_barrier = &start_barrier;
		workers[i].rc = 0;
		rc = pthread_create(&threads[i], NULL, worker_main, &workers[i]);
		if (rc) {
			fprintf(stderr, "pthread_create failed: %d\n", rc);
			return 1;
		}
	}

	pthread_barrier_wait(&start_barrier);
	start_ns = now_ns();

	for (i = 0; i < nr_threads; i++) {
		pthread_join(threads[i], NULL);
		if (workers[i].rc) {
			fprintf(stderr, "worker %d failed rc=%d\n", i, workers[i].rc);
			free(threads);
			free(workers);
			return 1;
		}
	}
	end_ns = now_ns();

	fd = redis_connect(host, port);
	if (fd < 0) {
		fprintf(stderr, "redis connect for GET failed rc=%d\n", fd);
		free(threads);
		free(workers);
		return 1;
	}
	rc = redis_send_get_int(fd, key, &counter);
	close(fd);
	if (rc) {
		fprintf(stderr, "redis GET failed rc=%d\n", rc);
		free(threads);
		free(workers);
		return 1;
	}
	if (counter != total_ops) {
		fprintf(stderr, "wrong counter expected=%d got=%lld\n", total_ops,
			counter);
		free(threads);
		free(workers);
		return 1;
	}

	printf("mode=redis threads=%d machines=%d total_ops=%d elapsed_ns=%" PRIu64
	       " avg_ns_per_inc=%.3f counter=%lld\n",
	       nr_threads, nr_threads, total_ops, end_ns - start_ns,
	       (double)(end_ns - start_ns) / (double)total_ops, counter);

	free(threads);
	free(workers);
	return 0;
}

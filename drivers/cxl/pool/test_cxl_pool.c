/*
 * CXL Pool Allocator Correctness Test
 *
 * Verifies:
 * 1. Multi-allocation returns unique alloc_id values
 * 2. free_pages accounting decreases/increases as expected
 * 3. mmap works for every allocation and data is preserved
 * 4. final free_pages matches initial free_pages after cleanup
 */

#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <inttypes.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <string.h>
#include <errno.h>
#include <stdbool.h>

#include "cxl_pool.h"

#define CXL_DEV "/dev/cxl_pool"
#define MAX_ALLOCS 32
#define REQ_SIZE_BYTES (1 * 1024 * 1024) /* rounds to 1 x 4MB page */

struct alloc_rec {
	uint64_t alloc_id;
	uint64_t mapped_size;
	void *addr;
	uint64_t signature;
};

static int get_pool_info(int fd, struct cxl_pool_info *info)
{
	if (ioctl(fd, CXL_GET_INFO, info) < 0)
		return -1;
	return 0;
}

static bool has_duplicate_ids(struct alloc_rec *recs, int n)
{
	int i, j;

	for (i = 0; i < n; i++) {
		for (j = i + 1; j < n; j++) {
			if (recs[i].alloc_id == recs[j].alloc_id)
				return true;
		}
	}
	return false;
}

int main(int argc, char *argv[])
{
	(void)argc;
	(void)argv;

	int fd;
	int ret = 1;
	int i;
	int num_allocs;
	uint64_t allocated_pages = 0;
	struct cxl_pool_info info_before, info_after_alloc, info_after_free;
	struct alloc_rec recs[MAX_ALLOCS];

	memset(recs, 0, sizeof(recs));

	printf("=== CXL Pool Allocator Correctness Test ===\n\n");

	fd = open(CXL_DEV, O_RDWR);
	if (fd < 0) {
		perror("Failed to open " CXL_DEV);
		return 1;
	}

	printf("1) Reading initial pool state...\n");
	if (get_pool_info(fd, &info_before) < 0) {
		perror("CXL_GET_INFO ioctl failed");
		goto out;
	}

	printf("   total_pages=%" PRIu64 ", free_pages=%" PRIu64
	       ", huge_page_size=%" PRIu64 "\n",
	       info_before.total_pages, info_before.free_pages,
	       info_before.huge_page_size);

	if (info_before.huge_page_size == 0) {
		fprintf(stderr, "Invalid huge_page_size=0\n");
		goto out;
	}

	/*
	 * Keep headroom: do not consume all pages so this test is less invasive
	 * when run repeatedly.
	 */
	num_allocs = (int)(info_before.free_pages / 2);
	if (num_allocs > MAX_ALLOCS)
		num_allocs = MAX_ALLOCS;
	if (num_allocs < 2)
		num_allocs = (int)info_before.free_pages;
	if (num_allocs <= 0) {
		fprintf(stderr, "No free CXL pages available for test\n");
		goto out;
	}

	printf("2) Allocating %d chunks (request=%d bytes each)...\n",
	       num_allocs, REQ_SIZE_BYTES);
	for (i = 0; i < num_allocs; i++) {
		struct cxl_alloc_req req;
		uint64_t pages_this_alloc;
		off_t offset;

		memset(&req, 0, sizeof(req));
		req.size = REQ_SIZE_BYTES;
		if (ioctl(fd, CXL_ALLOC, &req) < 0) {
			perror("CXL_ALLOC failed");
			fprintf(stderr, "Allocation failed at i=%d\n", i);
			goto cleanup;
		}

		if (req.alloc_id == 0 || req.mapped_size == 0) {
			fprintf(stderr, "Invalid alloc result: id=%" PRIu64
				" size=%" PRIu64 "\n",
				req.alloc_id, req.mapped_size);
			goto cleanup;
		}

		if (req.mapped_size % info_before.huge_page_size != 0) {
			fprintf(stderr, "Mapped size is not 4MB aligned: %" PRIu64
				"\n", req.mapped_size);
			goto cleanup;
		}

		recs[i].alloc_id = req.alloc_id;
		recs[i].mapped_size = req.mapped_size;
		recs[i].signature = 0xC0A00000ULL + (uint64_t)i;

		offset = (off_t)req.alloc_id * getpagesize();
		recs[i].addr = mmap(NULL, req.mapped_size, PROT_READ | PROT_WRITE,
				    MAP_SHARED, fd, offset);
		if (recs[i].addr == MAP_FAILED) {
			recs[i].addr = NULL;
			perror("mmap failed");
			goto cleanup;
		}

		((uint64_t *)recs[i].addr)[0] = recs[i].signature;
		((uint64_t *)recs[i].addr)[(req.mapped_size / sizeof(uint64_t)) - 1] =
			recs[i].signature ^ 0xAAAAAAAAAAAAAAAAULL;

		pages_this_alloc = req.mapped_size / info_before.huge_page_size;
		allocated_pages += pages_this_alloc;
	}

	if (has_duplicate_ids(recs, num_allocs)) {
		fprintf(stderr, "Duplicate alloc_id detected\n");
		goto cleanup;
	}

	printf("3) Validating free_pages accounting after allocation...\n");
	if (get_pool_info(fd, &info_after_alloc) < 0) {
		perror("CXL_GET_INFO after alloc failed");
		goto cleanup;
	}
	if (info_after_alloc.free_pages + allocated_pages != info_before.free_pages) {
		fprintf(stderr,
			"free_pages mismatch after alloc: before=%" PRIu64
			" after=%" PRIu64 " allocated=%" PRIu64 "\n",
			info_before.free_pages, info_after_alloc.free_pages,
			allocated_pages);
		goto cleanup;
	}

	printf("4) Validating mmap data for each allocation...\n");
	for (i = 0; i < num_allocs; i++) {
		uint64_t first = ((uint64_t *)recs[i].addr)[0];
		uint64_t last = ((uint64_t *)recs[i].addr)
			[(recs[i].mapped_size / sizeof(uint64_t)) - 1];

		if (first != recs[i].signature ||
		    last != (recs[i].signature ^ 0xAAAAAAAAAAAAAAAAULL)) {
			fprintf(stderr, "Data mismatch for alloc_id=%" PRIu64 "\n",
				recs[i].alloc_id);
			goto cleanup;
		}
	}

	printf("5) Freeing all allocations...\n");
	for (i = num_allocs - 1; i >= 0; i--) {
		struct cxl_free_req free_req;

		if (recs[i].addr) {
			munmap(recs[i].addr, recs[i].mapped_size);
			recs[i].addr = NULL;
		}

		memset(&free_req, 0, sizeof(free_req));
		free_req.alloc_id = recs[i].alloc_id;
		if (ioctl(fd, CXL_FREE, &free_req) < 0) {
			perror("CXL_FREE failed");
			goto cleanup;
		}
	}

	printf("6) Validating free_pages accounting after free...\n");
	if (get_pool_info(fd, &info_after_free) < 0) {
		perror("CXL_GET_INFO after free failed");
		goto out;
	}
	if (info_after_free.free_pages != info_before.free_pages) {
		fprintf(stderr,
			"free_pages mismatch after free: before=%" PRIu64
			" after=%" PRIu64 "\n",
			info_before.free_pages, info_after_free.free_pages);
		goto out;
	}

	printf("\nPASS: allocator correctness checks succeeded\n");
	ret = 0;
	goto out;

cleanup:
	for (i = 0; i < num_allocs; i++) {
		struct cxl_free_req free_req;

		if (recs[i].addr) {
			munmap(recs[i].addr, recs[i].mapped_size);
			recs[i].addr = NULL;
		}
		if (!recs[i].alloc_id)
			continue;
		memset(&free_req, 0, sizeof(free_req));
		free_req.alloc_id = recs[i].alloc_id;
		ioctl(fd, CXL_FREE, &free_req);
		recs[i].alloc_id = 0;
	}

out:
	close(fd);
	if (ret != 0)
		printf("\nFAIL: allocator correctness test failed\n");
	return ret;
}

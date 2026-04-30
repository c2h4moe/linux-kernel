#define _GNU_SOURCE

#include "cxl_app.h"

#include "cxl_macro_runtime.h"

#include "../cxl_pool.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/futex.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define CXL_DEV_PATH "/dev/cxl_pool"

#define CXL_APP_MAX_MAILBOX 4
#define CXL_APP_MAX_OWNED_EXTENTS 128

#define CXL_APP_EXTENT_SIZE (4ULL * 1024 * 1024)
#define CXL_APP_RUN_SIZE (64ULL * 1024)
#define CXL_APP_RUNS_PER_EXTENT (CXL_APP_EXTENT_SIZE / CXL_APP_RUN_SIZE)
#define CXL_APP_FIRST_USABLE_RUN 1
#define CXL_APP_ALLOC_ALIGN 16

#define CXL_APP_EXTENT_MAGIC 0x43584C45U /* CXLE */
#define CXL_APP_RUN_MAGIC 0x43584C52U    /* CXLR */

enum cxl_app_extent_kind {
	CXL_APP_EXTENT_SMALL = 1,
	CXL_APP_EXTENT_LARGE_HEAD = 2,
};

enum cxl_app_large_state {
	CXL_APP_LARGE_LIVE = 1,
	CXL_APP_LARGE_FREE_PENDING = 2,
	CXL_APP_LARGE_FREE = 3,
};

struct cxl_extent_hdr {
	uint32_t magic;
	uint16_t kind;
	uint16_t owner_machine;
	uint32_t state;
	uint32_t run_size;
	uint32_t nr_runs;
	uint64_t mapped_size;
	uint64_t user_size;
};

struct cxl_run_hdr {
	uint32_t magic;
	uint16_t size_class;
	uint16_t flags;
	uint32_t slot_count;
	uint32_t bitmap_words;
	uint32_t payload_offset;
	uint32_t reserved;
};

struct cxl_small_extent {
	void *base;
};

struct cxl_large_alloc {
	struct cxl_extent_hdr *hdr;
	void *user_ptr;
};

struct cxl_app {
	int machine_id;
	int nr_machines;
	int timeout_ms;
	uint64_t window_base;
	uint64_t window_size;
	struct cxl_macro_runtime *rt;
	struct cxl_control_plane *ctl;

	pthread_mutex_t alloc_lock;
	struct cxl_small_extent small_extents[CXL_APP_MAX_OWNED_EXTENTS];
	size_t nr_small_extents;
	struct cxl_large_alloc large_allocs[CXL_APP_MAX_OWNED_EXTENTS];
	size_t nr_large_allocs;
};

struct cxl_worker_desc {
	const char *name;
	cxl_worker_fn_t worker;
	void *(*entry_fn)(void *arg);
	uint32_t id;
	struct cxl_worker_desc *next;
};

static const uint16_t cxl_size_classes[] = {
	16, 32, 64, 128, 256, 512, 1024, 2048, 4096, 8192, 16384,
};

static pthread_mutex_t cxl_worker_lock = PTHREAD_MUTEX_INITIALIZER;
static struct cxl_worker_desc *cxl_workers;
static int cxl_worker_registry_error;
/* One simulated machine-process owns exactly one app context. */
static struct cxl_app *cxl_process_app;
/* User-facing helpers resolve the current app through thread-local state. */
static __thread struct cxl_app *cxl_current_app_tls;
static cxl_main_fn_t cxl_run_main_fn;

static uint64_t cxl_now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static uint32_t cxl_worker_hash(const char *name)
{
	const unsigned char *p = (const unsigned char *)name;
	uint32_t hash = 2166136261u;

	while (*p) {
		hash ^= *p++;
		hash *= 16777619u;
	}
	if (hash == 0)
		hash = 1;
	return hash;
}

static void cxl_set_current_app(struct cxl_app *app)
{
	cxl_current_app_tls = app;
}

static struct cxl_app *cxl_get_current_app(void)
{
	if (cxl_current_app_tls)
		return cxl_current_app_tls;
	return cxl_process_app;
}

static struct cxl_worker_desc *cxl_find_worker_by_fn(cxl_worker_fn_t worker)
{
	struct cxl_worker_desc *it;

	for (it = cxl_workers; it; it = it->next) {
		if (it->worker == worker)
			return it;
	}
	return NULL;
}

static int cxl_install_workers(struct cxl_app *app)
{
	struct cxl_worker_desc *it;

	/* Every machine installs the same worker ID -> wrapper table locally. */
	pthread_mutex_lock(&cxl_worker_lock);
	for (it = cxl_workers; it; it = it->next) {
		int rc = cxl_macro_register_entry(app->rt, it->id, it->entry_fn,
						 it->name);

		if (rc && rc != -EEXIST) {
			pthread_mutex_unlock(&cxl_worker_lock);
			return rc;
		}
	}
	pthread_mutex_unlock(&cxl_worker_lock);
	return 0;
}

static int cxl_ptr_is_shared(struct cxl_app *app, const void *ptr)
{
	uintptr_t ptr_u;
	uint64_t base;
	uint64_t size;

	if (!ptr)
		return 1;

	ptr_u = (uintptr_t)ptr;
	base = app->window_base;
	size = app->window_size;
	return ptr_u >= base && ptr_u < base + size;
}

static int cxl_mutex_is_shared(struct cxl_mutex *lock, struct cxl_app **app_out)
{
	struct cxl_app *app = cxl_get_current_app();

	if (!app)
		return -EINVAL;
	if (!lock)
		return -EINVAL;
	if (!cxl_ptr_is_shared(app, lock))
		return -ERANGE;
	if (app_out)
		*app_out = app;
	return 0;
}

static int cxl_mutex_validate_live(struct cxl_mutex *lock)
{
	int rc = cxl_mutex_is_shared(lock, NULL);

	if (rc)
		return rc;
	if (atomic_load_explicit(&lock->magic, memory_order_acquire) !=
	    CXL_MUTEX_MAGIC)
		return -EINVAL;
	return 0;
}

static inline int cxl_mutex_fast_validate(struct cxl_mutex *lock)
{
	return lock ? 0 : -EINVAL;
}

/*
 * Lock callers only see cxl_mutex_lock/unlock. Pause/backoff stay here as an
 * internal policy choice so benchmarks and user code do not duplicate it.
 */
static void cxl_mutex_spin_pause(unsigned int *spins)
{
	(*spins)++;
#if defined(__x86_64__) || defined(__i386__)
	__asm__ __volatile__("pause");
#endif
	if ((*spins & 0x3fffU) == 0)
		sched_yield();
}

static void cxl_mutex_backoff(unsigned int *spins, unsigned int *delay)
{
	unsigned int i;

	for (i = 0; i < *delay; i++)
		cxl_mutex_spin_pause(spins);
	if (*delay < 1024U)
		*delay <<= 1;
}

int cxl_worker_register_internal(const char *name, cxl_worker_fn_t worker,
				 void *(*entry_fn)(void *arg))
{
	struct cxl_worker_desc *it;
	struct cxl_worker_desc *desc;
	uint32_t id;

	if (!name || !worker || !entry_fn)
		return -EINVAL;

	id = cxl_worker_hash(name);
	pthread_mutex_lock(&cxl_worker_lock);
	for (it = cxl_workers; it; it = it->next) {
		if (it->worker == worker) {
			if (strcmp(it->name, name) == 0 && it->entry_fn == entry_fn) {
				pthread_mutex_unlock(&cxl_worker_lock);
				return 0;
			}
			if (!cxl_worker_registry_error)
				cxl_worker_registry_error = -EEXIST;
			pthread_mutex_unlock(&cxl_worker_lock);
			return -EEXIST;
		}
		if (it->id == id && strcmp(it->name, name) != 0) {
			if (!cxl_worker_registry_error)
				cxl_worker_registry_error = -EEXIST;
			pthread_mutex_unlock(&cxl_worker_lock);
			return -EEXIST;
		}
	}

	desc = calloc(1, sizeof(*desc));
	if (!desc) {
		if (!cxl_worker_registry_error)
			cxl_worker_registry_error = -ENOMEM;
		pthread_mutex_unlock(&cxl_worker_lock);
		return -ENOMEM;
	}

	desc->name = name;
	desc->worker = worker;
	desc->entry_fn = entry_fn;
	desc->id = id;
	desc->next = cxl_workers;
	cxl_workers = desc;
	pthread_mutex_unlock(&cxl_worker_lock);
	return 0;
}

void *cxl_worker_invoke_internal(cxl_worker_fn_t worker, void *arg)
{
	struct cxl_app *app = cxl_process_app;
	int rc;

	cxl_set_current_app(app);
	rc = worker(arg);
	return (void *)(intptr_t)rc;
}

static int cxl_futex_wait(_Atomic uint32_t *uaddr, uint32_t val, int ms)
{
	struct timespec ts;
	struct timespec *tsp = NULL;

	if (ms >= 0) {
		ts.tv_sec = ms / 1000;
		ts.tv_nsec = (ms % 1000) * 1000000L;
		tsp = &ts;
	}

	return syscall(SYS_futex, uaddr, FUTEX_WAIT, val, tsp, NULL, 0);
}

static int cxl_futex_wake(_Atomic uint32_t *uaddr, int n)
{
	return syscall(SYS_futex, uaddr, FUTEX_WAKE, n, NULL, NULL, 0);
}

static size_t cxl_align_up(size_t value, size_t align)
{
	return (value + align - 1) & ~(align - 1);
}

static uintptr_t cxl_align_down_uintptr(uintptr_t value, size_t align)
{
	return value & ~((uintptr_t)align - 1);
}

static struct cxl_extent_hdr *cxl_extent_hdr_from_ptr(void *ptr)
{
	return (struct cxl_extent_hdr *)cxl_align_down_uintptr((uintptr_t)ptr,
							       CXL_APP_EXTENT_SIZE);
}

static struct cxl_run_hdr *cxl_run_hdr_from_ptr(void *ptr)
{
	return (struct cxl_run_hdr *)cxl_align_down_uintptr((uintptr_t)ptr,
							    CXL_APP_RUN_SIZE);
}

static uint16_t cxl_pick_size_class(size_t size)
{
	size_t i;

	for (i = 0; i < sizeof(cxl_size_classes) / sizeof(cxl_size_classes[0]);
	     i++) {
		if (size <= cxl_size_classes[i])
			return cxl_size_classes[i];
	}

	return 0;
}

static uint64_t *cxl_run_bitmap(struct cxl_run_hdr *hdr)
{
	return (uint64_t *)(hdr + 1);
}

static void *cxl_large_user_ptr(struct cxl_extent_hdr *hdr)
{
	size_t off = cxl_align_up(sizeof(*hdr), CXL_APP_ALLOC_ALIGN);

	return (char *)hdr + off;
}

static int cxl_wait_launch_go(struct cxl_control_plane *ctl, int timeout_ms)
{
	uint64_t start_ns = cxl_now_ns();
	uint64_t timeout_ns = (timeout_ms < 0) ? UINT64_MAX :
						(uint64_t)timeout_ms * 1000000ULL;

	for (;;) {
		uint32_t go = atomic_load(&ctl->launch_go);
		uint32_t futex_word;

		if (go)
			return 0;
		if (timeout_ns != UINT64_MAX &&
		    cxl_now_ns() - start_ns > timeout_ns)
			return -ETIMEDOUT;

		futex_word = atomic_load(&ctl->launch_futex);
		cxl_futex_wait(&ctl->launch_futex, futex_word, 20);
	}
}

static int cxl_wait_for_children(struct cxl_control_plane *ctl, int nr_machines,
				 int timeout_ms)
{
	uint64_t start_ns = cxl_now_ns();
	uint64_t timeout_ns = (timeout_ms < 0) ? UINT64_MAX :
						(uint64_t)timeout_ms * 1000000ULL;

	for (;;) {
		if (atomic_load(&ctl->launch_ready) >= (uint32_t)nr_machines)
			return 0;
		if (timeout_ns != UINT64_MAX &&
		    cxl_now_ns() - start_ns > timeout_ns)
			return -ETIMEDOUT;
		usleep(1000);
	}
}

static int cxl_app_init_run(struct cxl_run_hdr *hdr, uint16_t size_class)
{
	size_t payload_offset;
	size_t slot_count;
	size_t bitmap_words;
	size_t bitmap_bytes;

	memset(hdr, 0, CXL_APP_RUN_SIZE);
	slot_count = (CXL_APP_RUN_SIZE - sizeof(*hdr)) / size_class;
	while (slot_count > 0) {
		bitmap_words = (slot_count + 63) / 64;
		bitmap_bytes = bitmap_words * sizeof(uint64_t);
		payload_offset =
			cxl_align_up(sizeof(*hdr) + bitmap_bytes, CXL_APP_ALLOC_ALIGN);
		if (payload_offset >= CXL_APP_RUN_SIZE)
			return -EINVAL;
		if ((CXL_APP_RUN_SIZE - payload_offset) / size_class == slot_count)
			break;
		slot_count = (CXL_APP_RUN_SIZE - payload_offset) / size_class;
	}

	if (!slot_count)
		return -EINVAL;

	hdr->magic = CXL_APP_RUN_MAGIC;
	hdr->size_class = size_class;
	hdr->slot_count = slot_count;
	hdr->bitmap_words = (slot_count + 63) / 64;
	hdr->payload_offset = payload_offset;
	return 0;
}

static void *cxl_app_alloc_from_run(struct cxl_run_hdr *hdr)
{
	uint64_t *bitmap;
	uint32_t word_idx;

	if (hdr->magic != CXL_APP_RUN_MAGIC || hdr->size_class == 0)
		return NULL;

	bitmap = cxl_run_bitmap(hdr);
	for (word_idx = 0; word_idx < hdr->bitmap_words; word_idx++) {
		uint32_t bits_left = hdr->slot_count - (word_idx * 64);
		uint64_t valid_mask;
		uint64_t old_word;

		if ((int32_t)bits_left <= 0)
			break;

		valid_mask = (bits_left >= 64) ? ~0ULL : ((1ULL << bits_left) - 1);
		old_word = __atomic_load_n(&bitmap[word_idx], __ATOMIC_SEQ_CST);

		for (;;) {
			uint64_t free_mask = (~old_word) & valid_mask;
			uint64_t bit;
			uint64_t new_word;
			uint32_t slot_idx;

			if (!free_mask)
				break;

			bit = free_mask & (~free_mask + 1);
			new_word = old_word | bit;
			if (!__atomic_compare_exchange_n(&bitmap[word_idx], &old_word,
							 new_word, 0,
							 __ATOMIC_SEQ_CST,
							 __ATOMIC_SEQ_CST))
				continue;

			slot_idx = (word_idx * 64) + __builtin_ctzll(bit);
			return (char *)hdr + hdr->payload_offset +
			       ((size_t)slot_idx * hdr->size_class);
		}
	}

	return NULL;
}

static int cxl_app_free_small(void *ptr)
{
	struct cxl_run_hdr *run = cxl_run_hdr_from_ptr(ptr);
	uint64_t *bitmap;
	uintptr_t payload_base;
	uintptr_t ptr_u = (uintptr_t)ptr;
	uintptr_t offset;
	uint32_t slot_idx;
	uint32_t word_idx;
	uint64_t mask;
	uint64_t old_word;

	if (run->magic != CXL_APP_RUN_MAGIC || run->size_class == 0)
		return -EINVAL;

	payload_base = (uintptr_t)run + run->payload_offset;
	if (ptr_u < payload_base)
		return -EINVAL;

	offset = ptr_u - payload_base;
	if (offset % run->size_class)
		return -EINVAL;

	slot_idx = offset / run->size_class;
	if (slot_idx >= run->slot_count)
		return -EINVAL;

	word_idx = slot_idx / 64;
	mask = 1ULL << (slot_idx % 64);
	bitmap = cxl_run_bitmap(run);
	old_word = __atomic_fetch_and(&bitmap[word_idx], ~mask, __ATOMIC_SEQ_CST);
	if (!(old_word & mask))
		return -EALREADY;

	return 0;
}

static int cxl_app_add_small_extent(struct cxl_app *app, void *base)
{
	struct cxl_extent_hdr *hdr = base;

	if (app->nr_small_extents >= CXL_APP_MAX_OWNED_EXTENTS)
		return -ENOSPC;

	memset(base, 0, CXL_APP_EXTENT_SIZE);
	hdr->magic = CXL_APP_EXTENT_MAGIC;
	hdr->kind = CXL_APP_EXTENT_SMALL;
	hdr->owner_machine = app->machine_id;
	hdr->run_size = CXL_APP_RUN_SIZE;
	hdr->nr_runs = CXL_APP_RUNS_PER_EXTENT;
	hdr->mapped_size = CXL_APP_EXTENT_SIZE;

	app->small_extents[app->nr_small_extents++].base = base;
	return 0;
}

static void *cxl_app_alloc_small_in_extent(struct cxl_app *app, void *extent_base,
					   uint16_t size_class)
{
	uint32_t run_idx;

	(void)app;
	for (run_idx = CXL_APP_FIRST_USABLE_RUN; run_idx < CXL_APP_RUNS_PER_EXTENT;
	     run_idx++) {
		struct cxl_run_hdr *run = (struct cxl_run_hdr *)((char *)extent_base +
							 ((size_t)run_idx *
							  CXL_APP_RUN_SIZE));

		if (run->magic != 0 && run->magic != CXL_APP_RUN_MAGIC)
			continue;
		if (run->size_class != 0 && run->size_class != size_class)
			continue;
		if (run->size_class == 0 && cxl_app_init_run(run, size_class))
			continue;

		if (run->size_class == size_class) {
			void *slot = cxl_app_alloc_from_run(run);

			if (slot)
				return slot;
		}
	}

	return NULL;
}

static void *cxl_app_alloc_small(struct cxl_app *app, size_t size)
{
	uint16_t size_class = cxl_pick_size_class(size);
	size_t i;
	void *slot;
	void *extent = NULL;

	if (!size_class) {
		errno = ENOMEM;
		return NULL;
	}

	pthread_mutex_lock(&app->alloc_lock);
	for (i = 0; i < app->nr_small_extents; i++) {
		slot = cxl_app_alloc_small_in_extent(app, app->small_extents[i].base,
						     size_class);
		if (slot) {
			pthread_mutex_unlock(&app->alloc_lock);
			return slot;
		}
	}

	extent = cxl_macro_malloc(app->rt, CXL_APP_EXTENT_SIZE, app->timeout_ms);
	if (!extent) {
		pthread_mutex_unlock(&app->alloc_lock);
		return NULL;
	}

	if (cxl_app_add_small_extent(app, extent)) {
		cxl_macro_free(app->rt, extent, app->timeout_ms);
		pthread_mutex_unlock(&app->alloc_lock);
		errno = ENOSPC;
		return NULL;
	}

	slot = cxl_app_alloc_small_in_extent(app, extent, size_class);
	pthread_mutex_unlock(&app->alloc_lock);
	if (!slot)
		errno = ENOMEM;
	return slot;
}

static void cxl_app_reap_large(struct cxl_app *app)
{
	size_t i;

	for (i = 0; i < app->nr_large_allocs; i++) {
		struct cxl_extent_hdr *hdr = app->large_allocs[i].hdr;
		uint32_t expected = CXL_APP_LARGE_FREE_PENDING;

		__atomic_compare_exchange_n(&hdr->state, &expected, CXL_APP_LARGE_FREE,
					    0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
	}
}

static void *cxl_app_alloc_large(struct cxl_app *app, size_t size)
{
	size_t i;
	size_t total_size = cxl_align_up(sizeof(struct cxl_extent_hdr),
					 CXL_APP_ALLOC_ALIGN) + size;
	struct cxl_extent_hdr *hdr = NULL;
	void *base = NULL;

	pthread_mutex_lock(&app->alloc_lock);
	cxl_app_reap_large(app);

	for (i = 0; i < app->nr_large_allocs; i++) {
		hdr = app->large_allocs[i].hdr;
		if (__atomic_load_n(&hdr->state, __ATOMIC_SEQ_CST) !=
		    CXL_APP_LARGE_FREE)
			continue;
		if (hdr->mapped_size < total_size)
			continue;

		__atomic_store_n(&hdr->state, CXL_APP_LARGE_LIVE, __ATOMIC_SEQ_CST);
		hdr->user_size = size;
		pthread_mutex_unlock(&app->alloc_lock);
		return app->large_allocs[i].user_ptr;
	}

	base = cxl_macro_malloc(app->rt, total_size, app->timeout_ms);
	if (!base) {
		pthread_mutex_unlock(&app->alloc_lock);
		return NULL;
	}
	if (app->nr_large_allocs >= CXL_APP_MAX_OWNED_EXTENTS) {
		cxl_macro_free(app->rt, base, app->timeout_ms);
		pthread_mutex_unlock(&app->alloc_lock);
		errno = ENOSPC;
		return NULL;
	}

	memset(base, 0, total_size < CXL_APP_EXTENT_SIZE ? CXL_APP_EXTENT_SIZE :
						    cxl_align_up(total_size,
								 CXL_APP_EXTENT_SIZE));
	hdr = base;
	hdr->magic = CXL_APP_EXTENT_MAGIC;
	hdr->kind = CXL_APP_EXTENT_LARGE_HEAD;
	hdr->owner_machine = app->machine_id;
	hdr->state = CXL_APP_LARGE_LIVE;
	hdr->mapped_size = cxl_align_up(total_size, CXL_APP_EXTENT_SIZE);
	hdr->user_size = size;
	app->large_allocs[app->nr_large_allocs].hdr = hdr;
	app->large_allocs[app->nr_large_allocs].user_ptr = cxl_large_user_ptr(hdr);
	app->nr_large_allocs++;
	pthread_mutex_unlock(&app->alloc_lock);
	return cxl_large_user_ptr(hdr);
}

static int cxl_app_free_large(struct cxl_app *app, struct cxl_extent_hdr *hdr)
{
	uint32_t expected = CXL_APP_LARGE_LIVE;

	(void)app;
	if (hdr->magic != CXL_APP_EXTENT_MAGIC ||
	    hdr->kind != CXL_APP_EXTENT_LARGE_HEAD)
		return -EINVAL;

	if (!__atomic_compare_exchange_n(&hdr->state, &expected,
					 CXL_APP_LARGE_FREE_PENDING, 0,
					 __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
		if (expected == CXL_APP_LARGE_FREE_PENDING ||
		    expected == CXL_APP_LARGE_FREE)
			return -EALREADY;
		return -EINVAL;
	}

	return 0;
}

static int cxl_app_reclaim_owned_extents(struct cxl_app *app)
{
	size_t i;
	int first_rc = 0;

	if (!app || !app->rt)
		return -EINVAL;

	for (i = 0; i < app->nr_large_allocs; i++) {
		struct cxl_extent_hdr *hdr = app->large_allocs[i].hdr;
		int rc;

		if (!hdr)
			continue;
		if (hdr->magic != CXL_APP_EXTENT_MAGIC ||
		    hdr->kind != CXL_APP_EXTENT_LARGE_HEAD)
			continue;
		if (hdr->owner_machine != app->machine_id)
			continue;

		rc = cxl_macro_free(app->rt, hdr, app->timeout_ms);
		if (rc && rc != -ENOENT && !first_rc)
			first_rc = rc;
		app->large_allocs[i].hdr = NULL;
		app->large_allocs[i].user_ptr = NULL;
	}
	app->nr_large_allocs = 0;

	for (i = 0; i < app->nr_small_extents; i++) {
		void *base = app->small_extents[i].base;
		struct cxl_extent_hdr *hdr = base;
		int rc;

		if (!base)
			continue;
		if (hdr->magic != CXL_APP_EXTENT_MAGIC ||
		    hdr->kind != CXL_APP_EXTENT_SMALL)
			continue;
		if (hdr->owner_machine != app->machine_id)
			continue;

		rc = cxl_macro_free(app->rt, base, app->timeout_ms);
		if (rc && rc != -ENOENT && !first_rc)
			first_rc = rc;
		app->small_extents[i].base = NULL;
	}
	app->nr_small_extents = 0;

	return first_rc;
}

static int cxl_app_child_main(int machine_id, const struct cxl_app_cfg *cfg,
			      uint64_t control_alloc_id, uint64_t control_size,
			      cxl_app_main_fn_t app_main)
{
	struct cxl_macro_cfg macro_cfg;
	struct cxl_macro_runtime *rt = NULL;
	struct cxl_app app;
	int rc;

	memset(&macro_cfg, 0, sizeof(macro_cfg));
	macro_cfg.machine_id = machine_id;
	macro_cfg.nr_machines = cfg->nr_machines;
	macro_cfg.window_base = cfg->window_base;
	macro_cfg.window_size = cfg->window_size;
	macro_cfg.control_alloc_id = control_alloc_id;
	macro_cfg.control_size = control_size;

	rc = cxl_macro_init(&rt, &macro_cfg);
	if (rc)
		return 2;

	memset(&app, 0, sizeof(app));
	app.machine_id = machine_id;
	app.nr_machines = cfg->nr_machines;
	app.timeout_ms = cfg->timeout_ms;
	app.window_base = cfg->window_base;
	app.window_size = cfg->window_size;
	app.rt = rt;
	app.ctl = cxl_macro_ctl(rt);
	pthread_mutex_init(&app.alloc_lock, NULL);
	cxl_process_app = &app;
	cxl_set_current_app(&app);

	rc = cxl_install_workers(&app);
	if (rc) {
		pthread_mutex_destroy(&app.alloc_lock);
		cxl_macro_destroy(rt);
		return 2;
	}

	atomic_fetch_add(&app.ctl->launch_ready, 1);
	atomic_fetch_add(&app.ctl->launch_futex, 1);
	cxl_futex_wake(&app.ctl->launch_futex, INT_MAX);

	rc = cxl_wait_launch_go(app.ctl, cfg->timeout_ms);
	if (!rc && app_main)
		rc = app_main(&app);
	if (!cxl_app_reclaim_owned_extents(&app) && rc == 0) {
		/* nothing */
	} else if (rc == 0) {
		rc = 1;
	}

	pthread_mutex_destroy(&app.alloc_lock);
	cxl_set_current_app(NULL);
	cxl_process_app = NULL;
	cxl_macro_destroy(rt);
	return rc ? 2 : 0;
}

static int cxl_app_run_bridge(struct cxl_app *app)
{
	(void)app;
	if (!cxl_run_main_fn)
		return -EINVAL;
	return cxl_run_main_fn();
}

static void cxl_terminate_children(pid_t *pids, int nr_machines, int except_idx,
				       bool *terminated)
{
	int i;

	for (i = 0; i < nr_machines; i++) {
		if (i == except_idx || pids[i] <= 0)
			continue;
		if (kill(pids[i], SIGTERM) == 0 || errno == ESRCH)
			terminated[i] = true;
	}
}

int cxl_app_run(const struct cxl_app_cfg *cfg, cxl_app_main_fn_t app_main)
{
	struct cxl_app_cfg local_cfg;
	int fd = -1;
	uint64_t control_alloc_id = 0;
	uint64_t control_size = 0;
	struct cxl_control_plane *ctl = NULL;
	struct cxl_free_req free_req;
	pid_t pids[CXL_MACRO_MAX_MACHINES] = { 0 };
	bool terminated[CXL_MACRO_MAX_MACHINES] = { false };
	int exit_code = 0;
	int remaining = 0;
	int i;

	if (!cfg || !app_main)
		return -EINVAL;
	if (cxl_worker_registry_error)
		return cxl_worker_registry_error;

	local_cfg = *cfg;
	if (local_cfg.nr_machines <= 0)
		local_cfg.nr_machines = 2;
	if (local_cfg.nr_machines > CXL_MACRO_MAX_MACHINES)
		return -EINVAL;
	if (!local_cfg.window_base)
		local_cfg.window_base = 0x4000000000ULL;
	if (!local_cfg.window_size)
		local_cfg.window_size = 256ULL * 1024 * 1024;
	if (!local_cfg.timeout_ms)
		local_cfg.timeout_ms = 5000;

	fd = open(CXL_DEV_PATH, O_RDWR);
	if (fd < 0)
		return -errno;

	if (cxl_macro_control_create(fd, local_cfg.nr_machines,
				     local_cfg.window_base,
				     local_cfg.window_size,
				     &control_alloc_id, &control_size, &ctl)) {
		close(fd);
		return -errno;
	}

	for (i = 0; i < local_cfg.nr_machines; i++) {
		pid_t pid = fork();

		if (pid < 0) {
			exit_code = 1;
			break;
		}
		if (pid == 0) {
			int child_rc = cxl_app_child_main(i, &local_cfg,
							 control_alloc_id,
							 control_size,
							 app_main);

			fflush(NULL);
			_exit(child_rc);
		}
		pids[i] = pid;
		remaining++;
	}

	if (exit_code != 0)
		cxl_terminate_children(pids, local_cfg.nr_machines, -1, terminated);

	if (exit_code == 0 &&
	    cxl_wait_for_children(ctl, local_cfg.nr_machines,
				  local_cfg.timeout_ms)) {
		exit_code = 1;
		cxl_terminate_children(pids, local_cfg.nr_machines, -1, terminated);
	}

	if (exit_code == 0) {
		atomic_store(&ctl->launch_go, 1);
		atomic_fetch_add(&ctl->launch_futex, 1);
		cxl_futex_wake(&ctl->launch_futex, INT_MAX);
	}

	while (remaining > 0) {
		pid_t pid;
		int st;
		int idx = -1;

		pid = waitpid(-1, &st, 0);
		if (pid < 0) {
			exit_code = 1;
			continue;
		}
		for (i = 0; i < local_cfg.nr_machines; i++) {
			if (pids[i] == pid) {
				idx = i;
				pids[i] = 0;
				remaining--;
				break;
			}
		}
		if (idx < 0)
			continue;

		if (idx == 0) {
			if (!WIFEXITED(st) || WEXITSTATUS(st) != 0)
				exit_code = 1;
			cxl_terminate_children(pids, local_cfg.nr_machines, 0,
					       terminated);
			continue;
		}

		if (terminated[idx])
			continue;

		if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
			exit_code = 1;
			cxl_terminate_children(pids, local_cfg.nr_machines, idx,
					       terminated);
		}
	}

	munmap(ctl, control_size);
	memset(&free_req, 0, sizeof(free_req));
	free_req.alloc_id = control_alloc_id;
	if (ioctl(fd, CXL_FREE, &free_req) < 0)
		exit_code = 1;
	close(fd);
	return exit_code ? 1 : 0;
}

int cxl_run(const struct cxl_app_cfg *cfg, cxl_main_fn_t main_fn)
{
	int rc;

	if (!main_fn)
		return -EINVAL;

	cxl_run_main_fn = main_fn;
	rc = cxl_app_run(cfg, cxl_app_run_bridge);
	cxl_run_main_fn = NULL;
	return rc;
}

int cxl_machine_id(struct cxl_app *app)
{
	return app ? app->machine_id : -1;
}

int cxl_machine_count(struct cxl_app *app)
{
	return app ? app->nr_machines : -1;
}

int cxl_current_machine(void)
{
	return cxl_machine_id(cxl_get_current_app());
}

int cxl_current_machine_count(void)
{
	return cxl_machine_count(cxl_get_current_app());
}

int cxl_mutex_init(struct cxl_mutex *lock)
{
	int rc = cxl_mutex_is_shared(lock, NULL);

	if (rc)
		return rc;

	atomic_store_explicit(&lock->state, 0, memory_order_relaxed);
	atomic_store_explicit(&lock->magic, CXL_MUTEX_MAGIC, memory_order_release);
	atomic_store_explicit(&lock->reserved, 0, memory_order_relaxed);
	atomic_store_explicit(&lock->pad, 0, memory_order_relaxed);
	return 0;
}

int cxl_mutex_destroy(struct cxl_mutex *lock)
{
	uint32_t state;
	int rc = cxl_mutex_validate_live(lock);

	if (rc)
		return rc;

	state = atomic_load_explicit(&lock->state, memory_order_acquire);
	if (state != 0)
		return -EBUSY;

	atomic_store_explicit(&lock->magic, 0, memory_order_release);
	atomic_store_explicit(&lock->reserved, 0, memory_order_relaxed);
	atomic_store_explicit(&lock->pad, 0, memory_order_relaxed);
	return 0;
}

struct cxl_mutex *cxl_mutex_create(void)
{
	struct cxl_mutex *lock;

	lock = cxl_malloc_current(sizeof(*lock));
	if (!lock)
		return NULL;
	if (cxl_mutex_init(lock)) {
		cxl_free_current(lock);
		errno = EINVAL;
		return NULL;
	}
	return lock;
}

int cxl_mutex_free(struct cxl_mutex *lock)
{
	int rc;

	rc = cxl_mutex_destroy(lock);
	if (rc)
		return rc;
	return cxl_free_current(lock);
}

int cxl_mutex_lock(struct cxl_mutex *lock)
{
	unsigned int spins = 0;
	unsigned int delay = 4;
	int rc = cxl_mutex_fast_validate(lock);

	if (rc)
		return rc;

	for (;;) {
		uint32_t expected = 0;

		while (atomic_load_explicit(&lock->state, memory_order_acquire) != 0)
			cxl_mutex_backoff(&spins, &delay);

		if (atomic_compare_exchange_weak_explicit(&lock->state, &expected, 1,
							  memory_order_acq_rel,
							  memory_order_acquire))
			return 0;
		cxl_mutex_backoff(&spins, &delay);
	}
}

int cxl_mutex_unlock(struct cxl_mutex *lock)
{
	int rc = cxl_mutex_fast_validate(lock);

	if (rc)
		return rc;

	atomic_store_explicit(&lock->state, 0, memory_order_release);
	return 0;
}

int cxl_mutex_trylock(struct cxl_mutex *lock)
{
	uint32_t expected = 0;
	int rc = cxl_mutex_fast_validate(lock);

	if (rc)
		return rc;

	if (!atomic_compare_exchange_strong_explicit(&lock->state, &expected, 1,
						     memory_order_acq_rel,
						     memory_order_acquire))
		return -EBUSY;

	return 0;
}

void *cxl_malloc(struct cxl_app *app, size_t size)
{
	if (!app || !size) {
		errno = EINVAL;
		return NULL;
	}

	if (size <= cxl_size_classes[(sizeof(cxl_size_classes) /
				      sizeof(cxl_size_classes[0])) - 1])
		return cxl_app_alloc_small(app, size);

	return cxl_app_alloc_large(app, size);
}

int cxl_free(struct cxl_app *app, void *ptr)
{
	struct cxl_extent_hdr *extent;

	if (!app || !ptr)
		return -EINVAL;

	extent = cxl_extent_hdr_from_ptr(ptr);
	if (extent->magic != CXL_APP_EXTENT_MAGIC)
		return -EINVAL;

	switch (extent->kind) {
	case CXL_APP_EXTENT_SMALL:
		return cxl_app_free_small(ptr);
	case CXL_APP_EXTENT_LARGE_HEAD:
		return cxl_app_free_large(app, extent);
	default:
		return -EINVAL;
	}
}

void *cxl_malloc_current(size_t size)
{
	return cxl_malloc(cxl_get_current_app(), size);
}

int cxl_free_current(void *ptr)
{
	return cxl_free(cxl_get_current_app(), ptr);
}

int cxl_app_spawn(struct cxl_app *app, cxl_worker_fn_t worker,
		  int target_machine, void *shared_arg)
{
	struct cxl_worker_desc *desc;

	if (!app || !worker)
		return -EINVAL;
	if (target_machine < 0 || target_machine >= app->nr_machines)
		return -EINVAL;
	if (!cxl_ptr_is_shared(app, shared_arg))
		return -ERANGE;
	if (cxl_worker_registry_error)
		return cxl_worker_registry_error;

	pthread_mutex_lock(&cxl_worker_lock);
	desc = cxl_find_worker_by_fn(worker);
	pthread_mutex_unlock(&cxl_worker_lock);
	if (!desc)
		return -ENOENT;

	if (target_machine == app->machine_id) {
		pthread_t tid;
		int rc = pthread_create(&tid, NULL, desc->entry_fn, shared_arg);

		if (rc)
			return -rc;
		pthread_detach(tid);
		return 0;
	}

	return cxl_spawn_remote(app->rt, target_machine, desc->id,
				(uint64_t)(uintptr_t)shared_arg, NULL,
				app->timeout_ms);
}

int cxl_spawn(cxl_worker_fn_t worker, int target_machine, void *shared_arg)
{
	return cxl_app_spawn(cxl_get_current_app(), worker, target_machine,
			     shared_arg);
}

int cxl_app_store_u32(struct cxl_app *app, unsigned int slot, uint32_t value)
{
	if (!app || slot >= CXL_APP_MAX_MAILBOX)
		return -EINVAL;

	__atomic_store_n(&app->ctl->app_mailbox32[slot], value, __ATOMIC_SEQ_CST);
	return 0;
}

uint32_t cxl_app_load_u32(struct cxl_app *app, unsigned int slot)
{
	if (!app || slot >= CXL_APP_MAX_MAILBOX)
		return 0;

	return __atomic_load_n(&app->ctl->app_mailbox32[slot], __ATOMIC_SEQ_CST);
}

int cxl_app_store_ptr(struct cxl_app *app, unsigned int slot, const void *ptr)
{
	uintptr_t ptr_u;
	uint64_t base;
	uint64_t window_size;
	uint32_t encoded;

	if (!app || slot >= CXL_APP_MAX_MAILBOX || !ptr)
		return -EINVAL;

	ptr_u = (uintptr_t)ptr;
	base = app->ctl->window_base;
	window_size = app->ctl->window_size;
	if (ptr_u < base || ptr_u >= base + window_size)
		return -ERANGE;

	if ((ptr_u - base) > UINT32_MAX - 1)
		return -ERANGE;

	encoded = (uint32_t)(ptr_u - base) + 1;
	return cxl_app_store_u32(app, slot, encoded);
}

void *cxl_app_load_ptr(struct cxl_app *app, unsigned int slot)
{
	uint32_t encoded;

	if (!app || slot >= CXL_APP_MAX_MAILBOX)
		return NULL;

	encoded = cxl_app_load_u32(app, slot);
	if (encoded == 0)
		return NULL;

	return (void *)(uintptr_t)(app->ctl->window_base + (uint64_t)(encoded - 1));
}

int cxl_store_u32(unsigned int slot, uint32_t value)
{
	return cxl_app_store_u32(cxl_get_current_app(), slot, value);
}

uint32_t cxl_load_u32(unsigned int slot)
{
	return cxl_app_load_u32(cxl_get_current_app(), slot);
}

int cxl_store_ptr(unsigned int slot, const void *ptr)
{
	return cxl_app_store_ptr(cxl_get_current_app(), slot, ptr);
}

void *cxl_load_ptr(unsigned int slot)
{
	return cxl_app_load_ptr(cxl_get_current_app(), slot);
}

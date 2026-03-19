#define _GNU_SOURCE

#include "cxl_macro_runtime.h"

#include "../cxl_pool.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/futex.h>
#include <sched.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

#define CXL_DEV_PATH "/dev/cxl_pool"
#define CXL_MAX_ENTRIES 128

struct cxl_local_alloc {
	void *addr;
	uint64_t alloc_id;
	uint64_t va_offset;
	uint64_t size;
	struct cxl_local_alloc *next;
};

struct cxl_entry {
	uint32_t id;
	cxl_entry_fn_t fn;
	char name[48];
};

struct cxl_spawn_ctx {
	cxl_entry_fn_t fn;
	uint64_t arg_u64;
};

struct cxl_macro_runtime {
	int fd;
	int machine_id;
	int nr_machines;
	int macro_id;
	uint64_t machine_bit;
	uint64_t membership_mask;
	uint64_t window_base;
	uint64_t window_size;

	uint64_t control_alloc_id;
	uint64_t control_size;
	struct cxl_control_plane *ctl;
	void *window_token;

	pthread_t rx_thread;
	atomic_int local_stop;
	atomic_int started;

	pthread_mutex_t entry_lock;
	struct cxl_entry entries[CXL_MAX_ENTRIES];
	int nr_entries;

	pthread_mutex_t alloc_lock;
	struct cxl_local_alloc *allocs;
};

static uint64_t cxl_now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static int cxl_futex_wait(atomic_uint_fast32_t *uaddr, uint32_t val, int ms)
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

static int cxl_futex_wake(atomic_uint_fast32_t *uaddr, int n)
{
	return syscall(SYS_futex, uaddr, FUTEX_WAKE, n, NULL, NULL, 0);
}

static int cxl_set_barrier_status(struct cxl_barrier *b, int status)
{
	int expected = 0;

	if (status >= 0)
		return 0;
	atomic_compare_exchange_strong(&b->status, &expected, status);
	return 0;
}

static int cxl_barrier_init(struct cxl_macro_runtime *rt, uint64_t cmd_id,
			    uint64_t expected_mask, struct cxl_barrier **out)
{
	uint64_t start_ns = cxl_now_ns();
	struct cxl_barrier *b;

	b = &rt->ctl->barriers[cmd_id % CXL_MACRO_MAX_BARRIERS];
	for (;;) {
		uint64_t expected = 0;

		if (atomic_compare_exchange_strong(&b->in_use, &expected, 1))
			break;
		if (cxl_now_ns() - start_ns > 3000000000ULL)
			return -EBUSY;
		sched_yield();
	}

	atomic_store(&b->cmd_id, cmd_id);
	atomic_store(&b->expected_mask, expected_mask);
	atomic_store(&b->ack_mask, 0);
	atomic_store(&b->status, 0);
	atomic_fetch_add(&b->futex_word, 1);
	*out = b;
	return 0;
}

static void cxl_barrier_fini(struct cxl_barrier *b)
{
	atomic_store(&b->cmd_id, 0);
	atomic_store(&b->expected_mask, 0);
	atomic_store(&b->ack_mask, 0);
	atomic_store(&b->status, 0);
	atomic_store(&b->in_use, 0);
}

static int cxl_barrier_ack(struct cxl_macro_runtime *rt, uint64_t cmd_id,
			   uint64_t machine_bit, int status)
{
	struct cxl_barrier *b = &rt->ctl->barriers[cmd_id % CXL_MACRO_MAX_BARRIERS];
	uint64_t cur_cmd = atomic_load(&b->cmd_id);

	if (!atomic_load(&b->in_use) || cur_cmd != cmd_id)
		return -ENOENT;

	atomic_fetch_or(&b->ack_mask, machine_bit);
	cxl_set_barrier_status(b, status);
	atomic_fetch_add(&b->futex_word, 1);
	cxl_futex_wake(&b->futex_word, INT32_MAX);
	return 0;
}

static int cxl_barrier_wait(struct cxl_barrier *b, int timeout_ms)
{
	uint64_t start_ns = cxl_now_ns();
	uint64_t timeout_ns;

	timeout_ns = (timeout_ms < 0) ? UINT64_MAX :
				      (uint64_t)timeout_ms * 1000000ULL;

	for (;;) {
		uint64_t expected = atomic_load(&b->expected_mask);
		uint64_t ack = atomic_load(&b->ack_mask);
		int status = atomic_load(&b->status);
		uint32_t futex_word;

		if (status < 0)
			return status;
		if ((ack & expected) == expected)
			return 0;
		if (timeout_ns != UINT64_MAX &&
		    cxl_now_ns() - start_ns > timeout_ns)
			return -ETIMEDOUT;

		futex_word = atomic_load(&b->futex_word);
		cxl_futex_wait(&b->futex_word, futex_word, 20);
	}
}

static int cxl_queue_push(struct cxl_machine_queue *q, const struct cxl_cmd *cmd)
{
	uint64_t seq;
	struct cxl_queue_slot *slot;
	uint64_t start_ns = cxl_now_ns();

	seq = atomic_fetch_add(&q->prod_seq, 1);
	slot = &q->slots[seq % CXL_MACRO_QUEUE_DEPTH];

	while (atomic_load(&slot->ready_seq) != 0) {
		if (cxl_now_ns() - start_ns > 5000000000ULL)
			return -ETIMEDOUT;
		sched_yield();
	}

	slot->cmd = *cmd;
	atomic_store_explicit(&slot->ready_seq, seq + 1, memory_order_release);
	atomic_fetch_add(&q->doorbell, 1);
	cxl_futex_wake(&q->doorbell, 1);
	return 0;
}

static int cxl_queue_pop_wait(struct cxl_machine_queue *q, struct cxl_cmd *cmd,
			      int timeout_ms)
{
	uint64_t seq;
	struct cxl_queue_slot *slot;
	uint32_t db;
	uint64_t start_ns = cxl_now_ns();
	uint64_t timeout_ns;

	timeout_ns = (timeout_ms < 0) ? UINT64_MAX :
				      (uint64_t)timeout_ms * 1000000ULL;

	for (;;) {
		seq = atomic_load(&q->cons_seq);
		slot = &q->slots[seq % CXL_MACRO_QUEUE_DEPTH];
		if (atomic_load_explicit(&slot->ready_seq, memory_order_acquire) ==
		    seq + 1) {
			*cmd = slot->cmd;
			atomic_store_explicit(&slot->ready_seq, 0,
					      memory_order_release);
			atomic_store(&q->cons_seq, seq + 1);
			return 0;
		}

		if (timeout_ns != UINT64_MAX &&
		    cxl_now_ns() - start_ns > timeout_ns)
			return -ETIMEDOUT;

		db = atomic_load(&q->doorbell);
		cxl_futex_wait(&q->doorbell, db, 20);
	}
}

static int cxl_map_fixed(struct cxl_macro_runtime *rt, uint64_t alloc_id,
			 uint64_t va_offset, uint64_t size, int prot)
{
	void *addr = (void *)(uintptr_t)(rt->window_base + va_offset);
	off_t off = (off_t)alloc_id * getpagesize();
	void *ret;

	ret = mmap(addr, size, prot, MAP_SHARED | MAP_FIXED, rt->fd, off);
	if (ret == MAP_FAILED)
		return -errno;
	return 0;
}

static int cxl_unmap_fixed(struct cxl_macro_runtime *rt, uint64_t va_offset,
			   uint64_t size)
{
	void *addr = (void *)(uintptr_t)(rt->window_base + va_offset);
	void *ret;

	if (munmap(addr, size) < 0)
		return -errno;

	ret = mmap(addr, size, PROT_NONE,
		   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
	if (ret == MAP_FAILED)
		return -errno;
	return 0;
}

static cxl_entry_fn_t cxl_lookup_entry(struct cxl_macro_runtime *rt, uint32_t id)
{
	int i;
	cxl_entry_fn_t fn = NULL;

	pthread_mutex_lock(&rt->entry_lock);
	for (i = 0; i < rt->nr_entries; i++) {
		if (rt->entries[i].id == id) {
			fn = rt->entries[i].fn;
			break;
		}
	}
	pthread_mutex_unlock(&rt->entry_lock);
	return fn;
}

static void *cxl_spawn_trampoline(void *arg)
{
	struct cxl_spawn_ctx *ctx = arg;

	ctx->fn((void *)(uintptr_t)ctx->arg_u64);
	free(ctx);
	return NULL;
}

static int cxl_handle_cmd(struct cxl_macro_runtime *rt, const struct cxl_cmd *cmd)
{
	int ret = 0;
	pthread_t tid;
	struct cxl_spawn_ctx *spawn_ctx;
	cxl_entry_fn_t fn;

	if ((int)cmd->macro_id != rt->macro_id)
		return 0;

	switch (cmd->type) {
	case CXL_CMD_MAP_REQ:
		ret = cxl_map_fixed(rt, cmd->alloc_id, cmd->va_offset, cmd->size,
				    cmd->prot);
		cxl_barrier_ack(rt, cmd->cmd_id, rt->machine_bit, ret);
		break;

	case CXL_CMD_UNMAP_REQ:
		ret = cxl_unmap_fixed(rt, cmd->va_offset, cmd->size);
		cxl_barrier_ack(rt, cmd->cmd_id, rt->machine_bit, ret);
		break;

	case CXL_CMD_SPAWN_REQ:
		fn = cxl_lookup_entry(rt, cmd->entry_id);
		if (!fn) {
			cxl_barrier_ack(rt, cmd->cmd_id, rt->machine_bit, -ENOENT);
			break;
		}
		spawn_ctx = calloc(1, sizeof(*spawn_ctx));
		if (!spawn_ctx) {
			cxl_barrier_ack(rt, cmd->cmd_id, rt->machine_bit, -ENOMEM);
			break;
		}
		spawn_ctx->fn = fn;
		spawn_ctx->arg_u64 = cmd->arg_u64;
		ret = pthread_create(&tid, NULL, cxl_spawn_trampoline, spawn_ctx);
		if (ret) {
			free(spawn_ctx);
			cxl_barrier_ack(rt, cmd->cmd_id, rt->machine_bit, -ret);
			break;
		}
		pthread_detach(tid);
		cxl_barrier_ack(rt, cmd->cmd_id, rt->machine_bit, 0);
		break;

	case CXL_CMD_FATAL:
		atomic_store(&rt->ctl->macros[rt->macro_id].fatal_flag, 1);
		cxl_barrier_ack(rt, cmd->cmd_id, rt->machine_bit, 0);
		break;

	default:
		break;
	}

	return 0;
}

static void cxl_update_heartbeat(struct cxl_macro_runtime *rt)
{
	atomic_store(&rt->ctl->machine_heartbeat_ns[rt->machine_id], cxl_now_ns());
}

static void cxl_check_membership_failure(struct cxl_macro_runtime *rt)
{
	uint64_t now_ns;
	uint64_t timeout_ns;
	uint64_t ready_mask;
	int i;

	timeout_ns = rt->ctl->heartbeat_timeout_ns;
	if (!timeout_ns)
		return;

	ready_mask = atomic_load(&rt->ctl->ready_mask);
	if ((ready_mask & rt->membership_mask) != rt->membership_mask)
		return;

	now_ns = cxl_now_ns();
	for (i = 0; i < rt->nr_machines; i++) {
		uint64_t bit = 1ULL << i;
		uint64_t hb;

		if (!(rt->membership_mask & bit))
			continue;
		hb = atomic_load(&rt->ctl->machine_heartbeat_ns[i]);
		if (!hb)
			continue;
		if (now_ns - hb <= timeout_ns)
			continue;

		atomic_store(&rt->ctl->macros[rt->macro_id].fatal_flag, 1);
		atomic_store(&rt->ctl->stop_flag, 1);
		cxl_futex_wake(&rt->ctl->queues[rt->machine_id].doorbell, 1);
		break;
	}
}

static void *cxl_rx_loop(void *arg)
{
	struct cxl_macro_runtime *rt = arg;
	struct cxl_machine_queue *q = &rt->ctl->queues[rt->machine_id];

	atomic_store(&rt->started, 1);

	while (!atomic_load(&rt->local_stop)) {
		struct cxl_cmd cmd;

		if (atomic_load(&rt->ctl->stop_flag))
			break;
		if (atomic_load(&rt->ctl->macros[rt->macro_id].fatal_flag))
			break;

		cxl_update_heartbeat(rt);
		cxl_check_membership_failure(rt);
		if (cxl_queue_pop_wait(q, &cmd, 50) == 0)
			cxl_handle_cmd(rt, &cmd);
	}

	return NULL;
}

static int cxl_send_all(struct cxl_macro_runtime *rt, const struct cxl_cmd *base_cmd,
			uint64_t mask)
{
	int i;

	for (i = 0; i < rt->nr_machines; i++) {
		struct cxl_cmd cmd = *base_cmd;
		int ret;

		if (!(mask & (1ULL << i)))
			continue;
		cmd.dst_machine = i;
		ret = cxl_queue_push(&rt->ctl->queues[i], &cmd);
		if (ret)
			return ret;
	}
	return 0;
}

static struct cxl_local_alloc *cxl_find_alloc(struct cxl_macro_runtime *rt,
					       void *ptr)
{
	struct cxl_local_alloc *it;

	for (it = rt->allocs; it; it = it->next) {
		if (it->addr == ptr)
			return it;
	}
	return NULL;
}

static void cxl_add_local_alloc(struct cxl_macro_runtime *rt,
				struct cxl_local_alloc *alloc)
{
	alloc->next = rt->allocs;
	rt->allocs = alloc;
}

static void cxl_remove_local_alloc(struct cxl_macro_runtime *rt,
				   struct cxl_local_alloc *alloc)
{
	struct cxl_local_alloc **pp = &rt->allocs;

	while (*pp) {
		if (*pp == alloc) {
			*pp = alloc->next;
			return;
		}
		pp = &(*pp)->next;
	}
}

static void cxl_maprec_add(struct cxl_macro_runtime *rt, uint64_t alloc_id,
			   uint64_t off, uint64_t size, int prot)
{
	int i;

	for (i = 0; i < CXL_MACRO_MAX_MAP_RECS; i++) {
		uint64_t expected = 0;
		struct cxl_map_record *rec = &rt->ctl->maps[i];

		if (!atomic_compare_exchange_strong(&rec->active, &expected, 1))
			continue;
		rec->alloc_id = alloc_id;
		rec->va_offset = off;
		rec->size = size;
		rec->prot = prot;
		rec->macro_id = rt->macro_id;
		return;
	}
}

static void cxl_maprec_del(struct cxl_macro_runtime *rt, uint64_t alloc_id)
{
	int i;

	for (i = 0; i < CXL_MACRO_MAX_MAP_RECS; i++) {
		struct cxl_map_record *rec = &rt->ctl->maps[i];

		if (!atomic_load(&rec->active))
			continue;
		if (rec->macro_id != (uint32_t)rt->macro_id)
			continue;
		if (rec->alloc_id != alloc_id)
			continue;
		atomic_store(&rec->active, 0);
		return;
	}
}

int cxl_macro_control_create(int fd, int nr_machines, uint64_t window_base,
			     uint64_t window_size, uint64_t timeout_ns,
			     uint64_t *alloc_id_out, uint64_t *mapped_size_out,
			     struct cxl_control_plane **ctl_out)
{
	struct cxl_alloc_req req;
	struct cxl_control_plane *ctl;
	off_t off;

	if (!alloc_id_out || !mapped_size_out || !ctl_out)
		return -EINVAL;
	if (nr_machines <= 0 || nr_machines > CXL_MACRO_MAX_MACHINES)
		return -EINVAL;

	memset(&req, 0, sizeof(req));
	req.size = sizeof(struct cxl_control_plane);
	if (ioctl(fd, CXL_ALLOC, &req) < 0)
		return -errno;

	off = (off_t)req.alloc_id * getpagesize();
	ctl = mmap(NULL, req.mapped_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
		   off);
	if (ctl == MAP_FAILED) {
		struct cxl_free_req free_req = { .alloc_id = req.alloc_id };

		ioctl(fd, CXL_FREE, &free_req);
		return -errno;
	}

	memset(ctl, 0, req.mapped_size);
	ctl->magic = CXL_MACRO_CTL_MAGIC;
	ctl->version = CXL_MACRO_CTL_VERSION;
	ctl->nr_machines = nr_machines;
	ctl->window_base = window_base;
	ctl->window_size = window_size;
	ctl->heartbeat_timeout_ns = timeout_ns;
	atomic_store(&ctl->global_cmd_id, 1);

	*alloc_id_out = req.alloc_id;
	*mapped_size_out = req.mapped_size;
	*ctl_out = ctl;
	return 0;
}

int cxl_macro_init(struct cxl_macro_runtime **rt_out,
		   const struct cxl_macro_cfg *cfg)
{
	struct cxl_macro_runtime *rt;
	off_t off;
	void *ret;
	struct cxl_macro_state *macro;
	int rc;

	if (!rt_out || !cfg)
		return -EINVAL;
	if (cfg->machine_id < 0 || cfg->machine_id >= cfg->nr_machines)
		return -EINVAL;
	if (cfg->nr_machines <= 0 || cfg->nr_machines > CXL_MACRO_MAX_MACHINES)
		return -EINVAL;
	if (cfg->macro_id < 0 || cfg->macro_id >= CXL_MACRO_MAX_MACROS)
		return -EINVAL;

	rt = calloc(1, sizeof(*rt));
	if (!rt)
		return -ENOMEM;

	rt->fd = open(CXL_DEV_PATH, O_RDWR);
	if (rt->fd < 0) {
		rc = -errno;
		goto err_rt;
	}

	rt->machine_id = cfg->machine_id;
	rt->nr_machines = cfg->nr_machines;
	rt->macro_id = cfg->macro_id;
	rt->machine_bit = 1ULL << cfg->machine_id;
	rt->membership_mask = cfg->membership_mask;
	rt->window_base = cfg->window_base;
	rt->window_size = cfg->window_size;
	rt->control_alloc_id = cfg->control_alloc_id;
	rt->control_size = cfg->control_size;

	off = (off_t)cfg->control_alloc_id * getpagesize();
	rt->ctl = mmap(NULL, cfg->control_size, PROT_READ | PROT_WRITE, MAP_SHARED,
		       rt->fd, off);
	if (rt->ctl == MAP_FAILED) {
		rc = -errno;
		goto err_fd;
	}
	if (rt->ctl->magic != CXL_MACRO_CTL_MAGIC ||
	    rt->ctl->version != CXL_MACRO_CTL_VERSION) {
		rc = -EINVAL;
		goto err_ctl;
	}

	if (cfg->nr_machines != (int)rt->ctl->nr_machines) {
		rc = -EINVAL;
		goto err_ctl;
	}

	ret = mmap((void *)(uintptr_t)cfg->window_base, cfg->window_size, PROT_NONE,
		   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
	if (ret == MAP_FAILED) {
		rc = -errno;
		goto err_ctl;
	}
	rt->window_token = ret;

	pthread_mutex_init(&rt->entry_lock, NULL);
	pthread_mutex_init(&rt->alloc_lock, NULL);
	atomic_store(&rt->local_stop, 0);
	atomic_store(&rt->started, 0);

	macro = &rt->ctl->macros[rt->macro_id];
	macro->window_size = cfg->window_size;
	atomic_store(&macro->membership_mask, cfg->membership_mask);
	atomic_fetch_or(&macro->membership_mask, rt->machine_bit);

	rc = pthread_create(&rt->rx_thread, NULL, cxl_rx_loop, rt);
	if (rc) {
		rc = -rc;
		goto err_window;
	}

	while (!atomic_load(&rt->started))
		sched_yield();

	*rt_out = rt;
	return 0;

err_window:
	munmap((void *)(uintptr_t)cfg->window_base, cfg->window_size);
err_ctl:
	munmap(rt->ctl, cfg->control_size);
err_fd:
	close(rt->fd);
err_rt:
	free(rt);
	return rc;
}

void cxl_macro_destroy(struct cxl_macro_runtime *rt)
{
	struct cxl_local_alloc *it, *next;

	if (!rt)
		return;

	atomic_store(&rt->local_stop, 1);
	atomic_fetch_add(&rt->ctl->queues[rt->machine_id].doorbell, 1);
	cxl_futex_wake(&rt->ctl->queues[rt->machine_id].doorbell, 1);
	pthread_join(rt->rx_thread, NULL);

	pthread_mutex_lock(&rt->alloc_lock);
	for (it = rt->allocs; it; it = next) {
		it = rt->allocs;
		next = it->next;
		munmap(it->addr, it->size);
		free(it);
		rt->allocs = next;
	}
	pthread_mutex_unlock(&rt->alloc_lock);

	munmap((void *)(uintptr_t)rt->window_base, rt->window_size);
	munmap(rt->ctl, rt->control_size);
	close(rt->fd);
	pthread_mutex_destroy(&rt->entry_lock);
	pthread_mutex_destroy(&rt->alloc_lock);
	free(rt);
}

int cxl_macro_register_entry(struct cxl_macro_runtime *rt, uint32_t entry_id,
			     cxl_entry_fn_t fn, const char *name)
{
	int i;

	if (!rt || !fn)
		return -EINVAL;

	pthread_mutex_lock(&rt->entry_lock);
	for (i = 0; i < rt->nr_entries; i++) {
		if (rt->entries[i].id == entry_id) {
			pthread_mutex_unlock(&rt->entry_lock);
			return -EEXIST;
		}
	}
	if (rt->nr_entries >= CXL_MAX_ENTRIES) {
		pthread_mutex_unlock(&rt->entry_lock);
		return -ENOSPC;
	}

	rt->entries[rt->nr_entries].id = entry_id;
	rt->entries[rt->nr_entries].fn = fn;
	if (name)
		snprintf(rt->entries[rt->nr_entries].name,
			 sizeof(rt->entries[rt->nr_entries].name), "%s", name);
	rt->nr_entries++;
	pthread_mutex_unlock(&rt->entry_lock);
	return 0;
}

void *cxl_macro_malloc(struct cxl_macro_runtime *rt, size_t size, int timeout_ms)
{
	struct cxl_alloc_req req;
	struct cxl_cmd cmd;
	struct cxl_barrier *b = NULL;
	uint64_t cmd_id;
	uint64_t va_off;
	struct cxl_local_alloc *alloc = NULL;
	int rc;

	if (!rt || !size)
		return NULL;

	memset(&req, 0, sizeof(req));
	req.size = size;
	if (ioctl(rt->fd, CXL_ALLOC, &req) < 0)
		return NULL;

	va_off = atomic_fetch_add(&rt->ctl->macros[rt->macro_id].va_next,
				  req.mapped_size);
	if (va_off + req.mapped_size > rt->window_size) {
		struct cxl_free_req free_req = { .alloc_id = req.alloc_id };

		ioctl(rt->fd, CXL_FREE, &free_req);
		errno = ENOSPC;
		return NULL;
	}

	cmd_id = atomic_fetch_add(&rt->ctl->global_cmd_id, 1);
	rc = cxl_barrier_init(rt, cmd_id, rt->membership_mask, &b);
	if (rc)
		goto rollback_free;

	memset(&cmd, 0, sizeof(cmd));
	cmd.type = CXL_CMD_MAP_REQ;
	cmd.cmd_id = cmd_id;
	cmd.macro_id = rt->macro_id;
	cmd.src_machine = rt->machine_id;
	cmd.alloc_id = req.alloc_id;
	cmd.va_offset = va_off;
	cmd.size = req.mapped_size;
	cmd.prot = PROT_READ | PROT_WRITE;

	rc = cxl_send_all(rt, &cmd, rt->membership_mask);
	if (rc)
		goto rollback_barrier;

	rc = cxl_barrier_wait(b, timeout_ms);
	if (rc)
		goto rollback_barrier;

	alloc = calloc(1, sizeof(*alloc));
	if (!alloc) {
		rc = -ENOMEM;
		goto rollback_barrier;
	}

	alloc->alloc_id = req.alloc_id;
	alloc->va_offset = va_off;
	alloc->size = req.mapped_size;
	alloc->addr = (void *)(uintptr_t)(rt->window_base + va_off);
	pthread_mutex_lock(&rt->alloc_lock);
	cxl_add_local_alloc(rt, alloc);
	pthread_mutex_unlock(&rt->alloc_lock);

	cxl_maprec_add(rt, req.alloc_id, va_off, req.mapped_size, PROT_READ | PROT_WRITE);
	cxl_barrier_fini(b);
	return alloc->addr;

rollback_barrier: {
		struct cxl_cmd unmap_cmd;
		struct cxl_barrier *rb = NULL;
		struct cxl_free_req free_req = { .alloc_id = req.alloc_id };
		uint64_t rollback_id = atomic_fetch_add(&rt->ctl->global_cmd_id, 1);

		if (!cxl_barrier_init(rt, rollback_id, rt->membership_mask, &rb)) {
			memset(&unmap_cmd, 0, sizeof(unmap_cmd));
			unmap_cmd.type = CXL_CMD_UNMAP_REQ;
			unmap_cmd.cmd_id = rollback_id;
			unmap_cmd.macro_id = rt->macro_id;
			unmap_cmd.src_machine = rt->machine_id;
			unmap_cmd.alloc_id = req.alloc_id;
			unmap_cmd.va_offset = va_off;
			unmap_cmd.size = req.mapped_size;
			cxl_send_all(rt, &unmap_cmd, rt->membership_mask);
			cxl_barrier_wait(rb, 1000);
			cxl_barrier_fini(rb);
		}
		cxl_barrier_fini(b);
		ioctl(rt->fd, CXL_FREE, &free_req);
		errno = -rc;
		return NULL;
	}

rollback_free: {
		struct cxl_free_req free_req = { .alloc_id = req.alloc_id };

		ioctl(rt->fd, CXL_FREE, &free_req);
		errno = -rc;
		return NULL;
	}
}

int cxl_macro_free(struct cxl_macro_runtime *rt, void *ptr, int timeout_ms)
{
	struct cxl_local_alloc *alloc;
	struct cxl_cmd cmd;
	struct cxl_barrier *b;
	struct cxl_free_req free_req;
	uint64_t cmd_id;
	int rc;

	if (!rt || !ptr)
		return -EINVAL;

	pthread_mutex_lock(&rt->alloc_lock);
	alloc = cxl_find_alloc(rt, ptr);
	if (!alloc) {
		pthread_mutex_unlock(&rt->alloc_lock);
		return -ENOENT;
	}
	cxl_remove_local_alloc(rt, alloc);
	pthread_mutex_unlock(&rt->alloc_lock);

	cmd_id = atomic_fetch_add(&rt->ctl->global_cmd_id, 1);
	rc = cxl_barrier_init(rt, cmd_id, rt->membership_mask, &b);
	if (rc)
		goto restore_alloc;

	memset(&cmd, 0, sizeof(cmd));
	cmd.type = CXL_CMD_UNMAP_REQ;
	cmd.cmd_id = cmd_id;
	cmd.macro_id = rt->macro_id;
	cmd.src_machine = rt->machine_id;
	cmd.alloc_id = alloc->alloc_id;
	cmd.va_offset = alloc->va_offset;
	cmd.size = alloc->size;

	rc = cxl_send_all(rt, &cmd, rt->membership_mask);
	if (rc) {
		cxl_barrier_fini(b);
		goto restore_alloc;
	}

	rc = cxl_barrier_wait(b, timeout_ms);
	cxl_barrier_fini(b);
	if (rc)
		goto restore_alloc;

	free_req.alloc_id = alloc->alloc_id;
	if (ioctl(rt->fd, CXL_FREE, &free_req) < 0) {
		rc = -errno;
		goto restore_alloc;
	}

	cxl_maprec_del(rt, alloc->alloc_id);
	free(alloc);
	return 0;

restore_alloc:
	pthread_mutex_lock(&rt->alloc_lock);
	cxl_add_local_alloc(rt, alloc);
	pthread_mutex_unlock(&rt->alloc_lock);
	return rc;
}

int cxl_spawn_remote(struct cxl_macro_runtime *rt, int target_machine,
		     uint32_t entry_id, uint64_t arg_u64,
		     uint64_t *thread_id_out, int timeout_ms)
{
	struct cxl_cmd cmd;
	struct cxl_barrier *b;
	uint64_t cmd_id;
	uint64_t mask;
	int rc;

	if (!rt || target_machine < 0 || target_machine >= rt->nr_machines)
		return -EINVAL;

	mask = 1ULL << target_machine;
	cmd_id = atomic_fetch_add(&rt->ctl->global_cmd_id, 1);
	rc = cxl_barrier_init(rt, cmd_id, mask, &b);
	if (rc)
		return rc;

	memset(&cmd, 0, sizeof(cmd));
	cmd.type = CXL_CMD_SPAWN_REQ;
	cmd.cmd_id = cmd_id;
	cmd.macro_id = rt->macro_id;
	cmd.src_machine = rt->machine_id;
	cmd.dst_machine = target_machine;
	cmd.entry_id = entry_id;
	cmd.arg_u64 = arg_u64;
	cmd.thread_id = cmd_id;

	rc = cxl_queue_push(&rt->ctl->queues[target_machine], &cmd);
	if (rc) {
		cxl_barrier_fini(b);
		return rc;
	}

	rc = cxl_barrier_wait(b, timeout_ms);
	cxl_barrier_fini(b);
	if (rc)
		return rc;

	if (thread_id_out)
		*thread_id_out = cmd_id;
	return 0;
}

int cxl_macro_set_ready(struct cxl_macro_runtime *rt)
{
	if (!rt)
		return -EINVAL;

	atomic_fetch_or(&rt->ctl->ready_mask, rt->machine_bit);
	return 0;
}

uint64_t cxl_macro_get_ready_mask(struct cxl_macro_runtime *rt)
{
	if (!rt)
		return 0;
	return atomic_load(&rt->ctl->ready_mask);
}

int cxl_macro_wait_ready(struct cxl_macro_runtime *rt, uint64_t target_mask,
			 int timeout_ms)
{
	uint64_t start_ns = cxl_now_ns();
	uint64_t timeout_ns;

	if (!rt)
		return -EINVAL;

	timeout_ns = (timeout_ms < 0) ? UINT64_MAX :
				      (uint64_t)timeout_ms * 1000000ULL;
	while ((atomic_load(&rt->ctl->ready_mask) & target_mask) != target_mask) {
		if (timeout_ns != UINT64_MAX &&
		    cxl_now_ns() - start_ns > timeout_ns)
			return -ETIMEDOUT;
		usleep(1000);
	}
	return 0;
}

int cxl_macro_request_stop(struct cxl_macro_runtime *rt)
{
	int i;

	if (!rt)
		return -EINVAL;
	atomic_store(&rt->ctl->stop_flag, 1);
	for (i = 0; i < rt->nr_machines; i++) {
		atomic_fetch_add(&rt->ctl->queues[i].doorbell, 1);
		cxl_futex_wake(&rt->ctl->queues[i].doorbell, 1);
	}
	return 0;
}

int cxl_macro_wait_stop(struct cxl_macro_runtime *rt, int timeout_ms)
{
	uint64_t start_ns = cxl_now_ns();
	uint64_t timeout_ns;

	if (!rt)
		return -EINVAL;
	timeout_ns = (timeout_ms < 0) ? UINT64_MAX :
				      (uint64_t)timeout_ms * 1000000ULL;
	while (!atomic_load(&rt->ctl->stop_flag)) {
		if (timeout_ns != UINT64_MAX &&
		    cxl_now_ns() - start_ns > timeout_ns)
			return -ETIMEDOUT;
		usleep(1000);
	}
	return 0;
}

struct cxl_control_plane *cxl_macro_ctl(struct cxl_macro_runtime *rt)
{
	if (!rt)
		return NULL;
	return rt->ctl;
}

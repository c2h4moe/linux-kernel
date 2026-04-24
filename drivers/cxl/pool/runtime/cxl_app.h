#ifndef CXL_APP_H
#define CXL_APP_H

#include <stddef.h>
#include <stdatomic.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct cxl_app;

#define CXL_MUTEX_MAGIC 0x43584C4DU /* CXLM */

typedef int (*cxl_app_main_fn_t)(struct cxl_app *app);
typedef int (*cxl_main_fn_t)(void);
typedef int (*cxl_worker_fn_t)(void *arg);

struct cxl_mutex {
	_Atomic uint32_t next_ticket;
	_Atomic uint32_t now_serving;
	_Atomic uint32_t magic;
	_Atomic uint32_t reserved;
};

struct cxl_app_cfg {
	int nr_machines;
	uint64_t window_base;
	uint64_t window_size;
	int timeout_ms;
};

int cxl_app_run(const struct cxl_app_cfg *cfg, cxl_app_main_fn_t app_main);
int cxl_run(const struct cxl_app_cfg *cfg, cxl_main_fn_t main_fn);

int cxl_worker_register_internal(const char *name, cxl_worker_fn_t worker,
				 void *(*entry_fn)(void *arg));
void *cxl_worker_invoke_internal(cxl_worker_fn_t worker, void *arg);

/* Register one worker so cxl_spawn() can refer to it by a stable worker ID. */
#define CXL_WORKER(name)                                                     \
	static void *__cxl_worker_entry_##name(void *arg)                    \
	{                                                                    \
		return cxl_worker_invoke_internal(name, arg);                \
	}                                                                    \
	static void __attribute__((constructor)) __cxl_worker_ctor_##name(void) \
	{                                                                    \
		(void)cxl_worker_register_internal(#name, name,             \
						 __cxl_worker_entry_##name); \
	}

int cxl_machine_id(struct cxl_app *app);
int cxl_machine_count(struct cxl_app *app);
int cxl_current_machine(void);
int cxl_current_machine_count(void);

int cxl_mutex_init(struct cxl_mutex *lock);
int cxl_mutex_destroy(struct cxl_mutex *lock);
struct cxl_mutex *cxl_mutex_create(void);
int cxl_mutex_free(struct cxl_mutex *lock);
int cxl_mutex_lock(struct cxl_mutex *lock);
int cxl_mutex_unlock(struct cxl_mutex *lock);
int cxl_mutex_trylock(struct cxl_mutex *lock);

void *cxl_malloc(struct cxl_app *app, size_t size);
int cxl_free(struct cxl_app *app, void *ptr);
void *cxl_malloc_current(size_t size);
int cxl_free_current(void *ptr);
int cxl_app_spawn(struct cxl_app *app, cxl_worker_fn_t worker,
		  int target_machine, void *shared_arg);
int cxl_spawn(cxl_worker_fn_t worker, int target_machine, void *shared_arg);

int cxl_app_store_u32(struct cxl_app *app, unsigned int slot, uint32_t value);
uint32_t cxl_app_load_u32(struct cxl_app *app, unsigned int slot);
int cxl_app_store_ptr(struct cxl_app *app, unsigned int slot, const void *ptr);
void *cxl_app_load_ptr(struct cxl_app *app, unsigned int slot);
int cxl_store_u32(unsigned int slot, uint32_t value);
uint32_t cxl_load_u32(unsigned int slot);
int cxl_store_ptr(unsigned int slot, const void *ptr);
void *cxl_load_ptr(unsigned int slot);

#ifdef __cplusplus
}
#endif

#endif

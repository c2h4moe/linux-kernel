#ifndef CXL_MACRO_RUNTIME_H
#define CXL_MACRO_RUNTIME_H

#include <pthread.h>
#include <stdint.h>

#include "cxl_macro_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void *(*cxl_entry_fn_t)(void *arg);

struct cxl_macro_cfg {
	int machine_id;
	int nr_machines;
	uint64_t window_base;
	uint64_t window_size;
	uint64_t control_alloc_id;
	uint64_t control_size;
};

struct cxl_macro_runtime;

int cxl_macro_control_create(int fd, int nr_machines, uint64_t window_base,
			     uint64_t window_size, uint64_t *alloc_id_out,
			     uint64_t *mapped_size_out,
			     struct cxl_control_plane **ctl_out);

int cxl_macro_init(struct cxl_macro_runtime **rt_out,
		   const struct cxl_macro_cfg *cfg);
void cxl_macro_destroy(struct cxl_macro_runtime *rt);

int cxl_macro_register_entry(struct cxl_macro_runtime *rt, uint32_t entry_id,
			     cxl_entry_fn_t fn, const char *name);

void *cxl_macro_malloc(struct cxl_macro_runtime *rt, size_t size,
		       int timeout_ms);
int cxl_macro_free(struct cxl_macro_runtime *rt, void *ptr, int timeout_ms);

int cxl_spawn_remote(struct cxl_macro_runtime *rt, int target_machine,
		     uint32_t entry_id, uint64_t arg_u64,
		     uint64_t *thread_id_out, int timeout_ms);

struct cxl_control_plane *cxl_macro_ctl(struct cxl_macro_runtime *rt);

#ifdef __cplusplus
}
#endif

#endif

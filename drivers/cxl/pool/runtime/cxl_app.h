#ifndef CXL_APP_H
#define CXL_APP_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct cxl_app;

typedef int (*cxl_app_main_fn_t)(struct cxl_app *app);

struct cxl_app_cfg {
	int nr_machines;
	uint64_t window_base;
	uint64_t window_size;
	int timeout_ms;
};

int cxl_app_run(const struct cxl_app_cfg *cfg, cxl_app_main_fn_t app_main);

int cxl_machine_id(struct cxl_app *app);
int cxl_machine_count(struct cxl_app *app);

void *cxl_malloc(struct cxl_app *app, size_t size);
int cxl_free(struct cxl_app *app, void *ptr);

int cxl_app_store_u32(struct cxl_app *app, unsigned int slot, uint32_t value);
uint32_t cxl_app_load_u32(struct cxl_app *app, unsigned int slot);
int cxl_app_store_ptr(struct cxl_app *app, unsigned int slot, const void *ptr);
void *cxl_app_load_ptr(struct cxl_app *app, unsigned int slot);

#ifdef __cplusplus
}
#endif

#endif

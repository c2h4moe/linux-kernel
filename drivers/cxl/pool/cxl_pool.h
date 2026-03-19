#ifndef _CXL_POOL_H
#define _CXL_POOL_H

#include <stdint.h>
#include <linux/ioctl.h>

#define CXL_POOL_MAGIC 'C'

#define CXL_ALLOC _IOWR(CXL_POOL_MAGIC, 1, struct cxl_alloc_req)
#define CXL_FREE _IOW(CXL_POOL_MAGIC, 2, struct cxl_free_req)
#define CXL_GET_INFO _IOR(CXL_POOL_MAGIC, 3, struct cxl_pool_info)

struct cxl_alloc_req {
	uint64_t size; /* Size to allocate in bytes */
	uint64_t alloc_id; /* OUT: allocation handle */
	uint64_t mapped_size; /* OUT: rounded mapping size in bytes */
};

struct cxl_free_req {
	uint64_t alloc_id; /* Allocation handle to free */
};

struct cxl_pool_info {
	uint64_t total_pages;
	uint64_t free_pages;
	uint64_t page_size;
	uint64_t huge_page_size;
	uint64_t base_phys;
	uint64_t total_size;
};

#endif /* _CXL_POOL_H */

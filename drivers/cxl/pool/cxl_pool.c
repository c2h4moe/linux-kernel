// SPDX-License-Identifier: GPL-2.0
/*
 * CXL Memory Pool Allocator
 *
 * This module provides:
 * 1. Global 4MB page allocation from a shared CXL region
 * 2. Handle-based allocations (alloc_id)
 * 3. mmap(/dev/cxl_pool) to map non-contiguous physical pages into
 *    contiguous user virtual memory
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/mm.h>
#include <linux/io.h>
#include <linux/idr.h>
#include <linux/mutex.h>
#include <linux/xarray.h>

#define CXL_POOL_MAGIC_MEM 0x43415800 /* "CAX\0" */
#define CXL_POOL_META_VERSION 2
#define CXL_PAGE_SIZE 4096
#define CXL_HUGE_PAGE_SIZE (4 * 1024 * 1024)
#define CXL_INVALID_PAGE_IDX U32_MAX
#define DEV_NAME "cxl_pool"
#define CLASS_NAME "cxl_pool"

/* IOCTL commands for CXL pool */
#define CXL_POOL_MAGIC 'C'
#define CXL_ALLOC _IOWR(CXL_POOL_MAGIC, 1, struct cxl_alloc_req)
#define CXL_FREE _IOW(CXL_POOL_MAGIC, 2, struct cxl_free_req)
#define CXL_GET_INFO _IOR(CXL_POOL_MAGIC, 3, struct cxl_pool_info)

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Research");
MODULE_DESCRIPTION("CXL Memory Pool Allocator");
MODULE_VERSION("0.2");

/* Module parameter: machine ID (0 = initialize pool, >0 = join pool) */
static int machine_id = 0;
module_param(machine_id, int, 0644);
MODULE_PARM_DESC(machine_id, "Machine ID (0 = initialize pool, other = join)");

/*
 * Metadata structure stored in first 4KB of the CXL memory.
 *
 * head_tagged layout:
 *   bits [31:0]  -> head page index
 *   bits [63:32] -> version tag (ABA guard)
 */
struct cxl_pool_metadata {
	__u64 magic;
	__u64 version;
	__u64 total_pages_4mb;
	__u64 free_pages_4mb;
	__u64 head_tagged;
	__u64 cxl_base_phys;
	__u64 total_size;
	__u64 page_size;
	__u8 reserved[4096 - 64];
};

/* Header at offset 0 of each free/allocated 4MB page */
struct cxl_page_header {
	__u32 next_idx;
	__u32 reserved;
};

struct cxl_alloc_req {
	__u64 size;
	__u64 alloc_id;
	__u64 mapped_size;
};

struct cxl_free_req {
	__u64 alloc_id;
};

struct cxl_pool_info {
	__u64 total_pages;
	__u64 free_pages;
	__u64 page_size;
	__u64 huge_page_size;
	__u64 base_phys;
	__u64 total_size;
};

struct cxl_allocation {
	__u64 alloc_id;
	__u64 mapped_size;
	__u32 num_pages;
	__u32 *page_idx;
	atomic_t map_count;
};

/* Device structure */
struct cxl_pool_dev {
	dev_t devno;
	struct cdev cdev;
	struct class *class;
	struct device *device;

	/* CXL memory info */
	void *kaddr;
	__u64 base_phys;
	__u64 total_size;
	long nr_pages;
	bool used_ioremap;

	/* Pool info in shared CXL memory */
	struct cxl_pool_metadata *meta;
	int initialized;

	/* Allocation registry (kernel local) */
	struct mutex alloc_lock;
	struct xarray allocations;
	struct ida alloc_ids;
};

static struct cxl_pool_dev *pool_dev;

static inline __u64 cxl_head_pack(__u32 tag, __u32 idx)
{
	return ((__u64)tag << 32) | idx;
}

static inline __u32 cxl_head_tag(__u64 head_tagged)
{
	return (__u32)(head_tagged >> 32);
}

static inline __u32 cxl_head_idx(__u64 head_tagged)
{
	return (__u32)head_tagged;
}

static bool cxl_idx_valid(struct cxl_pool_dev *dev, __u32 idx)
{
	return idx < dev->meta->total_pages_4mb;
}

static __u64 cxl_phys_from_idx(struct cxl_pool_dev *dev, __u32 idx)
{
	return dev->base_phys + CXL_HUGE_PAGE_SIZE +
	       ((__u64)idx * CXL_HUGE_PAGE_SIZE);
}

static struct cxl_page_header *cxl_page_hdr_from_idx(struct cxl_pool_dev *dev,
						      __u32 idx)
{
	return (struct cxl_page_header *)((char *)dev->kaddr +
					  CXL_HUGE_PAGE_SIZE +
					  ((__u64)idx * CXL_HUGE_PAGE_SIZE));
}

static bool cxl_change_free_pages(struct cxl_pool_metadata *meta, s64 delta)
{
	__u64 old, new;

	do {
		old = READ_ONCE(meta->free_pages_4mb);
		if (delta < 0 && old < (__u64)(-delta))
			return false;
		new = old + delta;
	} while (cmpxchg64(&meta->free_pages_4mb, old, new) != old);

	return true;
}

static int cxl_push_page_idx(struct cxl_pool_dev *dev, __u32 idx)
{
	struct cxl_pool_metadata *meta = dev->meta;
	struct cxl_page_header *hdr;
	__u64 old_head, new_head;
	__u32 old_tag, old_idx;

	if (!cxl_idx_valid(dev, idx))
		return -EINVAL;

	hdr = cxl_page_hdr_from_idx(dev, idx);
	do {
		old_head = READ_ONCE(meta->head_tagged);
		old_tag = cxl_head_tag(old_head);
		old_idx = cxl_head_idx(old_head);

		WRITE_ONCE(hdr->next_idx, old_idx);
		smp_wmb();
		new_head = cxl_head_pack(old_tag + 1, idx);
	} while (cmpxchg64(&meta->head_tagged, old_head, new_head) != old_head);

	if (!cxl_change_free_pages(meta, 1))
		return -EIO;

	return 0;
}

static int cxl_pop_page_idx(struct cxl_pool_dev *dev, __u32 *idx_out)
{
	struct cxl_pool_metadata *meta = dev->meta;
	struct cxl_page_header *hdr;
	__u64 old_head, new_head;
	__u32 old_tag, idx, next_idx;
	int ret;

	for (;;) {
		old_head = READ_ONCE(meta->head_tagged);
		old_tag = cxl_head_tag(old_head);
		idx = cxl_head_idx(old_head);

		if (idx == CXL_INVALID_PAGE_IDX)
			return -ENOMEM;

		if (!cxl_idx_valid(dev, idx))
			return -EIO;

		hdr = cxl_page_hdr_from_idx(dev, idx);
		next_idx = READ_ONCE(hdr->next_idx);
		new_head = cxl_head_pack(old_tag + 1, next_idx);

		if (cmpxchg64(&meta->head_tagged, old_head, new_head) == old_head)
			break;
		cpu_relax();
	}

	WRITE_ONCE(hdr->next_idx, CXL_INVALID_PAGE_IDX);
	if (!cxl_change_free_pages(meta, -1)) {
		ret = cxl_push_page_idx(dev, idx);
		if (ret)
			return ret;
		return -EIO;
	}

	*idx_out = idx;
	return 0;
}

static int cxl_alloc_page_indices(struct cxl_pool_dev *dev, __u32 num_pages,
				  __u32 *idxv)
{
	__u32 i;
	int ret;

	for (i = 0; i < num_pages; i++) {
		ret = cxl_pop_page_idx(dev, &idxv[i]);
		if (ret)
			goto rollback;
	}

	return 0;

rollback:
	while (i > 0) {
		i--;
		cxl_push_page_idx(dev, idxv[i]);
	}
	return ret;
}

static void cxl_free_allocation_pages(struct cxl_pool_dev *dev,
				      struct cxl_allocation *alloc)
{
	__u32 i;

	for (i = 0; i < alloc->num_pages; i++)
		cxl_push_page_idx(dev, alloc->page_idx[i]);
}

static void cxl_allocation_put(struct cxl_allocation *alloc)
{
	kfree(alloc->page_idx);
	kfree(alloc);
}

static void cxl_vma_open(struct vm_area_struct *vma)
{
	struct cxl_allocation *alloc = vma->vm_private_data;

	if (alloc)
		atomic_inc(&alloc->map_count);
}

static void cxl_vma_close(struct vm_area_struct *vma)
{
	struct cxl_allocation *alloc = vma->vm_private_data;

	if (alloc)
		atomic_dec(&alloc->map_count);
}

static const struct vm_operations_struct cxl_pool_vm_ops = {
	.open = cxl_vma_open,
	.close = cxl_vma_close,
};

static int cxl_pool_open(struct inode *inode, struct file *filp)
{
	filp->private_data = pool_dev;
	return 0;
}

static int cxl_pool_release(struct inode *inode, struct file *filp)
{
	return 0;
}

static ssize_t cxl_pool_read(struct file *filp, char __user *buf, size_t count,
			     loff_t *ppos)
{
	struct cxl_pool_dev *dev = filp->private_data;
	char info[256];
	int len;

	if (*ppos > 0)
		return 0;

	len = snprintf(info, sizeof(info),
		       "machine_id=%d\n"
		       "initialized=%d\n"
		       "total_size=%llu\n"
		       "total_4mb_pages=%llu\n"
		       "free_4mb_pages=%llu\n"
		       "head_idx=%u\n"
		       "base_phys=0x%llx\n",
		       machine_id, dev->initialized, dev->meta->total_size,
		       dev->meta->total_pages_4mb, dev->meta->free_pages_4mb,
		       cxl_head_idx(dev->meta->head_tagged),
		       dev->meta->cxl_base_phys);

	if (count < len)
		return -EINVAL;

	if (copy_to_user(buf, info, len))
		return -EFAULT;

	*ppos = len;
	return len;
}

static int cxl_pool_mmap(struct file *filp, struct vm_area_struct *vma)
{
	struct cxl_pool_dev *dev = filp->private_data;
	struct cxl_allocation *alloc;
	__u64 alloc_id = vma->vm_pgoff;
	unsigned long uaddr = vma->vm_start;
	__u64 len = vma->vm_end - vma->vm_start;
	__u64 remain;
	__u32 i;
	int ret = 0;

	if (!dev || !dev->initialized || !dev->kaddr)
		return -ENODEV;

	if (!(vma->vm_flags & VM_SHARED))
		return -EINVAL;
	if (!len || (len & (PAGE_SIZE - 1)))
		return -EINVAL;

	mutex_lock(&dev->alloc_lock);
	alloc = xa_load(&dev->allocations, alloc_id);
	if (!alloc) {
		ret = -ENOENT;
		goto out_unlock;
	}

	if (len != alloc->mapped_size) {
		ret = -EINVAL;
		goto out_unlock;
	}

	vm_flags_set(vma, VM_DONTEXPAND | VM_DONTDUMP);
	remain = len;
	for (i = 0; i < alloc->num_pages && remain; i++) {
		__u64 chunk = min_t(__u64, remain, CXL_HUGE_PAGE_SIZE);
		__u64 phys = cxl_phys_from_idx(dev, alloc->page_idx[i]);

		ret = remap_pfn_range(vma, uaddr, phys >> PAGE_SHIFT, chunk,
				      vma->vm_page_prot);
		if (ret)
			goto out_unlock;

		uaddr += chunk;
		remain -= chunk;
	}

	if (remain) {
		ret = -EINVAL;
		goto out_unlock;
	}

	vma->vm_private_data = alloc;
	vma->vm_ops = &cxl_pool_vm_ops;
	cxl_vma_open(vma);

out_unlock:
	mutex_unlock(&dev->alloc_lock);
	return ret;
}

static long cxl_pool_ioctl(struct file *filp, unsigned int cmd,
			   unsigned long arg)
{
	struct cxl_pool_dev *dev = filp->private_data;
	struct cxl_alloc_req alloc_req;
	struct cxl_free_req free_req;
	struct cxl_pool_info info_req;
	struct cxl_allocation *alloc;
	void __user *argp = (void __user *)arg;
	__u64 size;
	__u32 num_pages;
	int ret = 0;

	if (!dev || !dev->initialized || !dev->kaddr)
		return -ENODEV;

	switch (cmd) {
	case CXL_ALLOC: {
		int id;

		if (copy_from_user(&alloc_req, argp, sizeof(alloc_req)))
			return -EFAULT;

		size = alloc_req.size;
		if (!size)
			return -EINVAL;

		num_pages =
			DIV_ROUND_UP_ULL(size, (__u64)CXL_HUGE_PAGE_SIZE);

		alloc = kzalloc(sizeof(*alloc), GFP_KERNEL);
		if (!alloc)
			return -ENOMEM;

		alloc->page_idx = kcalloc(num_pages, sizeof(*alloc->page_idx),
					  GFP_KERNEL);
		if (!alloc->page_idx) {
			kfree(alloc);
			return -ENOMEM;
		}

		ret = cxl_alloc_page_indices(dev, num_pages, alloc->page_idx);
		if (ret) {
			cxl_allocation_put(alloc);
			return ret;
		}

		alloc->num_pages = num_pages;
		alloc->mapped_size = (__u64)num_pages * CXL_HUGE_PAGE_SIZE;
		atomic_set(&alloc->map_count, 0);

		mutex_lock(&dev->alloc_lock);
		id = ida_alloc_min(&dev->alloc_ids, 1, GFP_KERNEL);
		if (id < 0) {
			mutex_unlock(&dev->alloc_lock);
			cxl_free_allocation_pages(dev, alloc);
			cxl_allocation_put(alloc);
			return id;
		}
		alloc->alloc_id = id;

		ret = xa_err(xa_store(&dev->allocations, id, alloc, GFP_KERNEL));
		if (ret) {
			ida_free(&dev->alloc_ids, id);
			mutex_unlock(&dev->alloc_lock);
			cxl_free_allocation_pages(dev, alloc);
			cxl_allocation_put(alloc);
			return ret;
		}
		mutex_unlock(&dev->alloc_lock);

		alloc_req.alloc_id = alloc->alloc_id;
		alloc_req.mapped_size = alloc->mapped_size;

		if (copy_to_user(argp, &alloc_req, sizeof(alloc_req)))
			return -EFAULT;

		return 0;
	}

	case CXL_FREE:
		if (copy_from_user(&free_req, argp, sizeof(free_req)))
			return -EFAULT;

		mutex_lock(&dev->alloc_lock);
		alloc = xa_load(&dev->allocations, free_req.alloc_id);
		if (!alloc) {
			mutex_unlock(&dev->alloc_lock);
			return -ENOENT;
		}
		if (atomic_read(&alloc->map_count) > 0) {
			mutex_unlock(&dev->alloc_lock);
			return -EBUSY;
		}

		alloc = xa_erase(&dev->allocations, free_req.alloc_id);
		ida_free(&dev->alloc_ids, free_req.alloc_id);
		mutex_unlock(&dev->alloc_lock);

		cxl_free_allocation_pages(dev, alloc);
		cxl_allocation_put(alloc);
		return 0;

	case CXL_GET_INFO:
		info_req.total_pages = dev->meta->total_pages_4mb;
		info_req.free_pages = dev->meta->free_pages_4mb;
		info_req.page_size = CXL_PAGE_SIZE;
		info_req.huge_page_size = CXL_HUGE_PAGE_SIZE;
		info_req.base_phys = dev->base_phys;
		info_req.total_size = dev->total_size;

		if (copy_to_user(argp, &info_req, sizeof(info_req)))
			return -EFAULT;

		return 0;

	default:
		return -ENOTTY;
	}
}

static const struct file_operations cxl_pool_fops = {
	.owner = THIS_MODULE,
	.open = cxl_pool_open,
	.release = cxl_pool_release,
	.read = cxl_pool_read,
	.mmap = cxl_pool_mmap,
	.unlocked_ioctl = cxl_pool_ioctl,
	.compat_ioctl = compat_ptr_ioctl,
};

static int cxl_init_free_list(struct cxl_pool_dev *dev)
{
	__u64 total_4mb_pages, i;
	struct cxl_page_header *hdr;

	if (dev->total_size <= CXL_HUGE_PAGE_SIZE)
		return -ENOMEM;

	total_4mb_pages =
		(dev->total_size - CXL_HUGE_PAGE_SIZE) / CXL_HUGE_PAGE_SIZE;
	if (!total_4mb_pages)
		return -ENOMEM;
	if (total_4mb_pages > U32_MAX)
		return -E2BIG;

	dev->meta->magic = CXL_POOL_MAGIC_MEM;
	dev->meta->version = CXL_POOL_META_VERSION;
	dev->meta->total_pages_4mb = total_4mb_pages;
	dev->meta->free_pages_4mb = total_4mb_pages;
	dev->meta->cxl_base_phys = dev->base_phys;
	dev->meta->total_size = dev->total_size;
	dev->meta->page_size = CXL_PAGE_SIZE;
	dev->meta->head_tagged = cxl_head_pack(0, 0);

	for (i = 0; i < total_4mb_pages; i++) {
		__u32 next_idx = (i + 1 < total_4mb_pages) ?
				     (__u32)(i + 1) :
				     CXL_INVALID_PAGE_IDX;
		hdr = cxl_page_hdr_from_idx(dev, i);
		WRITE_ONCE(hdr->next_idx, next_idx);
	}

	return 0;
}

static int init_cxl_pool(struct cxl_pool_dev *dev)
{
	struct file *filp;
	phys_addr_t phys_addr;
	u64 mem_size;
	char buf[64];
	loff_t pos;
	ssize_t bytes_read;
	unsigned long long tmp;
	int ret;

	pr_info("cxl_pool: Using memremap approach\n");

	filp = filp_open("/sys/bus/dax/devices/dax0.0/resource", O_RDONLY, 0);
	if (IS_ERR(filp))
		filp = filp_open("/sys/class/dax/dax0.0/device/resource",
				 O_RDONLY, 0);
	if (IS_ERR(filp)) {
		pr_err("cxl_pool: Failed to open sysfs resource: %ld\n",
		       PTR_ERR(filp));
		return PTR_ERR(filp);
	}

	pos = 0;
	bytes_read = kernel_read(filp, buf, sizeof(buf) - 1, &pos);
	filp_close(filp, NULL);
	if (bytes_read <= 0) {
		pr_err("cxl_pool: Failed to read resource: %zd\n", bytes_read);
		return -EIO;
	}

	buf[bytes_read] = '\0';
	if (sscanf(buf, "%llx", &tmp) != 1 && sscanf(buf, "%llu", &tmp) != 1) {
		pr_err("cxl_pool: Failed to parse phys addr: %s\n", buf);
		return -EIO;
	}
	phys_addr = (phys_addr_t)tmp;

	filp = filp_open("/sys/bus/dax/devices/dax0.0/size", O_RDONLY, 0);
	if (IS_ERR(filp))
		filp = filp_open("/sys/class/dax/dax0.0/size", O_RDONLY, 0);
	if (IS_ERR(filp)) {
		mem_size = 526385152;
		phys_addr = 0x140000000ULL;
		goto got_size;
	}

	pos = 0;
	bytes_read = kernel_read(filp, buf, sizeof(buf) - 1, &pos);
	filp_close(filp, NULL);
	if (bytes_read <= 0) {
		mem_size = 526385152;
		phys_addr = 0x140000000ULL;
		goto got_size;
	}

	buf[bytes_read] = '\0';
	if (kstrtou64(buf, 0, &mem_size)) {
		mem_size = 526385152;
		phys_addr = 0x140000000ULL;
	}

got_size:
	dev->kaddr = memremap(phys_addr, mem_size, MEMREMAP_WB);
	if (!dev->kaddr) {
		dev->kaddr = ioremap(phys_addr, mem_size);
		if (!dev->kaddr) {
			dev->kaddr = ioremap_wc(phys_addr, mem_size);
			if (!dev->kaddr) {
				pr_err("cxl_pool: failed to map CXL memory\n");
				return -ENOMEM;
			}
		}
		dev->used_ioremap = true;
	}

	dev->base_phys = phys_addr;
	dev->total_size = mem_size;
	dev->nr_pages = mem_size / CXL_PAGE_SIZE;
	dev->meta = (struct cxl_pool_metadata *)dev->kaddr;

	pr_info("cxl_pool: Mapped CXL memory: phys=0x%llx size=%llu bytes\n",
		(u64)phys_addr, mem_size);

	if (machine_id == 0) {
		if (dev->meta->magic != CXL_POOL_MAGIC_MEM ||
		    dev->meta->version != CXL_POOL_META_VERSION) {
			pr_info("cxl_pool: Initializing CXL memory pool...\n");
			ret = cxl_init_free_list(dev);
			if (ret)
				return ret;
		} else {
			pr_info("cxl_pool: Pool already initialized, joining...\n");
		}
		dev->initialized = 1;
		return 0;
	}

	if (dev->meta->magic != CXL_POOL_MAGIC_MEM ||
	    dev->meta->version != CXL_POOL_META_VERSION) {
		pr_err("cxl_pool: Pool not initialized with v%u metadata.\n",
		       CXL_POOL_META_VERSION);
		if (dev->used_ioremap)
			iounmap(dev->kaddr);
		else
			memunmap(dev->kaddr);
		dev->kaddr = NULL;
		return -ENODEV;
	}

	pr_info("cxl_pool: Joining existing pool: total=%llu free=%llu\n",
		dev->meta->total_pages_4mb, dev->meta->free_pages_4mb);
	dev->initialized = 1;
	return 0;
}

static void cxl_cleanup_allocations(struct cxl_pool_dev *dev)
{
	unsigned long index;
	struct cxl_allocation *alloc;

	mutex_lock(&dev->alloc_lock);
	xa_for_each(&dev->allocations, index, alloc) {
		xa_erase(&dev->allocations, index);
		ida_free(&dev->alloc_ids, alloc->alloc_id);
		if (atomic_read(&alloc->map_count) == 0)
			cxl_free_allocation_pages(dev, alloc);
		cxl_allocation_put(alloc);
	}
	mutex_unlock(&dev->alloc_lock);
}

static int __init cxl_pool_init(void)
{
	int ret;

	pr_info("cxl_pool: Loading module, machine_id=%d\n", machine_id);

	pool_dev = kzalloc(sizeof(*pool_dev), GFP_KERNEL);
	if (!pool_dev)
		return -ENOMEM;

	mutex_init(&pool_dev->alloc_lock);
	xa_init(&pool_dev->allocations);
	ida_init(&pool_dev->alloc_ids);

	ret = init_cxl_pool(pool_dev);
	if (ret < 0)
		goto err_free_pool_dev;

	ret = alloc_chrdev_region(&pool_dev->devno, 0, 1, DEV_NAME);
	if (ret < 0)
		goto err_unmap;

	cdev_init(&pool_dev->cdev, &cxl_pool_fops);
	pool_dev->cdev.owner = THIS_MODULE;
	ret = cdev_add(&pool_dev->cdev, pool_dev->devno, 1);
	if (ret < 0)
		goto err_chrdev;

	pool_dev->class = class_create(CLASS_NAME);
	if (IS_ERR(pool_dev->class)) {
		ret = PTR_ERR(pool_dev->class);
		goto err_cdev;
	}

	pool_dev->device = device_create(pool_dev->class, NULL, pool_dev->devno,
					 NULL, DEV_NAME);
	if (IS_ERR(pool_dev->device)) {
		ret = PTR_ERR(pool_dev->device);
		goto err_class;
	}

	pr_info("cxl_pool: Module loaded successfully, /dev/%s created\n",
		DEV_NAME);
	return 0;

err_class:
	class_destroy(pool_dev->class);
err_cdev:
	cdev_del(&pool_dev->cdev);
err_chrdev:
	unregister_chrdev_region(pool_dev->devno, 1);
err_unmap:
	if (pool_dev->kaddr) {
		if (pool_dev->used_ioremap)
			iounmap(pool_dev->kaddr);
		else
			memunmap(pool_dev->kaddr);
	}
err_free_pool_dev:
	xa_destroy(&pool_dev->allocations);
	ida_destroy(&pool_dev->alloc_ids);
	kfree(pool_dev);
	pool_dev = NULL;
	return ret;
}

static void __exit cxl_pool_exit(void)
{
	pr_info("cxl_pool: Unloading module\n");

	if (!pool_dev)
		return;

	cxl_cleanup_allocations(pool_dev);

	device_destroy(pool_dev->class, pool_dev->devno);
	class_destroy(pool_dev->class);
	cdev_del(&pool_dev->cdev);
	unregister_chrdev_region(pool_dev->devno, 1);

	if (pool_dev->kaddr) {
		if (pool_dev->used_ioremap)
			iounmap(pool_dev->kaddr);
		else
			memunmap(pool_dev->kaddr);
	}

	xa_destroy(&pool_dev->allocations);
	ida_destroy(&pool_dev->alloc_ids);
	kfree(pool_dev);
	pool_dev = NULL;
}

module_init(cxl_pool_init);
module_exit(cxl_pool_exit);

#ifndef _LINUX_VC4MEM_H
#define _LINUX_VC4MEM_H

#include <linux/ioctl.h>

#define VC4MEM_MAX_NUM_REQS 16

struct vc4mem_alloc_mem {
	union {
		struct {
			unsigned n;
			unsigned size[VC4MEM_MAX_NUM_REQS];
		} user;
		struct {
			unsigned long dma[VC4MEM_MAX_NUM_REQS];
		} kern;
	};
};

struct vc4mem_free_mem {
	union {
		struct {
			unsigned n;
			unsigned long dma[VC4MEM_MAX_NUM_REQS];
			unsigned size[VC4MEM_MAX_NUM_REQS];
		} user;
	};
};

/*
 *   CPU-side cache manipulation
 *
 *   There are two operations needed to do when you use the same memory region
 * for CPU and GPU.
 *   One is invalidation.  This operation ensures that updates made by GPU are
 * visible to CPU by marking CPU cache contents as invalid.  So do this when GPU
 * has just written data to a memory region and CPU is going to read from it.
 *   The other is clean.  This operation ensures that updates made by CPU are
 * visible to GPU by writing CPU cache contents to memory.  So do this when CPU
 * issued store instructions and GPU is going to read from the region to which
 * CPU wrote.
 *   For example:
 *
 *     uint32_t *p_user, p_dma;
 *
 *     p_user = vc4mem_alloc_mem(SIZE, &p_dma);
 *     if (p_user == NULL) {
 *         error_and_exit();
 *     }
 *     initalize_on_cpu(p_user);
 *     vc4mem_cpu_cache_op_clean(p_dma, SIZE);
 *     process_on_gpu(p_dma, SIZE);
 *     vc4mem_cpu_cache_op_invalidate(p_dma, SIZE);
 *     read_on_cpu(p_user);
 */
typedef unsigned vc4mem_cpu_cache_op_t;
#define VC4MEM_CPU_CACHE_OP_INVALIDATE   0
#define VC4MEM_CPU_CACHE_OP_CLEAN        1

struct vc4mem_cpu_cache_op {
	union {
		struct {
			unsigned n;
			vc4mem_cpu_cache_op_t op[VC4MEM_MAX_NUM_REQS];
			unsigned long dma[VC4MEM_MAX_NUM_REQS];
			unsigned size[VC4MEM_MAX_NUM_REQS];
		} user;
	};
};

#define VC4MEM_IOC_MAGIC '4'
#define VC4MEM_IOC_ALLOC_MEM    _IOWR(VC4MEM_IOC_MAGIC, 0, \
		struct vc4mem_alloc_mem)
#define VC4MEM_IOC_FREE_MEM     _IOW (VC4MEM_IOC_MAGIC, 1, \
		struct vc4mem_free_mem)
#define VC4MEM_IOC_CPU_CACHE_OP _IOW (VC4MEM_IOC_MAGIC, 2, \
		struct vc4mem_cpu_cache_op)


#endif /* _LINUX_VC4MEM_H */

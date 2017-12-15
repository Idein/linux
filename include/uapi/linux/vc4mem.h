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

typedef unsigned vc4mem_cpu_cache_op_t;
#define VC4MEM_CPU_CACHE_OP_INVALIDATE   0
#define VC4MEM_CPU_CACHE_OP_CLEAN        1
#define VC4MEM_CPU_CACHE_OP_FLUSH        VC4MEM_CPU_CACHE_OP_CLEAN

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

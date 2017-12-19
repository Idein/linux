/*
 * Character device driver for manipulating VideoCore IV memory.
 *
 * Written by Sugizaki Yukimasa <ysugi@idein.jp>
 * Copyright (c) 2017 Idein Inc.
 *
 * This driver is based on rpi-4.9.y/drivers/char/broadcom/bcm2835_smi_dev.c,
 * written by Luke Wren <luke@raspberrypi.org>.
 * Copyright (c) 2015, Raspberry Pi (Trading) Ltd.
 *
 * The vc4mem_mmap() function is derived from drivers/char/mem.c.
 * Copyright (C) 1991, 1992  Linus Torvalds
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions, and the following disclaimer,
 *    without modification.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. The names of the above-listed copyright holders may not be used
 *    to endorse or promote products derived from this software without
 *    specific prior written permission.
 *
 * ALTERNATIVELY, this software may be distributed under the terms of the
 * GNU General Public License ("GPL") version 2, as published by the Free
 * Software Foundation.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS
 * IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <linux/kernel.h>
#include <linux/platform_device.h>
#include <linux/module.h>
#include <linux/cdev.h>
#include <linux/of.h>
#include <linux/uaccess.h>
#include <linux/memblock.h>
#include <linux/dma-mapping.h>
#include <linux/dma-contiguous.h>
#include <linux/cma.h>
#include <uapi/linux/vc4mem.h>

#define DEVICE_NAME "vc4mem"
#define DRIVER_NAME "vc4mem"
#define DEVICE_MINOR 0
#define MEM_ATTRS                                                      \
	(                                                              \
		/* VC4 requires memory to be contiguous. */            \
		  DMA_ATTR_FORCE_CONTIGUOUS                            \
		/* This driver doesn't access the allocated memory. */ \
		| DMA_ATTR_NO_KERNEL_MAPPING                           \
	)

static struct cdev vc4mem_cdev;
static dev_t vc4mem_devid;
static struct class *vc4mem_class;
static struct device *vc4mem_dev;

static struct {
	struct device *dev;
} inst;


/* File ops */

static int vc4mem_open(struct inode *inode, struct file *file)
{
	int dev = iminor(inode);

	dev_info(inst.dev, "vc4mem device opened\n");

	if (dev != DEVICE_MINOR) {
		dev_err(inst.dev, "%s: Unknown minor number: %d\n",
				__func__, dev);
		return -ENXIO;
	}

	return 0;
}

static int vc4mem_release(struct inode *inode, struct file *file)
{
	int dev = iminor(inode);

	dev_info(inst.dev, "vc4mem device closing\n");

	if (dev != DEVICE_MINOR) {
		dev_err(inst.dev, "%s: Unknown minor number: %d\n",
				__func__, dev);
		return -ENXIO;
	}

	return 0;
}


/* ioctl calls */

static dma_addr_t alloc_mem(const unsigned size)
{
	const unsigned long order = get_order(size);
	const size_t count = size >> PAGE_SHIFT;
	struct page *page;
	dma_addr_t dma;

	page = dma_alloc_from_contiguous(inst.dev, count, order);
	if (page == NULL) {
		dev_err(inst.dev, "%s: Failed to allocate memory from CMA\n",
				__func__);
		return 0;
	}
	/* xxx: Do we need to see DMA_ATTR_SKIP_CPU_SYNC attr of the page and do
	 *      dmac_flush_range() here?
	 */

	dma = pfn_to_dma(inst.dev, page_to_pfn(page));

	dev_info(inst.dev, "%s: Allocated addr=0x%08x size=0x%08x page=0x%p\n",
			__func__, (unsigned) dma, size, page);

	return dma;
}

static int free_mem(const dma_addr_t dma, const unsigned size)
{
	struct page *page = pfn_to_page(dma_to_pfn(inst.dev, dma));
	const size_t count = size >> PAGE_SHIFT;

	dev_info(inst.dev, "%s: Freeing addr=0x%08x size=0x%08x page=0x%p\n",
			__func__, (unsigned) dma, size, page);

	if (!dma_release_from_contiguous(inst.dev, page, count)) {
		dev_err(inst.dev, "%s: Failed to free memory\n", __func__);
		return 1;
	}

	return 0;
}

static int sync_cache_cpu(const vc4mem_cpu_cache_op_t op,
		const dma_addr_t dma, const unsigned size)
{
	void (*sync_func)(struct device *dev, dma_addr_t dma, size_t size,
			enum dma_data_direction dir);
	enum dma_data_direction dir;

	/*
	 * - dma_sync_single_for_cpu:
	 *     - dir=from_dev: Invalidate cache and mark page clean
	 * - dma_sync_single_for_device:
	 *     - dir=from_dev: Invalidate cache
	 *     - dir=to_dev:   Clean cache
	 *
	 * Note: Marking a page clean avoids extra flushing when the page is
	 *       re-allocated without DMA_ATTR_SKIP_CPU_SYNC.
	 */

	switch (op) {
	case VC4MEM_CPU_CACHE_OP_INVALIDATE:
		sync_func = dma_sync_single_for_device;
		dir = DMA_FROM_DEVICE;
		break;
	case VC4MEM_CPU_CACHE_OP_CLEAN:
		sync_func = dma_sync_single_for_device;
		dir = DMA_TO_DEVICE;
		break;
	default:
		dev_err(inst.dev, "%s: Invalid cache op: %d\n",
				__func__, op);
		return -EINVAL;
	}

	dev_info(inst.dev, "%s: Syncing addr=0x%08x size=0x%08x dir=%d\n",
			__func__, (unsigned) dma, size, dir);

	sync_func(inst.dev, dma, size, dir);

	return 0;
}


/* ioctl handlers */

static int ioctl_alloc_mem(unsigned long arg)
{
	unsigned i, n;
	struct vc4mem_alloc_mem ioparam;
	typeof(*ioparam.user.size) size[VC4MEM_MAX_NUM_REQS];

	if (copy_from_user(&ioparam, (void*) arg, sizeof(ioparam))) {
		dev_err(inst.dev, "%s: Failed to copy_from_user\n", __func__);
		return -EFAULT;
	}

	n = ioparam.user.n;
	(void) memcpy(size, ioparam.user.size, sizeof(*size) * n);

	for (i = 0; i < n; i++) {
		dma_addr_t dma;
		dma = alloc_mem(size[i]);
		if (dma == 0) {
			unsigned j;
			dev_err(inst.dev,
					"%s: Failed to allocate memory at %d\n",
					__func__, i);
			for (j = 0; j < i; j ++)
				(void) free_mem(ioparam.kern.dma[j], size[j]);
			return -ENOMEM;
		}
		ioparam.kern.dma[i] = dma;
	}

	if (copy_to_user((void*) arg, &ioparam, sizeof(ioparam))) {
		dev_err(inst.dev, "%s: Failed to copy_to_user\n", __func__);
		for (i = 0; i < n; i++)
			(void) free_mem(ioparam.kern.dma[i], size[i]);
		return -EFAULT;
	}

	return 0;
}

static int ioctl_free_mem(unsigned long arg)
{
	int ret = 0;
	unsigned i;
	struct vc4mem_free_mem ioparam;

	if (copy_from_user(&ioparam, (void*) arg, sizeof(ioparam))) {
		dev_err(inst.dev, "%s: Failed to copy_from_user\n", __func__);
		return -EFAULT;
	}

	for (i = 0; i < ioparam.user.n; i++)
		if (free_mem(ioparam.user.dma[i], ioparam.user.size[i]))
			ret = -EAGAIN;

	return ret;
}

static int ioctl_cpu_cache_op(const unsigned long arg)
{
	int ret;
	unsigned i;
	struct vc4mem_cpu_cache_op ioparam;

	if (copy_from_user(&ioparam, (void*) arg, sizeof(ioparam))) {
		dev_err(inst.dev, "%s: Failed to copy_from_user\n", __func__);
		return -EFAULT;
	}

	for (i = 0; i < ioparam.user.n; i++) {
		ret = sync_cache_cpu(ioparam.user.op[i], ioparam.user.dma[i],
				ioparam.user.size[i]);
		if (ret)
			return ret;
	}

	return 0;
}

static long vc4mem_ioctl(struct file *file, unsigned cmd, unsigned long arg)
{
	dev_info(inst.dev, "%s: ioctl received: cmd=0x%08x\n", __func__, cmd);

	switch (cmd) {
	case VC4MEM_IOC_ALLOC_MEM:
		return ioctl_alloc_mem(arg);

	case VC4MEM_IOC_FREE_MEM:
		return ioctl_free_mem(arg);

	case VC4MEM_IOC_CPU_CACHE_OP:
		return ioctl_cpu_cache_op(arg);

	default:
		dev_err(inst.dev, "%s: Invalid ioctl cmd: 0x%08x\n",
				__func__, cmd);
		return -ENOTTY;
	}

	unreachable();
}

static int vc4mem_mmap(struct file *file, struct vm_area_struct *vma)
{
	const size_t size = vma->vm_end - vma->vm_start;

	if (size <= 0) {
		dev_err(inst.dev, "%s: Invalid size=%zd\n", __func__, size);
		return -EINVAL;
	}
	if (!valid_mmap_phys_addr_range(vma->vm_pgoff, size)) {
		dev_err(inst.dev, "%s: Invalid phys range\n", __func__);
		return -EINVAL;
	}
	if (!pfn_valid(vma->vm_pgoff)) {
		dev_err(inst.dev, "%s: Only memory regions I served is "
				"mmap'able here\n", __func__);
		return -EINVAL;
	}

	/*
	 * If the device is opened with O_SYNC, the prot will be writecombine,
	 * else writeback, which is faster.
	 */
	vma->vm_page_prot = phys_mem_access_prot(file, vma->vm_pgoff, size,
			vma->vm_page_prot);

	if (remap_pfn_range(vma, vma->vm_start, vma->vm_pgoff, size,
			vma->vm_page_prot)) {
		return -EAGAIN;
	}

	return 0;
}


static const struct file_operations vc4mem_fops = {
	.owner = THIS_MODULE,
	.open = vc4mem_open,
	.release = vc4mem_release,
	.unlocked_ioctl = vc4mem_ioctl,
	.mmap = vc4mem_mmap,
};

static int vc4mem_dev_probe(struct platform_device *pdev)
{
	int err;
	struct device *dev = &pdev->dev;

	inst.dev = dev;


	/* Create character device entry. */
	err = alloc_chrdev_region(&vc4mem_devid, DEVICE_MINOR, 1, DEVICE_NAME);
	if (err) {
		dev_err(dev, "%s: Failed to create cdev entry\n", __func__);
		goto failed_cdev_create;
	}
	cdev_init(&vc4mem_cdev, &vc4mem_fops);
	vc4mem_cdev.owner = THIS_MODULE;
	err = cdev_add(&vc4mem_cdev, vc4mem_devid, 1);
	if (err) {
		dev_err(dev, "%s: Failed to add cdev entry\n", __func__);
		goto failed_cdev_add;
	}

	/* Create sysfs entry. */
	vc4mem_class = class_create(THIS_MODULE, DEVICE_NAME);
	if (IS_ERR(vc4mem_class)) {
		dev_err(dev, "%s: Failed to create sysfs class\n", __func__);
		err = PTR_ERR(vc4mem_class);
		goto failed_class_create;
	}

	/* Create device. */
	vc4mem_dev = device_create(vc4mem_class, NULL, vc4mem_devid, NULL,
			"%s", DEVICE_NAME);
	if (IS_ERR(vc4mem_dev)) {
		dev_err(dev, "%s: Failed to create device\n", __func__);
		err = PTR_ERR(vc4mem_dev);
		goto failed_device_create;
	}

	dev_info(dev, "%s: Initialized\n", __func__);

	return 0;

failed_device_create:
	class_destroy(vc4mem_class);
failed_class_create:
	cdev_del(&vc4mem_cdev);
failed_cdev_add:
	unregister_chrdev_region(vc4mem_devid, 1);
failed_cdev_create:
	return err;
}

static int vc4mem_dev_remove(struct platform_device *pdev)
{
	device_destroy(vc4mem_class, vc4mem_devid);
	class_destroy(vc4mem_class);
	cdev_del(&vc4mem_cdev);
	unregister_chrdev_region(vc4mem_devid, 1);

	return 0;
}

static const struct of_device_id vc4mem_of_match_table[] = {
	{.compatible = "brcm,vc4mem",},
	{},
};
MODULE_DEVICE_TABLE(of, vc4mem_of_match_table);

static struct platform_driver vc4mem_dev_driver = {
	.probe = vc4mem_dev_probe,
	.remove = vc4mem_dev_remove,
	.driver = {
		.name = DRIVER_NAME,
		.owner = THIS_MODULE,
		.of_match_table = vc4mem_of_match_table,
	},
};

static int __init vc4mem_init(void)
{
	int ret;

	ret = platform_driver_register(&vc4mem_dev_driver);
	if (ret)
		return ret;

	return ret;
}
module_init(vc4mem_init);

static void __exit vc4mem_exit(void)
{
	platform_driver_unregister(&vc4mem_dev_driver);
}
module_exit(vc4mem_exit);


MODULE_ALIAS("platform:vc4mem");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("VideoCore IV memory management driver");
MODULE_AUTHOR("Sugizaki Yukimasa <ysugi@idein.jp>");

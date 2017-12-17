/*
 * A driver for Mailbox interface of Raspberry Pi.
 *
 * Written by Sugizaki Yukimasa <ysugi@idein.jp>
 * Copyright (c) 2017 Idein Inc.
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

#include <linux/types.h>
#include <soc/bcm2835/raspberrypi-firmware.h>
#include <linux/idein/mailbox.h>

/*
 * See https://github.com/raspberrypi/firmware/wiki/Mailbox-property-interface
 * about the parameters.
 */

u32 mailbox_mem_alloc(struct rpi_firmware *fw,
		const u32 size, const u32 align, const u32 flags)
{
	int ret;
	struct {
		union {
			struct {
				u32 size;
				u32 align;
				u32 flags;
			} in;
			struct {
				u32 handle;
			} out;
		};
	} ioparam = {
		.in = {
			.size = size,
			.align = align,
			.flags = flags,
		},
	};

	ret = rpi_firmware_property(fw, RPI_FIRMWARE_ALLOCATE_MEMORY,
			&ioparam, sizeof(ioparam));

	if (ret) {
		pr_err("%s: Mailbox property call failed: %d\n", __func__, ret);
		return 0;
	}
	return ioparam.out.handle;
}
EXPORT_SYMBOL_GPL(mailbox_mem_alloc);

int mailbox_mem_release(struct rpi_firmware *fw, const u32 handle)
{
	int ret;
	struct {
		union {
			struct {
				u32 handle;
			} in;
			struct {
				u32 status;
			} out;
		};
	} ioparam = {
		.in = {
			.handle = handle,
		},
	};

	ret = rpi_firmware_property(fw, RPI_FIRMWARE_RELEASE_MEMORY,
			&ioparam, sizeof(ioparam));
	if (ret)
		return ret;
	if (ioparam.out.status)
		return ioparam.out.status;
	return 0;
}
EXPORT_SYMBOL_GPL(mailbox_mem_release);

u32 mailbox_mem_lock(struct rpi_firmware *fw, const u32 handle)
{
	int ret;
	struct {
		union {
			struct {
				u32 handle;
			} in;
			struct {
				u32 bus;
			} out;
		};
	} ioparam = {
		.in = {
			.handle = handle,
		},
	};

	ret = rpi_firmware_property(fw, RPI_FIRMWARE_LOCK_MEMORY,
			&ioparam, sizeof(ioparam));
	if (ret)
		return ret;
	return ioparam.out.bus;
}
EXPORT_SYMBOL_GPL(mailbox_mem_lock);

int mailbox_mem_unlock(struct rpi_firmware *fw, const u32 bus)
{
	int ret;
	struct {
		union {
			struct {
				u32 bus;
			} in;
			struct {
				u32 status;
			} out;
		};
	} ioparam = {
		.in = {
			.bus = bus,
		},
	};

	ret = rpi_firmware_property(fw, RPI_FIRMWARE_UNLOCK_MEMORY,
			&ioparam, sizeof(ioparam));
	if (ret)
		return ret;
	if (ioparam.out.status)
		return ioparam.out.status;
	return 0;
}
EXPORT_SYMBOL_GPL(mailbox_mem_unlock);

/*
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

#ifndef __IDEIN_MAILBOX_H__
#define __IDEIN_MAILBOX_H__

#include <linux/types.h>
#include <soc/bcm2835/raspberrypi-firmware.h>

u32 mailbox_mem_alloc(struct rpi_firmware *fw,
		const u32 size, const u32 align, const u32 flags);
int mailbox_mem_release(struct rpi_firmware *fw, const u32 handle);
u32 mailbox_mem_lock(struct rpi_firmware *fw, const u32 handle);
int mailbox_mem_unlock(struct rpi_firmware *fw, const u32 bus);

/*
 * mem_alloc flags.
 * Bits [3:2] specifies caching type and the other bits specifies flags.
 */
enum {
	/* normal allocating alias. Don't use from ARM */
	MEM_FLAG_NORMAL = 0 << 2,
	/* 0xC alias uncached */
	MEM_FLAG_DIRECT = 1 << 2,
	/* 0x8 alias. Non-allocating in L2 but coherent */
	MEM_FLAG_COHERENT = 2 << 2,
	/* Allocating in L2 */
	MEM_FLAG_L1_NONALLOCATING = (MEM_FLAG_DIRECT | MEM_FLAG_COHERENT),

	/* can be resized to 0 at any time. Use for cached data */
	MEM_FLAG_DISCARDABLE = 1 << 0,
	/* initialise buffer to all zeros */
	MEM_FLAG_ZERO = 1 << 4,
	/* don't initialise (default is initialise to all ones */
	MEM_FLAG_NO_INIT = 1 << 5,
	/* Likely to be locked for long periods of time. */
	MEM_FLAG_HINT_PERMALOCK = 1 << 6,
};

#define __bus_to_phys(x) ((u32)(x) & ~0xc0000000)

#endif /* __IDEIN_MAILBOX_H__ */

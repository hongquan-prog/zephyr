/*
 * Copyright (c) 2026 Process Mission
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/drivers/virtio/virtio_buf.h>
#include <zephyr/kernel.h>
#include <zephyr/kernel/internal/mm.h>
#include <zephyr/linker/linker-defs.h>
#include <zephyr/sys/util.h>

/*
 * The accepted region is the boot mapping of RAM: the interval in which
 * k_mem_phys_addr() is defined. It is derived from the same definitions the
 * kernel uses, which are visible to drivers, instead of the kernel-private
 * mmu.h.
 */
static bool virtio_buf_region(uintptr_t *lower, uintptr_t *upper)
{
#if defined(CONFIG_MMU) && defined(CONFIG_ARCH_MAPS_ALL_RAM)
	uintptr_t phys_end = (uintptr_t)DT_CHOSEN_SRAM_ADDR;

	/* The whole RAM is mapped at boot, so the window ends with it. */
	if (phys_end > UINTPTR_MAX - (uintptr_t)DT_CHOSEN_SRAM_SIZE) {
		return false;
	}

	phys_end += (uintptr_t)DT_CHOSEN_SRAM_SIZE;
	*lower = (uintptr_t)z_mapped_start;
	*upper = (uintptr_t)K_MEM_VIRT_ADDR(phys_end);

	return *upper > *lower;
#elif defined(CONFIG_MMU)
	/* Only the kernel image is mapped at boot. */
	*lower = (uintptr_t)z_mapped_start;
	*upper = (uintptr_t)z_mapped_end;

	return *upper > *lower;
#elif DT_HAS_CHOSEN(zephyr_sram)
	*lower = (uintptr_t)DT_CHOSEN_SRAM_ADDR;
	*upper = (uintptr_t)DT_CHOSEN_SRAM_ADDR;

	if (*upper > UINTPTR_MAX - (uintptr_t)DT_CHOSEN_SRAM_SIZE) {
		return false;
	}

	*upper += (uintptr_t)DT_CHOSEN_SRAM_SIZE;

	return *upper > *lower;
#else
	ARG_UNUSED(lower);
	ARG_UNUSED(upper);

	/* Without a known RAM region there is nothing the device can address. */
	return false;
#endif
}

bool virtio_buf_is_directly_addressable(const void *addr, size_t len)
{
	uintptr_t start = (uintptr_t)addr;
	uintptr_t lower;
	uintptr_t upper;

	if (IS_ENABLED(CONFIG_DEMAND_PAGING)) {
		return false;
	}

	if (addr == NULL) {
		return false;
	}

	if (len > UINTPTR_MAX - start) {
		return false;
	}

	if (!virtio_buf_region(&lower, &upper)) {
		return false;
	}

	if (len == 0U) {
		return start > lower && start < upper;
	}

	return start >= lower && (start + len - 1U) < upper;
}

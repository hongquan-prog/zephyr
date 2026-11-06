/*
 * Copyright (c) 2026 Process Mission
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Virtio buffer helpers.
 * @ingroup virtio_interface
 */

#ifndef ZEPHYR_INCLUDE_DRIVERS_VIRTIO_VIRTIO_BUF_H_
#define ZEPHYR_INCLUDE_DRIVERS_VIRTIO_VIRTIO_BUF_H_

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Virtio buffer helpers
 * @defgroup virtio_buf_interface Virtio buffer helpers
 * @ingroup virtio_interface
 * @since 4.6
 * @version 0.1.0
 * @{
 */

/**
 * @brief Check whether a buffer range is directly usable by a virtio device
 *
 * The predicate is conservative: it reports true when the whole
 * [@p addr, @p addr + @p len) range lies in the region for which
 * k_mem_phys_addr() yields a device-usable address, under the assumptions the
 * virtio-mmio and virtio-pci transports already make. That region is the
 * boot-time linear mapping of RAM: the kernel image on MMU systems that map
 * only the image, all RAM on systems that map it at boot, and the chosen SRAM
 * region on systems without an MMU.
 *
 * It does @b not check device address windows, IOMMU mappings or cache
 * coherency, and it is not a substitute for any of them. A true result also
 * says nothing about the buffer remaining in that state.
 *
 * With demand paging enabled the function always returns false, including for
 * zero-length ranges, because the backing pages can be evicted or remapped.
 * Otherwise a zero-length range is accepted only when @p addr is strictly
 * inside the region. A NULL address and a range whose end wraps around are
 * never accepted.
 *
 * @param addr start of the buffer
 * @param len length of the buffer in bytes
 * @retval true The whole range is usable with k_mem_phys_addr().
 * @retval false Part of the range is not, or the arguments are invalid.
 */
bool virtio_buf_is_directly_addressable(const void *addr, size_t len);

/**
 * @}
 */

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_INCLUDE_DRIVERS_VIRTIO_VIRTIO_BUF_H_ */

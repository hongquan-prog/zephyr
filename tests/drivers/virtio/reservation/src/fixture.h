/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 Process Mission
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef TESTS_DRIVERS_VIRTIO_RESERVATION_FIXTURE_H_
#define TESTS_DRIVERS_VIRTIO_RESERVATION_FIXTURE_H_

#include <zephyr/drivers/virtio.h>
#include <zephyr/drivers/virtio/virtqueue.h>

/**
 * Software virtio device used to drive the real interrupt path.
 *
 * Each device owns at most one virtqueue, so two independent devices let a
 * test destroy one queue while the other still has blocked submitters.
 */
const struct device *fixture_dev_a(void);
const struct device *fixture_dev_b(void);

/** Attach (or detach, with NULL) the queue served by a fixture device. */
void fixture_attach(const struct device *dev, struct virtq *vq);

struct virtq *fixture_vq(const struct device *dev);

/** Append one used ring entry, as the device would. */
void fixture_used_push(const struct device *dev, uint16_t head, uint32_t len);

/** Append a used ring entry and run the real virtio_isr() from interrupt context. */
void fixture_complete_isr(const struct device *dev, uint16_t head, uint32_t len);

/** Index of the last descriptor chain published to the available ring. */
uint16_t fixture_last_avail_head(const struct virtq *vq);

#endif /* TESTS_DRIVERS_VIRTIO_RESERVATION_FIXTURE_H_ */

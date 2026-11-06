/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 Process Mission
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/drivers/virtio.h>
#include <zephyr/drivers/virtio/virtqueue.h>
#include <zephyr/irq_offload.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/barrier.h>
#include <zephyr/sys/byteorder.h>

#include "virtio_common.h"
#include "fixture.h"

struct fixture_data {
	struct virtq *vq;
};

static struct fixture_data fixture_a_data;
static struct fixture_data fixture_b_data;

static struct virtq *fixture_get_virtqueue(const struct device *dev, uint16_t queue_idx)
{
	struct fixture_data *data = dev->data;

	if (queue_idx != 0U) {
		return NULL;
	}

	return data->vq;
}

static void fixture_notify_virtqueue(const struct device *dev, uint16_t queue_idx)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(queue_idx);
}

static void *fixture_get_device_specific_config(const struct device *dev)
{
	ARG_UNUSED(dev);

	return NULL;
}

static bool fixture_read_device_feature_bit(const struct device *dev, int bit)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(bit);

	return false;
}

static int fixture_write_driver_feature_bit(const struct device *dev, int bit, bool value)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(bit);
	ARG_UNUSED(value);

	return 0;
}

static int fixture_commit_feature_bits(const struct device *dev)
{
	ARG_UNUSED(dev);

	return 0;
}

static int fixture_init_virtqueues(const struct device *dev, uint16_t num_queues,
				   virtio_enumerate_queues cb, void *opaque)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(num_queues);
	ARG_UNUSED(cb);
	ARG_UNUSED(opaque);

	return 0;
}

static void fixture_finalize_init(const struct device *dev)
{
	ARG_UNUSED(dev);
}

static DEVICE_API(virtio, fixture_api) = {
	.get_virtqueue = fixture_get_virtqueue,
	.notify_virtqueue = fixture_notify_virtqueue,
	.get_device_specific_config = fixture_get_device_specific_config,
	.read_device_feature_bit = fixture_read_device_feature_bit,
	.write_driver_feature_bit = fixture_write_driver_feature_bit,
	.commit_feature_bits = fixture_commit_feature_bits,
	.init_virtqueues = fixture_init_virtqueues,
	.finalize_init = fixture_finalize_init,
};

DEVICE_DEFINE(fixture_a, "virtio-fixture-a", NULL, NULL, &fixture_a_data, NULL, POST_KERNEL,
	      CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &fixture_api);
DEVICE_DEFINE(fixture_b, "virtio-fixture-b", NULL, NULL, &fixture_b_data, NULL, POST_KERNEL,
	      CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &fixture_api);

const struct device *fixture_dev_a(void)
{
	return DEVICE_GET(fixture_a);
}

const struct device *fixture_dev_b(void)
{
	return DEVICE_GET(fixture_b);
}

void fixture_attach(const struct device *dev, struct virtq *vq)
{
	struct fixture_data *data = dev->data;

	data->vq = vq;
}

struct virtq *fixture_vq(const struct device *dev)
{
	struct fixture_data *data = dev->data;

	return data->vq;
}

void fixture_used_push(const struct device *dev, uint16_t head, uint32_t len)
{
	struct virtq *vq = fixture_vq(dev);
	uint16_t idx = sys_le16_to_cpu(vq->used->idx);
	struct virtq_used_elem *elem = &vq->used->ring[idx % vq->num];

	elem->id = sys_cpu_to_le32(head);
	elem->len = sys_cpu_to_le32(len);
	barrier_dmem_fence_full();
	vq->used->idx = sys_cpu_to_le16(idx + 1U);
}

struct fixture_irq {
	const struct device *dev;
};

static void fixture_irq_handler(const void *arg)
{
	const struct fixture_irq *irq = arg;

	virtio_isr(irq->dev, VIRTIO_QUEUE_INTERRUPT, 1);
}

void fixture_complete_isr(const struct device *dev, uint16_t head, uint32_t len)
{
	struct fixture_irq irq = {.dev = dev};

	fixture_used_push(dev, head, len);
	irq_offload(fixture_irq_handler, &irq);
}

uint16_t fixture_last_avail_head(const struct virtq *vq)
{
	uint16_t idx = sys_le16_to_cpu(vq->avail->idx);

	__ASSERT_NO_MSG(vq->num != 0U);
	__ASSERT_NO_MSG(idx != 0U);

	return sys_le16_to_cpu(vq->avail->ring[(idx - 1U) & (vq->num - 1U)]);
}

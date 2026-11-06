/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 Process Mission
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * A separate test binary so these are the first virtq_create() calls the
 * process makes: the lazy wake workqueue is still unstarted when they race.
 */

#include <zephyr/drivers/virtio/virtqueue.h>
#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#define THREAD_STACK_SIZE 1024

K_THREAD_STACK_DEFINE(startup_a_stack, THREAD_STACK_SIZE);
K_THREAD_STACK_DEFINE(startup_b_stack, THREAD_STACK_SIZE);
static struct k_thread startup_a;
static struct k_thread startup_b;

static struct virtq startup_vq_a;
static struct virtq startup_vq_b;
static int startup_ret_a;
static int startup_ret_b;
static struct k_sem startup_done;

static void create_entry(void *p1, void *p2, void *p3)
{
	int *ret = p1;
	struct virtq *vq = p2;

	ARG_UNUSED(p3);

	*ret = virtq_create(vq, 4);
	k_sem_give(&startup_done);
}

ZTEST(reservation_startup, test_concurrent_create_starts_wake_queue_once)
{
	k_sem_init(&startup_done, 0, 2);

	k_thread_create(&startup_a, startup_a_stack, K_THREAD_STACK_SIZEOF(startup_a_stack),
			create_entry, &startup_ret_a, &startup_vq_a, NULL, K_PRIO_PREEMPT(2), 0,
			K_NO_WAIT);
	k_thread_create(&startup_b, startup_b_stack, K_THREAD_STACK_SIZEOF(startup_b_stack),
			create_entry, &startup_ret_b, &startup_vq_b, NULL, K_PRIO_PREEMPT(3), 0,
			K_NO_WAIT);

	zassert_ok(k_sem_take(&startup_done, K_MSEC(2000)), "creator did not finish");
	zassert_ok(k_sem_take(&startup_done, K_MSEC(2000)), "creator did not finish");
	zassert_ok(k_thread_join(&startup_a, K_MSEC(2000)));
	zassert_ok(k_thread_join(&startup_b, K_MSEC(2000)));

	zassert_ok(startup_ret_a, "first concurrent create failed");
	zassert_ok(startup_ret_b, "second concurrent create failed");
	zassert_equal(startup_vq_a.num, 4);
	zassert_equal(startup_vq_b.num, 4);

	virtq_free(&startup_vq_a);
	virtq_free(&startup_vq_b);
}

ZTEST_SUITE(reservation_startup, NULL, NULL, NULL, NULL, NULL);

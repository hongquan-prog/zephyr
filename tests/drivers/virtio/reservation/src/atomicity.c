/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 Process Mission
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Deterministic regression for the partial-reservation bug, kept free of
 * anything added by the all-or-nothing change so the same file builds and runs
 * against the parent revision.
 */

#include <zephyr/drivers/virtio/virtqueue.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/ztest.h>

#define CHAIN_LEN  2
#define TIMEOUT_MS 2000

/* The submitter must outrank the ztest thread for the semaphore ordering. */
BUILD_ASSERT(K_PRIO_COOP(1) < CONFIG_ZTEST_THREAD_PRIORITY,
	     "submitter must be able to preempt the test thread");

static uint8_t submit_bufs[CHAIN_LEN][32] __aligned(4);

K_THREAD_STACK_DEFINE(waiting_stack, 1024);
static struct k_thread waiting_thread;

struct submitter {
	struct virtq *vq;
	atomic_t ret;
	struct k_sem entered;
};

static void submitter_entry(void *p1, void *p2, void *p3)
{
	struct submitter *s = p1;
	struct virtq_buf bufs[CHAIN_LEN];

	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	for (uint16_t i = 0; i < CHAIN_LEN; i++) {
		bufs[i].addr = submit_bufs[i];
		bufs[i].len = sizeof(submit_bufs[i]);
	}

	k_sem_give(&s->entered);
	atomic_set(&s->ret, virtq_add_buffer_chain(s->vq, bufs, CHAIN_LEN, 0, NULL, NULL,
						   K_MSEC(TIMEOUT_MS)));
}

static uint16_t avail_head(const struct virtq *vq)
{
	uint16_t idx = sys_le16_to_cpu(vq->avail->idx);

	return sys_le16_to_cpu(vq->avail->ring[(idx - 1U) & (vq->num - 1U)]);
}

ZTEST(reservation_atomicity, test_waiting_chain_reserves_nothing)
{
	struct virtq vq;
	struct submitter s = {.vq = &vq};
	struct virtq_buf small = {.addr = submit_bufs[0], .len = sizeof(submit_bufs[0])};
	uint16_t held;
	int waiting_ret;
	int small_ret;
	int64_t start;

	if (CONFIG_MP_MAX_NUM_CPUS > 1) {
		/* The ordering argument is single-core only; SMP runs the stress case. */
		ztest_test_skip();
	}

	zassert_ok(virtq_create(&vq, 2));
	atomic_set(&s.ret, -1);

	/* H holds one descriptor through the legacy API. */
	zassert_ok(virtq_get_free_desc(&vq, &held, K_NO_WAIT));

	k_sem_init(&s.entered, 0, 1);
	start = k_uptime_get();
	k_thread_create(&waiting_thread, waiting_stack, K_THREAD_STACK_SIZEOF(waiting_stack),
			submitter_entry, &s, NULL, NULL, K_PRIO_COOP(1), 0, K_NO_WAIT);

	/* A is cooperative: nothing else runs until it blocks. */
	zassert_ok(k_sem_take(&s.entered, K_MSEC(500)), "submitter did not start");

	/* Its deadline must not have passed, otherwise "not returned" means nothing. */
	zassert_true(k_uptime_get() - start < TIMEOUT_MS,
		     "submitter deadline already elapsed after %d ms",
		     (int)(k_uptime_get() - start));
	zassert_equal(atomic_get(&s.ret), -1, "submitter returned before inspecting the queue");

	/* T must not be blocked by A's pending reservation. */
	small_ret = virtq_add_buffer_chain(&vq, &small, 1, 0, NULL, NULL, K_NO_WAIT);

	/* Bounded cleanup before reporting a failure. */
	if (small_ret == 0) {
		virtq_add_free_desc(&vq, avail_head(&vq));
	}
	virtq_add_free_desc(&vq, held);
	zassert_ok(k_thread_join(&waiting_thread, K_MSEC(2000)), "submitter did not return");

	waiting_ret = (int)atomic_get(&s.ret);
	if (waiting_ret == 0) {
		uint16_t head = avail_head(&vq);
		uint16_t next = sys_le16_to_cpu(vq.desc[head].next);

		virtq_add_free_desc(&vq, head);
		virtq_add_free_desc(&vq, next);
	}

	virtq_free(&vq);

	zassert_true(waiting_ret == 0 || waiting_ret == -EAGAIN, "waiting chain returned %d",
		     waiting_ret);
	zassert_equal(small_ret, 0,
		      "one-descriptor submission failed with %d while a larger chain waited",
		      small_ret);
}

static void atomicity_teardown(void *fixture)
{
	ARG_UNUSED(fixture);

	k_thread_abort(&waiting_thread);
}

ZTEST_SUITE(reservation_atomicity, NULL, NULL, NULL, atomicity_teardown, NULL);

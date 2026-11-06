/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 Process Mission
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/drivers/virtio/virtqueue.h>
#include <zephyr/irq_offload.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/ztest.h>

#include "fixture.h"

#define MAX_CHAIN         8
#define THREAD_STACK_SIZE 1024

/* The wake tests rely on lower-priority waiters running only after we block. */
BUILD_ASSERT(CONFIG_ZTEST_THREAD_PRIORITY < K_PRIO_PREEMPT(2),
	     "test thread must outrank the waiter threads");

/* Every submission in this suite uses these buffers. */
static uint8_t chain_bufs[MAX_CHAIN][32] __aligned(4);
static struct k_spinlock test_lock;

K_THREAD_STACK_DEFINE(thread_a_stack, THREAD_STACK_SIZE);
K_THREAD_STACK_DEFINE(thread_b_stack, THREAD_STACK_SIZE);
static struct k_thread thread_a;
static struct k_thread thread_b;

struct waiter {
	struct virtq *vq;
	uint16_t count;
	k_timeout_t timeout;
	uint16_t desc;
	int ret;
	bool legacy;
	struct k_sem entered;
	struct k_sem done;
};

static int submit_chain(struct virtq *vq, uint16_t count, k_timeout_t timeout,
			virtq_receive_callback cb, void *opaque)
{
	struct virtq_buf bufs[MAX_CHAIN];

	for (uint16_t i = 0; i < count; i++) {
		bufs[i].addr = chain_bufs[i];
		bufs[i].len = sizeof(chain_bufs[i]);
	}

	return virtq_add_buffer_chain(vq, bufs, count, 0, cb, opaque, timeout);
}

static void waiter_entry(void *p1, void *p2, void *p3)
{
	struct waiter *w = p1;

	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	k_sem_give(&w->entered);

	if (w->legacy) {
		w->ret = virtq_get_free_desc(w->vq, &w->desc, w->timeout);
	} else {
		w->ret = submit_chain(w->vq, w->count, w->timeout, NULL, NULL);
	}

	k_sem_give(&w->done);
}

static struct k_thread *live_threads[2];
static int live_count;

static void track_stop(struct k_thread *thread)
{
	for (int i = 0; i < live_count; i++) {
		if (live_threads[i] == thread) {
			live_threads[i] = live_threads[--live_count];
			return;
		}
	}
}

static void start_thread(struct k_thread *thread, k_thread_stack_t *stack, size_t size,
			 k_thread_entry_t entry, void *arg, int prio)
{
	k_thread_create(thread, stack, size, entry, arg, NULL, NULL, prio, 0, K_NO_WAIT);
	live_threads[live_count++] = thread;
}

static void abort_live_threads(void)
{
	while (live_count > 0) {
		k_thread_abort(live_threads[--live_count]);
	}
}

/* Never leave a test thread running into the next test. */
static void join_thread(struct k_thread *thread)
{
	if (k_thread_join(thread, K_MSEC(2000)) != 0) {
		k_thread_abort(thread);
	}

	track_stop(thread);
}

static void waiter_init(struct waiter *w, struct virtq *vq, uint16_t count, k_timeout_t timeout)
{
	*w = (struct waiter){
		.vq = vq,
		.count = count,
		.timeout = timeout,
	};

	k_sem_init(&w->entered, 0, 1);
	k_sem_init(&w->done, 0, 1);
}

static void legacy_waiter_init(struct waiter *w, struct virtq *vq, k_timeout_t timeout)
{
	waiter_init(w, vq, 1, timeout);
	w->legacy = true;
}

static void vq_init(struct virtq *vq, size_t size)
{
	zassert_ok(virtq_create(vq, size), "queue creation failed");
	fixture_attach(fixture_dev_a(), vq);
}

static void vq_deinit(struct virtq *vq)
{
	fixture_attach(fixture_dev_a(), NULL);
	virtq_free(vq);
}

static uint16_t last_head(const struct virtq *vq)
{
	return fixture_last_avail_head(vq);
}

static void complete_chain(struct virtq *vq, uint16_t head)
{
	fixture_complete_isr(fixture_dev_a(), head, 0);
}

static void wait_for_waiters(struct virtq *vq, long count, k_timeout_t timeout)
{
	k_timepoint_t deadline = sys_timepoint_calc(timeout);

	while (atomic_get(&vq->waiters) < count) {
		if (sys_timepoint_expired(deadline)) {
			break;
		}
		k_sleep(K_MSEC(1));
	}
}

ZTEST(reservation, test_chain_larger_than_queue_is_rejected)
{
	struct virtq vq;

	vq_init(&vq, 2);

	zassert_equal(submit_chain(&vq, 3, K_NO_WAIT, NULL, NULL), -EINVAL);
	zassert_equal(submit_chain(&vq, 3, K_FOREVER, NULL, NULL), -EINVAL);
	zassert_equal(atomic_get(&vq.free_desc_n), 2, "rejected chain reserved descriptors");

	vq_deinit(&vq);
}

ZTEST(reservation, test_empty_chain_is_rejected)
{
	struct virtq vq;

	vq_init(&vq, 2);
	zassert_equal(submit_chain(&vq, 0, K_NO_WAIT, NULL, NULL), -EINVAL);
	vq_deinit(&vq);
}

ZTEST(reservation, test_thread_no_wait)
{
	struct virtq vq;

	vq_init(&vq, 1);

	zassert_ok(submit_chain(&vq, 1, K_NO_WAIT, NULL, NULL));
	zassert_equal(submit_chain(&vq, 1, K_NO_WAIT, NULL, NULL), -EBUSY);

	complete_chain(&vq, last_head(&vq));
	zassert_ok(submit_chain(&vq, 1, K_NO_WAIT, NULL, NULL));

	vq_deinit(&vq);
}

struct isr_submit {
	struct virtq *vq;
	uint16_t count;
	k_timeout_t timeout;
	int ret;
};

static void isr_submit_entry(const void *arg)
{
	struct isr_submit *s = (struct isr_submit *)arg;

	s->ret = submit_chain(s->vq, s->count, s->timeout, NULL, NULL);
}

ZTEST(reservation, test_isr_no_wait)
{
	struct virtq vq;
	struct isr_submit submit;

	vq_init(&vq, 1);

	submit = (struct isr_submit){.vq = &vq, .count = 1, .timeout = K_NO_WAIT};
	irq_offload(isr_submit_entry, &submit);
	zassert_ok(submit.ret, "ISR submission with free descriptors failed");

	submit.ret = -1;
	irq_offload(isr_submit_entry, &submit);
	zassert_equal(submit.ret, -EBUSY, "ISR submission blocked or missed -EBUSY");

	complete_chain(&vq, last_head(&vq));
	vq_deinit(&vq);
}

ZTEST(reservation, test_no_wait_with_interrupts_locked)
{
	struct virtq vq;
	k_spinlock_key_t key;
	int first;
	int second;

	vq_init(&vq, 2);

	key = k_spin_lock(&test_lock);
	first = submit_chain(&vq, 2, K_NO_WAIT, NULL, NULL);
	second = submit_chain(&vq, 1, K_NO_WAIT, NULL, NULL);
	k_spin_unlock(&test_lock, key);

	zassert_ok(first, "fitting chain failed with interrupts locked");
	zassert_equal(second, -EBUSY, "exhausted queue did not report -EBUSY");

	vq_deinit(&vq);
}

static int cb_submit_ret;
static uint16_t cb_submit_count;

static void resubmit_cb(void *opaque, uint32_t used_len)
{
	ARG_UNUSED(opaque);
	ARG_UNUSED(used_len);

	cb_submit_ret =
		submit_chain(fixture_vq(fixture_dev_a()), cb_submit_count, K_FOREVER, NULL, NULL);
}

ZTEST(reservation, test_completion_callback_resubmits_with_forever)
{
	struct virtq vq;
	uint16_t busy;
	uint16_t pending;

	vq_init(&vq, 2);

	/* Occupy one descriptor so the callback finds the queue exhausted. */
	zassert_ok(submit_chain(&vq, 1, K_NO_WAIT, NULL, NULL));
	busy = last_head(&vq);

	cb_submit_count = 2;
	cb_submit_ret = -1;
	zassert_ok(submit_chain(&vq, 1, K_NO_WAIT, resubmit_cb, NULL));
	pending = last_head(&vq);

	complete_chain(&vq, pending);
	zassert_equal(cb_submit_ret, -EBUSY,
		      "ISR callback blocked or missed -EBUSY for a chain that cannot fit");

	/* No descriptor is in use, so the eth-style single-buffer requeue works. */
	complete_chain(&vq, busy);

	cb_submit_count = 1;
	cb_submit_ret = -1;
	zassert_ok(submit_chain(&vq, 1, K_NO_WAIT, resubmit_cb, NULL));
	pending = last_head(&vq);
	complete_chain(&vq, pending);

	zassert_equal(cb_submit_ret, 0, "ISR callback requeue with K_FOREVER failed");

	/* The callback's own submission is still outstanding. */
	complete_chain(&vq, last_head(&vq));

	vq_deinit(&vq);
}

ZTEST(reservation, test_legacy_timeout_and_registration)
{
	struct virtq vq;
	uint16_t desc;
	int64_t start;
	int64_t elapsed;

	vq_init(&vq, 1);
	zassert_ok(submit_chain(&vq, 1, K_NO_WAIT, NULL, NULL));

	start = k_uptime_get();
	zassert_equal(virtq_get_free_desc(&vq, &desc, K_MSEC(50)), -EAGAIN);
	elapsed = k_uptime_get() - start;

	zassert_true(elapsed >= 40, "timeout returned after %d ms", (int)elapsed);
	zassert_true(elapsed < 1000, "timeout returned after %d ms", (int)elapsed);
	zassert_equal(atomic_get(&vq.waiters), 0, "timed out waiter stayed registered");
	zassert_equal(virtq_get_free_desc(&vq, &desc, K_NO_WAIT), -EBUSY);

	complete_chain(&vq, last_head(&vq));
	zassert_ok(virtq_get_free_desc(&vq, &desc, K_NO_WAIT));
	virtq_add_free_desc(&vq, desc);

	vq_deinit(&vq);
}

ZTEST(reservation, test_legacy_zero_size_queue_is_rejected)
{
	struct virtq vq;

	vq_init(&vq, 0);
	zassert_equal(virtq_get_free_desc(&vq, NULL, K_FOREVER), -EINVAL);
	zassert_equal(atomic_get(&vq.waiters), 0);
	vq_deinit(&vq);
}

ZTEST(reservation, test_legacy_release_wakes_chain_waiter)
{
	struct virtq vq;
	struct waiter w;
	uint16_t pending;

	vq_init(&vq, 2);
	zassert_ok(submit_chain(&vq, 1, K_NO_WAIT, NULL, NULL));
	pending = last_head(&vq);

	waiter_init(&w, &vq, 2, K_FOREVER);
	start_thread(&thread_a, thread_a_stack, K_THREAD_STACK_SIZEOF(thread_a_stack), waiter_entry,
		     &w, K_PRIO_PREEMPT(2));
	zassert_ok(k_sem_take(&w.entered, K_MSEC(500)));

	wait_for_waiters(&vq, 1, K_MSEC(500));
	zassert_equal(atomic_get(&vq.waiters), 1, "chain waiter did not register");
	k_sleep(K_MSEC(2));

	/* A legacy release issued from a thread must run the wake protocol too. */
	virtq_add_free_desc(&vq, pending);

	zassert_ok(k_sem_take(&w.done, K_MSEC(1000)), "legacy release did not wake the waiter");
	zassert_ok(w.ret, "woken chain reservation failed");
	zassert_equal(atomic_get(&vq.waiters), 0);

	join_thread(&thread_a);
	vq_deinit(&vq);
}

ZTEST(reservation, test_legacy_and_chain_waiters_coexist)
{
	struct virtq vq;
	struct waiter legacy;
	struct waiter chain;
	uint16_t descs[4];

	vq_init(&vq, 4);

	/* Exhaust the queue: the legacy getter needs one descriptor to wake up. */
	for (int i = 0; i < 4; i++) {
		zassert_ok(virtq_get_free_desc(&vq, &descs[i], K_NO_WAIT));
	}

	legacy_waiter_init(&legacy, &vq, K_FOREVER);
	start_thread(&thread_a, thread_a_stack, K_THREAD_STACK_SIZEOF(thread_a_stack), waiter_entry,
		     &legacy, K_PRIO_PREEMPT(2));
	zassert_ok(k_sem_take(&legacy.entered, K_MSEC(500)));

	waiter_init(&chain, &vq, 2, K_FOREVER);
	start_thread(&thread_b, thread_b_stack, K_THREAD_STACK_SIZEOF(thread_b_stack), waiter_entry,
		     &chain, K_PRIO_PREEMPT(2));
	zassert_ok(k_sem_take(&chain.entered, K_MSEC(500)));

	wait_for_waiters(&vq, 2, K_MSEC(1000));
	zassert_equal(atomic_get(&vq.waiters), 2, "both waiters did not register");

	for (int i = 0; i < 3; i++) {
		virtq_add_free_desc(&vq, descs[i]);
		k_sleep(K_MSEC(20));
	}

	zassert_ok(k_sem_take(&legacy.done, K_MSEC(2000)), "legacy waiter did not complete");
	zassert_ok(k_sem_take(&chain.done, K_MSEC(2000)), "chain waiter did not complete");
	zassert_ok(legacy.ret);
	zassert_ok(chain.ret);
	zassert_equal(atomic_get(&vq.waiters), 0);

	join_thread(&thread_a);
	join_thread(&thread_b);
	vq_deinit(&vq);
}

ZTEST(reservation, test_finite_timeout)
{
	struct virtq vq;
	uint16_t held;
	int64_t start;
	int64_t elapsed;
	int ret;

	vq_init(&vq, 2);
	zassert_ok(virtq_get_free_desc(&vq, &held, K_NO_WAIT));

	start = k_uptime_get();
	ret = submit_chain(&vq, 2, K_MSEC(100), NULL, NULL);
	elapsed = k_uptime_get() - start;

	zassert_equal(ret, -EAGAIN, "finite timeout returned %d", ret);
	zassert_true(elapsed >= 90, "returned after %d ms", (int)elapsed);
	zassert_true(elapsed < 400, "budget exceeded: %d ms", (int)elapsed);
	zassert_equal(atomic_get(&vq.waiters), 0);

	virtq_add_free_desc(&vq, held);
	vq_deinit(&vq);
}

ZTEST(reservation, test_repeated_wakeups_do_not_extend_budget)
{
	struct virtq vq;
	struct waiter w;
	uint16_t held_a;
	uint16_t held_b;
	uint16_t held_c;
	int64_t start;
	int64_t elapsed;

	/* Three of four descriptors stay in use, so four can never be reserved. */
	vq_init(&vq, 4);
	zassert_ok(virtq_get_free_desc(&vq, &held_a, K_NO_WAIT));
	zassert_ok(virtq_get_free_desc(&vq, &held_b, K_NO_WAIT));
	zassert_ok(virtq_get_free_desc(&vq, &held_c, K_NO_WAIT));

	start = k_uptime_get();
	waiter_init(&w, &vq, 4, K_MSEC(200));
	start_thread(&thread_a, thread_a_stack, K_THREAD_STACK_SIZEOF(thread_a_stack), waiter_entry,
		     &w, K_PRIO_PREEMPT(2));

	/* Fruitless broadcasts: releasing one descriptor is never enough. */
	do {
		virtq_add_free_desc(&vq, held_a);
		k_sleep(K_MSEC(5));
		zassert_ok(virtq_get_free_desc(&vq, &held_a, K_NO_WAIT));
	} while (k_uptime_get() - start < 150);

	zassert_ok(k_sem_take(&w.done, K_MSEC(2000)), "waiter did not return");
	elapsed = k_uptime_get() - start;

	zassert_equal(w.ret, -EAGAIN, "waiter returned %d", w.ret);
	zassert_true(elapsed >= 190, "waiter returned early: %d ms", (int)elapsed);
	zassert_true(elapsed < 600, "wakeups extended the budget: %d ms", (int)elapsed);
	zassert_equal(atomic_get(&vq.waiters), 0);

	virtq_add_free_desc(&vq, held_a);
	virtq_add_free_desc(&vq, held_b);
	virtq_add_free_desc(&vq, held_c);
	join_thread(&thread_a);
	vq_deinit(&vq);
}

ZTEST(reservation, test_entry_mutex_timeout)
{
	struct virtq vq;
	struct waiter w;
	uint16_t held;
	int64_t start;
	int64_t elapsed;

	vq_init(&vq, 2);
	zassert_ok(virtq_get_free_desc(&vq, &held, K_NO_WAIT));

	/* Hold the slow path mutex so the waiter cannot even enter it. */
	zassert_ok(k_mutex_lock(&vq.slow_lock, K_FOREVER));

	waiter_init(&w, &vq, 2, K_MSEC(50));
	start = k_uptime_get();
	start_thread(&thread_a, thread_a_stack, K_THREAD_STACK_SIZEOF(thread_a_stack), waiter_entry,
		     &w, K_PRIO_PREEMPT(2));

	zassert_ok(k_sem_take(&w.done, K_MSEC(600)), "waiter did not leave on its own budget");
	elapsed = k_uptime_get() - start;
	zassert_equal(w.ret, -EAGAIN);
	zassert_true(elapsed < 400, "entry timeout took %d ms", (int)elapsed);
	zassert_equal(atomic_get(&vq.waiters), 0, "entry timeout leaked a registration");

	k_mutex_unlock(&vq.slow_lock);
	join_thread(&thread_a);

	virtq_add_free_desc(&vq, held);
	vq_deinit(&vq);
}

ZTEST(reservation, test_condvar_timeout_waits_for_mutex_reacquisition)
{
	struct virtq vq;
	struct waiter w;
	uint16_t held;
	bool in_condvar = false;
	k_timepoint_t deadline;

	vq_init(&vq, 2);
	zassert_ok(virtq_get_free_desc(&vq, &held, K_NO_WAIT));

	/* Cooperative so it runs to the condition variable before we look. */
	waiter_init(&w, &vq, 2, K_MSEC(1000));
	start_thread(&thread_a, thread_a_stack, K_THREAD_STACK_SIZEOF(thread_a_stack), waiter_entry,
		     &w, K_PRIO_COOP(1));
	zassert_ok(k_sem_take(&w.entered, K_MSEC(500)));
	k_sleep(K_MSEC(20));

	/* Acquiring the mutex here means the waiter is pending on the condvar. */
	deadline = sys_timepoint_calc(K_MSEC(500));
	while (!sys_timepoint_expired(deadline)) {
		if (atomic_get(&vq.waiters) > 0 && k_mutex_lock(&vq.slow_lock, K_NO_WAIT) == 0) {
			in_condvar = true;
			break;
		}
		k_sleep(K_MSEC(2));
	}
	zassert_true(in_condvar, "waiter never reached the condition variable");

	/* Its budget expires while we own the mutex. */
	zassert_equal(atomic_get(&vq.waiters), 1, "waiter left the condvar early");
	k_sleep(K_MSEC(1200));
	zassert_equal(k_sem_take(&w.done, K_NO_WAIT), -EBUSY,
		      "waiter returned before the mutex was released");

	k_mutex_unlock(&vq.slow_lock);
	zassert_ok(k_sem_take(&w.done, K_MSEC(1000)),
		   "waiter did not return after the mutex was released");
	zassert_equal(w.ret, -EAGAIN);
	zassert_equal(atomic_get(&vq.waiters), 0);

	join_thread(&thread_a);
	virtq_add_free_desc(&vq, held);
	vq_deinit(&vq);
}

ZTEST(reservation, test_isr_release_wakes_registered_waiter)
{
	struct virtq vq;
	struct waiter w;
	uint16_t pending;

	vq_init(&vq, 2);
	zassert_ok(submit_chain(&vq, 1, K_NO_WAIT, NULL, NULL));
	pending = last_head(&vq);

	waiter_init(&w, &vq, 2, K_FOREVER);
	start_thread(&thread_a, thread_a_stack, K_THREAD_STACK_SIZEOF(thread_a_stack), waiter_entry,
		     &w, K_PRIO_PREEMPT(2));
	zassert_ok(k_sem_take(&w.entered, K_MSEC(500)));

	wait_for_waiters(&vq, 1, K_MSEC(500));
	zassert_equal(atomic_get(&vq.waiters), 1, "chain waiter did not register");
	k_sleep(K_MSEC(2));

	/* Free the missing descriptor from the real interrupt path. */
	complete_chain(&vq, pending);

	zassert_ok(k_sem_take(&w.done, K_MSEC(1000)), "ISR release did not wake the waiter");
	zassert_ok(w.ret);
	join_thread(&thread_a);
	vq_deinit(&vq);
}

struct parked_waiter {
	struct virtq *vq;
	uint16_t count;
	int ret;
	struct k_sem parked;
	struct k_sem go;
	struct k_sem done_sem;
};

static void parked_waiter_entry(void *p1, void *p2, void *p3)
{
	struct parked_waiter *w = p1;

	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	k_sem_give(&w->parked);
	k_sem_take(&w->go, K_FOREVER);
	w->ret = submit_chain(w->vq, w->count, K_FOREVER, NULL, NULL);
	k_sem_give(&w->done_sem);
}

ZTEST(reservation, test_free_before_registration_is_not_lost)
{
	struct virtq vq;
	struct parked_waiter w;
	uint16_t pending;

	vq_init(&vq, 2);
	zassert_ok(submit_chain(&vq, 1, K_NO_WAIT, NULL, NULL));
	pending = last_head(&vq);

	/*
	 * Park the waiter before it calls, so the free batch provably precedes
	 * its registration on any number of CPUs: its first attempt must see
	 * the free descriptor without depending on a wakeup.
	 */
	w = (struct parked_waiter){.vq = &vq, .count = 2};
	k_sem_init(&w.parked, 0, 1);
	k_sem_init(&w.go, 0, 1);
	k_sem_init(&w.done_sem, 0, 1);

	start_thread(&thread_a, thread_a_stack, K_THREAD_STACK_SIZEOF(thread_a_stack),
		     parked_waiter_entry, &w, K_PRIO_PREEMPT(2));
	zassert_ok(k_sem_take(&w.parked, K_MSEC(500)), "waiter did not park");
	zassert_equal(atomic_get(&vq.waiters), 0, "waiter registered before the free batch");

	complete_chain(&vq, pending);
	k_sem_give(&w.go);

	zassert_ok(k_sem_take(&w.done_sem, K_MSEC(1000)), "waiter did not see the free batch");
	zassert_ok(w.ret);
	join_thread(&thread_a);
	vq_deinit(&vq);
}

static void reservation_teardown(void *fixture)
{
	ARG_UNUSED(fixture);

	/* A failed assertion must not leave a waiter running into the next test. */
	abort_live_threads();
}

ZTEST_SUITE(reservation, NULL, NULL, NULL, reservation_teardown, NULL);

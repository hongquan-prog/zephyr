/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 Process Mission
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/arch/cpu.h>
#include <zephyr/drivers/virtio/virtqueue.h>
#include <zephyr/irq_offload.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/printk.h>
#include <zephyr/ztest.h>

#include "virtio_common.h"
#include "fixture.h"

#define MAX_CHAIN         8
#define THREAD_STACK_SIZE 1024

static uint8_t chain_bufs[MAX_CHAIN][32] __aligned(4);

K_THREAD_STACK_DEFINE(release_thread_a_stack, THREAD_STACK_SIZE);
K_THREAD_STACK_DEFINE(release_thread_b_stack, THREAD_STACK_SIZE);
static struct k_thread thread_a;
static struct k_thread thread_b;

struct waiter {
	struct virtq *vq;
	uint16_t count;
	int ret;
	bool done;
	struct k_sem entered;
	struct k_sem done_sem;
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
	w->ret = submit_chain(w->vq, w->count, K_FOREVER, NULL, NULL);
	w->done = true;
	k_sem_give(&w->done_sem);
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

static void join_thread(struct k_thread *thread)
{
	if (k_thread_join(thread, K_MSEC(2000)) != 0) {
		k_thread_abort(thread);
	}

	track_stop(thread);
}

static void waiter_init(struct waiter *w, struct virtq *vq, uint16_t count)
{
	*w = (struct waiter){.vq = vq, .count = count};
	k_sem_init(&w->entered, 0, 1);
	k_sem_init(&w->done_sem, 0, 1);
}

static uint16_t last_head(const struct virtq *vq)
{
	return fixture_last_avail_head(vq);
}

static void complete_chain(const struct device *dev, uint16_t head)
{
	fixture_complete_isr(dev, head, 0);
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

static bool cb_called;

static void test_cb(void *opaque, uint32_t used_len)
{
	ARG_UNUSED(opaque);
	ARG_UNUSED(used_len);

	cb_called = true;
}

ZTEST(reservation_release, test_accounting_publication)
{
	struct virtq vq;
	uint32_t free_n;

	zassert_ok(virtq_create(&vq, 4));
	fixture_attach(fixture_dev_a(), &vq);

	zassert_ok(submit_chain(&vq, 2, K_NO_WAIT, NULL, NULL));
	complete_chain(fixture_dev_a(), last_head(&vq));

	if (CONFIG_MP_MAX_NUM_CPUS > 1) {
		/* On SMP only a locked reader gets a coherent count. */
		stack_data_t ids[8];
		k_spinlock_key_t key = k_spin_lock(&vq.lock);
		uint32_t popped = 0;
		stack_data_t desc;

		free_n = (uint32_t)atomic_get(&vq.free_desc_n);
		while (popped < ARRAY_SIZE(ids) &&
		       k_stack_pop(&vq.free_desc_stack, &desc, K_NO_WAIT) == 0) {
			ids[popped++] = desc;
		}
		for (uint32_t i = 0; i < popped; i++) {
			k_stack_push(&vq.free_desc_stack, ids[i]);
		}
		k_spin_unlock(&vq.lock, key);

		/* While locked, the count equals the stack depth. */
		zassert_equal(popped, free_n, "count and stack depth disagree");
	} else {
		/* The count is updated before the ISR returns, no wake work needed. */
		free_n = (uint32_t)atomic_get(&vq.free_desc_n);
	}

	zassert_equal(free_n, 4, "released descriptors are not visible after the ISR");

	/* Console-style flush: read the count, then submit without waiting. */
	zassert_ok(submit_chain(&vq, 2, K_NO_WAIT, NULL, NULL));
	complete_chain(fixture_dev_a(), last_head(&vq));

	fixture_attach(fixture_dev_a(), NULL);
	virtq_free(&vq);
}

ZTEST(reservation_release, test_bounded_release_validation)
{
	struct virtq vq;
	uint16_t head;
	uint16_t next1;
	uint16_t next2;
	uint32_t flags2;

	zassert_ok(virtq_create(&vq, 4));
	fixture_attach(fixture_dev_a(), &vq);

	cb_called = false;
	zassert_ok(submit_chain(&vq, 3, K_NO_WAIT, test_cb, NULL));
	head = last_head(&vq);
	next1 = sys_le16_to_cpu(vq.desc[head].next);
	next2 = sys_le16_to_cpu(vq.desc[next1].next);

	/* A link outside the descriptor table must be rejected as a whole. */
	vq.desc[head].next = sys_cpu_to_le16(vq.num + 2U);
	complete_chain(fixture_dev_a(), head);
	zassert_equal(atomic_get(&vq.free_desc_n), 1, "malformed chain was partially freed");
	zassert_false(cb_called, "callback ran for a rejected chain");
	zassert_equal(vq.last_used_idx, 1, "used index did not advance");

	/* A cycle that never terminates within the queue size is rejected too. */
	vq.desc[head].next = sys_cpu_to_le16(next1);
	flags2 = sys_le16_to_cpu(vq.desc[next2].flags);
	vq.desc[next2].flags = sys_cpu_to_le16((uint16_t)flags2 | VIRTQ_DESC_F_NEXT);
	vq.desc[next2].next = sys_cpu_to_le16(head);
	complete_chain(fixture_dev_a(), head);
	zassert_equal(atomic_get(&vq.free_desc_n), 1, "cyclic chain was partially freed");
	zassert_false(cb_called, "callback ran for a cyclic chain");
	zassert_equal(vq.last_used_idx, 2, "used index did not advance");

	/* Restore the chain and complete it: bounded cleanup before teardown. */
	vq.desc[next2].flags = sys_cpu_to_le16((uint16_t)flags2);
	vq.desc[next2].next = 0U;
	complete_chain(fixture_dev_a(), head);
	zassert_equal(atomic_get(&vq.free_desc_n), 4, "valid chain was not fully released");
	zassert_true(cb_called, "callback did not run for the valid chain");

	fixture_attach(fixture_dev_a(), NULL);
	virtq_free(&vq);
}

ZTEST(reservation_release, test_teardown_isolation)
{
	struct virtq va;
	struct virtq vb;
	struct waiter w;
	uint16_t pending;

	zassert_ok(virtq_create(&va, 2));
	zassert_ok(virtq_create(&vb, 2));
	fixture_attach(fixture_dev_a(), &va);
	fixture_attach(fixture_dev_b(), &vb);

	zassert_ok(submit_chain(&vb, 1, K_NO_WAIT, NULL, NULL));
	pending = last_head(&vb);

	waiter_init(&w, &vb, 2);
	start_thread(&thread_a, release_thread_a_stack,
		     K_THREAD_STACK_SIZEOF(release_thread_a_stack), waiter_entry, &w,
		     K_PRIO_PREEMPT(2));
	zassert_ok(k_sem_take(&w.entered, K_MSEC(500)));
	wait_for_waiters(&vb, 1, K_MSEC(500));
	zassert_equal(atomic_get(&vb.waiters), 1);
	k_sleep(K_MSEC(2));

	/*
	 * Hold B's slow-path mutex so the wake work submitted below stays
	 * pending while A is destroyed: a shared-queue drain in virtq_free()
	 * would have to wait for (or reject) another queue's notification.
	 */
	zassert_ok(k_mutex_lock(&vb.slow_lock, K_FOREVER));
	complete_chain(fixture_dev_b(), pending);

	/* Destroy the other queue: its own work item is flushed, nothing else. */
	fixture_attach(fixture_dev_a(), NULL);
	virtq_free(&va);

	k_mutex_unlock(&vb.slow_lock);

	zassert_ok(k_sem_take(&w.done_sem, K_MSEC(1000)),
		   "queue B did not wake after queue A was destroyed");
	zassert_ok(w.ret);
	join_thread(&thread_a);

	fixture_attach(fixture_dev_b(), NULL);
	virtq_free(&vb);
}

static int sys_submit_ret;
static struct k_sem sys_entered;
static struct k_sem sys_done;
static struct k_work sys_work;
static struct virtq *sys_vq;

static void sys_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	k_sem_give(&sys_entered);
	sys_submit_ret = submit_chain(sys_vq, 2, K_FOREVER, NULL, NULL);
	k_sem_give(&sys_done);
}

ZTEST(reservation_release, test_wake_queue_isolation_from_system_workqueue)
{
	struct virtq vq;
	struct waiter w;
	uint16_t busy;
	bool sys_done_taken = false;
	k_timepoint_t deadline;

	zassert_ok(virtq_create(&vq, 2));
	fixture_attach(fixture_dev_a(), &vq);

	zassert_ok(submit_chain(&vq, 1, K_NO_WAIT, NULL, NULL));
	busy = last_head(&vq);

	/* Block the system workqueue on a chain reservation. */
	sys_vq = &vq;
	k_sem_init(&sys_entered, 0, 1);
	k_sem_init(&sys_done, 0, 1);
	sys_submit_ret = -1;
	k_work_init(&sys_work, sys_work_handler);
	zassert_true(k_work_submit(&sys_work) >= 0, "work submission failed");
	zassert_ok(k_sem_take(&sys_entered, K_MSEC(1000)));

	waiter_init(&w, &vq, 2);
	start_thread(&thread_a, release_thread_a_stack,
		     K_THREAD_STACK_SIZEOF(release_thread_a_stack), waiter_entry, &w,
		     K_PRIO_PREEMPT(2));
	zassert_ok(k_sem_take(&w.entered, K_MSEC(500)));
	wait_for_waiters(&vq, 2, K_MSEC(1000));
	zassert_equal(atomic_get(&vq.waiters), 2);

	/*
	 * Freeing the held chain lets exactly one of the two waiters take both
	 * descriptors. The other one is only woken by the dedicated wake queue,
	 * even though a system workqueue handler is blocked on this queue.
	 */
	complete_chain(fixture_dev_a(), busy);

	/* Exactly one of the two waiters can win both descriptors. */
	deadline = sys_timepoint_calc(K_MSEC(2000));
	while (!sys_timepoint_expired(deadline) && !(w.done || sys_done_taken)) {
		if (!w.done && k_sem_take(&w.done_sem, K_NO_WAIT) == 0) {
			w.done = true;
		}
		if (!sys_done_taken && k_sem_take(&sys_done, K_NO_WAIT) == 0) {
			sys_done_taken = true;
		}
		k_sleep(K_MSEC(1));
	}

	/* Complete the winner's chain so the dedicated wake queue can wake the other. */
	if (w.done || sys_done_taken) {
		complete_chain(fixture_dev_a(), last_head(&vq));
	}

	deadline = sys_timepoint_calc(K_MSEC(2000));
	while (!sys_timepoint_expired(deadline) && !(w.done && sys_done_taken)) {
		if (!w.done && k_sem_take(&w.done_sem, K_NO_WAIT) == 0) {
			w.done = true;
		}
		if (!sys_done_taken && k_sem_take(&sys_done, K_NO_WAIT) == 0) {
			sys_done_taken = true;
		}
		k_sleep(K_MSEC(1));
	}

	zassert_true(w.done, "chain waiter did not complete");
	zassert_true(sys_done_taken, "system workqueue waiter did not complete");
	zassert_ok(w.ret);
	zassert_ok(sys_submit_ret);

	join_thread(&thread_a);
	fixture_attach(fixture_dev_a(), NULL);
	virtq_free(&vq);
}

ZTEST(reservation_release, test_mutual_blocking_stress)
{
	struct virtq vq;
	struct waiter a;
	struct waiter b;
	uint16_t seen = 0;
	k_timepoint_t deadline;

	zassert_ok(virtq_create(&vq, 2));
	fixture_attach(fixture_dev_a(), &vq);

	waiter_init(&a, &vq, 2);
	waiter_init(&b, &vq, 2);
	start_thread(&thread_a, release_thread_a_stack,
		     K_THREAD_STACK_SIZEOF(release_thread_a_stack), waiter_entry, &a,
		     K_PRIO_PREEMPT(2));
	zassert_ok(k_sem_take(&a.entered, K_MSEC(500)));
	start_thread(&thread_b, release_thread_b_stack,
		     K_THREAD_STACK_SIZEOF(release_thread_b_stack), waiter_entry, &b,
		     K_PRIO_PREEMPT(2));
	zassert_ok(k_sem_take(&b.entered, K_MSEC(500)));

	deadline = sys_timepoint_calc(K_MSEC(5000));
	while (!sys_timepoint_expired(deadline)) {
		uint16_t idx = sys_le16_to_cpu(vq.avail->idx);

		while (seen != idx) {
			uint16_t head = sys_le16_to_cpu(vq.avail->ring[seen % vq.num]);

			complete_chain(fixture_dev_a(), head);
			seen++;
		}

		if (k_sem_take(&a.done_sem, K_NO_WAIT) == 0) {
			a.done = true;
		}
		if (k_sem_take(&b.done_sem, K_NO_WAIT) == 0) {
			b.done = true;
		}
		if (a.done && b.done) {
			break;
		}

		k_sleep(K_MSEC(2));
	}

	zassert_true(a.done, "first 2-chain submitter did not complete");
	zassert_true(b.done, "second 2-chain submitter did not complete");
	zassert_ok(a.ret);
	zassert_ok(b.ret);
	zassert_equal(atomic_get(&vq.waiters), 0);

	join_thread(&thread_a);
	join_thread(&thread_b);
	fixture_attach(fixture_dev_a(), NULL);
	virtq_free(&vq);
}

static void mixed_lengths_case(uint16_t large_len)
{
	struct virtq vq;
	struct waiter w;
	uint16_t held[3];

	zassert_ok(virtq_create(&vq, 4));
	fixture_attach(fixture_dev_a(), &vq);

	/* One descriptor stays free for the stream, the large chain needs more. */
	for (int i = 0; i < 3; i++) {
		zassert_ok(virtq_get_free_desc(&vq, &held[i], K_NO_WAIT));
	}

	waiter_init(&w, &vq, large_len);
	start_thread(&thread_a, release_thread_a_stack,
		     K_THREAD_STACK_SIZEOF(release_thread_a_stack), waiter_entry, &w,
		     K_PRIO_PREEMPT(2));
	zassert_ok(k_sem_take(&w.entered, K_MSEC(500)));
	wait_for_waiters(&vq, 1, K_MSEC(500));
	zassert_equal(atomic_get(&vq.waiters), 1);

	/* Finite stream of one-descriptor chains while the large one waits. */
	for (int i = 0; i < 20; i++) {
		zassert_ok(submit_chain(&vq, 1, K_NO_WAIT, NULL, NULL));
		complete_chain(fixture_dev_a(), last_head(&vq));
		k_sleep(K_MSEC(1));
	}

	zassert_equal(k_sem_take(&w.done_sem, K_NO_WAIT), -EBUSY,
		      "large chain completed while the small stream was running");

	for (int i = 0; i < 3; i++) {
		virtq_add_free_desc(&vq, held[i]);
	}

	zassert_ok(k_sem_take(&w.done_sem, K_MSEC(2000)),
		   "large chain of %u did not complete after the stream drained", large_len);
	zassert_ok(w.ret);

	join_thread(&thread_a);
	fixture_attach(fixture_dev_a(), NULL);
	virtq_free(&vq);
}

ZTEST(reservation_release, test_mixed_chain_lengths)
{
	mixed_lengths_case(2);
	mixed_lengths_case(3);
	mixed_lengths_case(4);
}

static struct virtq_buf timing_bufs[MAX_CHAIN];

/*
 * Times complete API calls, not just the interrupt-disabled section: there is
 * no production hook around the locked region. For release the two walks are
 * inside the timed call; for reserve the descriptor programming is not.
 */
static void timing_case(uint16_t queue_size)
{
	struct virtq vq;
	uint32_t max_reserve = 0;
	uint32_t max_release = 0;

	zassert_ok(virtq_create(&vq, queue_size));
	fixture_attach(fixture_dev_a(), &vq);

	/*
	 * Chain lengths stay within the preallocated buffers; the locked
	 * sections are linear in N, which the 1..MAX_CHAIN sweep shows.
	 */
	for (uint16_t n = 1; n <= MAX_CHAIN; n++) {
		uint32_t min_reserve = UINT32_MAX;
		uint32_t min_release = UINT32_MAX;
		uint32_t hi_reserve = 0;
		uint32_t hi_release = 0;

		for (int i = 0; i < 100; i++) {
			uint32_t t0 = k_cycle_get_32();
			uint32_t t1;
			uint32_t t2;
			int ret;

			ret = virtq_add_buffer_chain(&vq, timing_bufs, n, 0, NULL, NULL, K_NO_WAIT);
			t1 = k_cycle_get_32();

			zassert_ok(ret);
			zassert_ok(virtq_release_chain(&vq, last_head(&vq)));
			t2 = k_cycle_get_32();

			min_reserve = MIN(min_reserve, t1 - t0);
			hi_reserve = MAX(hi_reserve, t1 - t0);
			min_release = MIN(min_release, t2 - t1);
			hi_release = MAX(hi_release, t2 - t1);
		}

		max_reserve = MAX(max_reserve, hi_reserve);
		max_release = MAX(max_release, hi_release);

		printk("MEASURE queue=%u chain=%u reserve min=%u max=%u release min=%u max=%u "
		       "cycles\n",
		       queue_size, n, min_reserve, hi_reserve, min_release, hi_release);
	}

	printk("MEASURE queue=%u summary reserve_max=%u release_max=%u cycles\n", queue_size,
	       max_reserve, max_release);
	zassert_true(max_reserve > 0 && max_release > 0, "no cycles measured");

	fixture_attach(fixture_dev_a(), NULL);
	virtq_free(&vq);
}

ZTEST(reservation_release, test_lock_duration_report)
{
	for (uint16_t i = 0; i < MAX_CHAIN; i++) {
		timing_bufs[i].addr = chain_bufs[i];
		timing_bufs[i].len = sizeof(chain_bufs[i]);
	}

	timing_case(8);
	timing_case(64);
	timing_case(256);
}

#if defined(CONFIG_SMP) && defined(CONFIG_SCHED_CPU_MASK)
K_THREAD_STACK_DEFINE(smp_probe_stack, 1024);
static struct k_thread smp_probe_thread;
static struct k_sem smp_probe_sem;

static void smp_probe_entry(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	k_sem_give(p1);
}
#endif

ZTEST(reservation_release, test_secondary_cpu_runs_threads)
{
#if defined(CONFIG_SMP) && defined(CONFIG_SCHED_CPU_MASK)
	k_tid_t tid;

	zassert_true(CONFIG_MP_MAX_NUM_CPUS >= 2, "SMP build has a single CPU");

	k_sem_init(&smp_probe_sem, 0, 1);
	tid = k_thread_create(&smp_probe_thread, smp_probe_stack,
			      K_THREAD_STACK_SIZEOF(smp_probe_stack), smp_probe_entry,
			      &smp_probe_sem, NULL, NULL, K_PRIO_PREEMPT(4), 0, K_FOREVER);
	zassert_ok(k_thread_cpu_pin(tid, 1), "could not pin the probe to CPU 1");
	k_thread_start(tid);

	zassert_ok(k_sem_take(&smp_probe_sem, K_MSEC(2000)), "CPU 1 is not running threads");
	zassert_ok(k_thread_join(tid, K_MSEC(2000)));
#else
	ztest_test_skip();
#endif
}

#if defined(CONFIG_SMP) && defined(CONFIG_SCHED_CPU_MASK)
K_THREAD_STACK_DEFINE(contention_stack, 1024);
K_THREAD_STACK_DEFINE(deadline_stack, 1024);
K_THREAD_STACK_DEFINE(orchestrator_stack, 1024);
static struct k_thread contention_thread;
static struct k_thread deadline_thread;
static struct k_thread orchestrator_thread;
static struct k_sem contention_ready;
static struct k_sem deadline_start;
static struct k_sem deadline_done;
static struct k_sem orchestrator_done;
static int64_t contention_end_ms;
static int64_t deadline_call_start_ms;
static int64_t deadline_elapsed_ms;
static int deadline_ret;

static void contention_entry(void *p1, void *p2, void *p3)
{
	struct k_spinlock *lock = p1;
	k_spinlock_key_t key;

	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	key = k_spin_lock(lock);
	k_sem_give(&contention_ready);
	k_busy_wait(800000);
	contention_end_ms = k_uptime_get();
	k_spin_unlock(lock, key);
}

/* Runs on CPU 0 so the fast attempt really spins while CPU 1 holds the lock. */
static void deadline_submitter(void *p1, void *p2, void *p3)
{
	struct virtq *vq = p1;
	struct virtq_buf one = {.addr = chain_bufs[0], .len = sizeof(chain_bufs[0])};

	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	k_sem_take(&deadline_start, K_FOREVER);
	deadline_call_start_ms = k_uptime_get();
	deadline_ret = virtq_add_buffer_chain(vq, &one, 1, 0, NULL, NULL, K_MSEC(500));
	deadline_elapsed_ms = k_uptime_get() - deadline_call_start_ms;
	k_sem_give(&deadline_done);
}

/*
 * Runs on CPU 0 and starts the submitter there; the ztest thread may otherwise
 * end up on CPU 1 and only run after the lock is already released.
 */
static void deadline_orchestrator(void *p1, void *p2, void *p3)
{
	struct virtq *vq = p1;
	k_tid_t contention;
	k_tid_t submitter;

	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	contention = k_thread_create(&contention_thread, contention_stack,
				     K_THREAD_STACK_SIZEOF(contention_stack), contention_entry,
				     &vq->lock, NULL, NULL, K_PRIO_PREEMPT(4), 0, K_FOREVER);
	(void)k_thread_cpu_pin(contention, 1);
	submitter = k_thread_create(&deadline_thread, deadline_stack,
				    K_THREAD_STACK_SIZEOF(deadline_stack), deadline_submitter, vq,
				    NULL, NULL, K_PRIO_PREEMPT(3), 0, K_FOREVER);
	(void)k_thread_cpu_pin(submitter, 0);

	k_thread_start(contention);
	(void)k_sem_take(&contention_ready, K_FOREVER);
	k_thread_start(submitter);
	k_sem_give(&deadline_start);
	(void)k_sem_take(&deadline_done, K_FOREVER);
	(void)k_thread_join(submitter, K_FOREVER);
	(void)k_thread_join(contention, K_FOREVER);

	k_sem_give(&orchestrator_done);
}
#endif

ZTEST(reservation_release, test_deadline_covers_nonblocking_attempt)
{
#if defined(CONFIG_SMP) && defined(CONFIG_SCHED_CPU_MASK)
	struct virtq vq;
	k_tid_t orchestrator;
	int64_t overlap;

	zassert_ok(virtq_create(&vq, 1));
	fixture_attach(fixture_dev_a(), &vq);
	zassert_ok(submit_chain(&vq, 1, K_NO_WAIT, NULL, NULL));

	k_sem_init(&contention_ready, 0, 1);
	k_sem_init(&deadline_start, 0, 1);
	k_sem_init(&deadline_done, 0, 1);
	k_sem_init(&orchestrator_done, 0, 1);

	orchestrator = k_thread_create(
		&orchestrator_thread, orchestrator_stack, K_THREAD_STACK_SIZEOF(orchestrator_stack),
		deadline_orchestrator, &vq, NULL, NULL, K_PRIO_PREEMPT(2), 0, K_FOREVER);
	zassert_ok(k_thread_cpu_pin(orchestrator, 0), "cannot pin the orchestrator");
	k_thread_start(orchestrator);

	zassert_ok(k_sem_take(&orchestrator_done, K_MSEC(3000)), "orchestrator did not finish");
	zassert_ok(k_thread_join(orchestrator, K_MSEC(1000)));

	printk("MEASURE deadline elapsed=%d start=%d end=%d\n", (int)deadline_elapsed_ms,
	       (int)deadline_call_start_ms, (int)contention_end_ms);

	/* The call must have started early enough that its 500 ms budget expires
	 * while the queue lock is still held, otherwise the case proves nothing.
	 */
	overlap = contention_end_ms - deadline_call_start_ms;
	zassert_true(overlap >= 600, "call did not overlap the lock (overlap %d ms)", (int)overlap);
	zassert_equal(deadline_ret, -EAGAIN, "returned %d", deadline_ret);
	zassert_true(deadline_elapsed_ms >= overlap - 100,
		     "returned before the lock was released: %d ms (overlap %d ms)",
		     (int)deadline_elapsed_ms, (int)overlap);
	zassert_true(
		deadline_elapsed_ms < overlap + 250,
		"budget restarted after the fast attempt: %d ms (overlap %d ms, budget 500 ms)",
		(int)deadline_elapsed_ms, (int)overlap);

	fixture_attach(fixture_dev_a(), NULL);
	virtq_free(&vq);
#else
	ztest_test_skip();
#endif
}

static void reservation_release_teardown(void *fixture)
{
	ARG_UNUSED(fixture);

	abort_live_threads();
}

ZTEST_SUITE(reservation_release, NULL, NULL, NULL, reservation_release_teardown, NULL);

/*
 * Copyright (c) 2026 Hongquan Li
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/arch/arch_interface.h>
#include <zephyr/drivers/interrupt_controller/intc_esp32.h>

#include <ipi.h>

#include <soc.h>
#include <esp_cpu.h>
#include <soc/interrupts.h>
#include <hal/crosscore_int_ll.h>

/*
 * ESP32-P4 cross-core interrupt used as scheduler IPI.
 *
 * Writing HP_SYSTEM_CPU_INT_FROM_CPU_x raises a level interrupt on core x;
 * ETS_FROM_CPU_INTR0_SOURCE targets core 0, ETS_FROM_CPU_INTR1_SOURCE
 * targets core 1. As in ESP-IDF's crosscore_int.c, each core routes and
 * enables its own source through the CLIC interrupt matrix, which
 * esp_intr_alloc() does for the calling core.
 */
static void esp32p4_crosscore_isr(void *arg)
{
	ARG_UNUSED(arg);

	/* Clear first so an IPI raised while handling is not lost. */
	crosscore_int_ll_clear_interrupt((int)esp_cpu_get_core_id());

	z_sched_ipi();
}

static int esp32p4_ipi_init_this_core(void)
{
	int source = (esp_cpu_get_core_id() == 0U) ? ETS_FROM_CPU_INTR0_SOURCE
						   : ETS_FROM_CPU_INTR1_SOURCE;

	return esp_intr_alloc(source, 0, esp32p4_crosscore_isr, NULL, NULL);
}

void arch_sched_directed_ipi(uint32_t cpu_bitmap)
{
	for (unsigned int cpu = 0; cpu < arch_num_cpus(); cpu++) {
		if ((cpu_bitmap & BIT(cpu)) != 0U) {
			crosscore_int_ll_trigger_interrupt((int)cpu);
		}
	}
}

int arch_smp_init(void)
{
	/* Runs on CPU0 during kernel init: install CPU0's IPI handler. */
	return esp32p4_ipi_init_this_core();
}

void soc_per_core_init_hook(void)
{
	/*
	 * Runs on every core. CPU0 cannot use it: there it is invoked from
	 * arch_kernel_init(), before the kernel heap that esp_intr_alloc()
	 * needs is initialized, so CPU0 is handled by arch_smp_init()
	 * instead. Secondary cores install their handler here.
	 */
	if (esp_cpu_get_core_id() != 0U) {
		int ret = esp32p4_ipi_init_this_core();

		__ASSERT(ret == 0, "CPU%u: failed to allocate cross-core interrupt (%d)",
			 (unsigned int)esp_cpu_get_core_id(), ret);
		ARG_UNUSED(ret);
	}
}

#ifdef CONFIG_FPU_SHARING
void arch_flush_fpu_ipi(void)
{
	/* TODO: implement FPU flush IPI for ESP32-P4 */
}

void arch_spin_relax(void)
{
	/* TODO: implement arch_spin_relax for ESP32-P4 */
}
#endif /* CONFIG_FPU_SHARING */

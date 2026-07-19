/*
 * Copyright (c) 2026 Hongquan Li
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/arch/arch_interface.h>

#ifdef CONFIG_SMP

void arch_sched_directed_ipi(uint32_t cpu_bitmap)
{
	ARG_UNUSED(cpu_bitmap);
	/* TODO: implement ESP32-P4 cross-core directed IPI */
}

int arch_smp_init(void)
{
	return 0;
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

#endif /* CONFIG_SMP */

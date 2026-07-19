/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/arch/arch_interface.h>
#include <zephyr/arch/riscv/csr.h>
#include <soc.h>
#include <esp_cpu.h>
#include <hal/cpu_utility_ll.h>

extern volatile uintptr_t riscv_cpu_wake_flag;
extern volatile uintptr_t riscv_cpu_boot_flag;
extern volatile void *riscv_cpu_sp;

extern volatile struct {
	arch_cpustart_t fn;
	void *arg;
} riscv_cpu_init[CONFIG_MP_MAX_NUM_CPUS];

extern char _vector_table[];
extern char _mtvt_table[];

void arch_secondary_cpu_init(int hartid);

/*
 * C portion of the CPU1 bring-up. Reached from
 * esp32p4_zephyr_secondary_entry() with a valid stack and global pointer.
 * Runs on CPU1 in M-mode.
 */
void __attribute__((section(".iram1"))) esp32p4_secondary_start(int hartid)
{
	/* Set up CLIC vectors for this core. */
	csr_write(mtvec, (uintptr_t)_vector_table);
	csr_write(0x307, (uintptr_t)_mtvt_table); /* mtvt */

	/* Clear the boot address so a later reset does not re-enter here. */
	ets_set_appcpu_boot_addr(0);

	/* Signal the boot CPU that we have taken our stack and vectors. */
	riscv_cpu_boot_flag = 1U;

	arch_secondary_cpu_init(hartid);
}

/*
 * CPU1 entry point. The ROM jumps here after arch_cpu_start() sets the
 * appcpu boot address and unstalled CPU1. Sets up the global pointer and
 * the per-CPU stack prepared by arch_cpu_start(), then continues in C.
 */
void __attribute__((naked, section(".iram1"))) esp32p4_zephyr_secondary_entry(void)
{
	__asm__ volatile(
		".option push\n"
		".option norelax\n"
		"la gp, __global_pointer$\n"
		".option pop\n"

		"csrw mie, zero\n"
		"csrci mstatus, 0x8\n"

		"la t0, riscv_cpu_sp\n"
		"lw sp, 0(t0)\n"

		"csrr a0, mhartid\n"
		"tail esp32p4_secondary_start\n"
		:
		:
		: "t0", "a0", "memory");
}

void arch_cpu_start(int cpu_num, k_thread_stack_t *stack, int sz,
		    arch_cpustart_t fn, void *arg)
{
	riscv_cpu_init[cpu_num].fn = fn;
	riscv_cpu_init[cpu_num].arg = arg;

	riscv_cpu_sp = K_KERNEL_STACK_BUFFER(stack) + sz;
	riscv_cpu_boot_flag = 0U;

	ets_set_appcpu_boot_addr((uint32_t)esp32p4_zephyr_secondary_entry);
	cpu_utility_ll_enable_clock_and_reset_app_cpu();
	esp_cpu_unstall(1);

	while (riscv_cpu_boot_flag == 0U) {
		riscv_cpu_wake_flag = _kernel.cpus[cpu_num].arch.hartid;
	}
}

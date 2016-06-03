/*
 * Copyright (c) 2016, Intel Corporation
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 * 3. Neither the name of the Intel Corporation nor the names of its
 *    contributors may be used to endorse or promote products derived from this
 *    software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE INTEL CORPORATION OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <string.h>
#include "mvic.h"
#include "idt.h"
#include "qm_interrupt.h"
#include "qm_pinmux.h"
#include "boot_clk.h"
#include "flash_layout.h"
#include "qm_gpio.h"
#include "power_states.h"

#if (SYSTEM_UPDATE_ENABLE)
#include "dm.h"
#endif

#if (DEBUG)
static QM_ISR_DECLARE(double_fault_isr)
{
	QM_ASSERT("Double Fault ISR\n");
	cpu_halt();
}
#endif

/*
 * Version number defines for the ROM
 */

/*
 * ROM Major version number
 */
#define QM_VER_ROM_MAJOR (1)

/*
 * ROM Minor version number
 */
#define QM_VER_ROM_MINOR (1)

/*
 * ROM Patch version number
 */
#define QM_VER_ROM_PATCH (0)

/* ROM version number combined */
static const unsigned int rom_version __attribute__((used))
__attribute__((section(".rom_version"))) =
    (QM_VER_ROM_MAJOR * 10000) + (QM_VER_ROM_MINOR * 100) + QM_VER_ROM_PATCH;

/* Lakemont application's entry point */
#define LMT_APP_ADDR (0x00180000)

/*
 * GPIO pin assigned for JTAG sensing.
 * On the Quark D2000 Development Platform, this is the IO0 pin.
 */
#define WAIT_FOR_JTAG_PIN (13)

/* Factory settings for Crystal Oscillator */
/* 7.45 pF load cap for Crystal */
#define OSC0_CFG1_OSC0_FADJ_XTAL_DEFAULT (0x4)
/* Crystal count value set to 5375 */
#define OSC0_CFG0_OSC0_XTAL_COUNT_VALUE_DEFAULT (0x2)

static QM_ISR_DECLARE(aonpt_spurious_isr)
{
	QM_ISR_EOI(QM_IRQ_AONPT_0_VECTOR);
}

/*
 * System power settings
 */
static __inline__ void power_setup(void)
{
	/* Pin MUX slew rate settings */
	/* By default, all pins are 12 mA, should be fine for now. */
}

/*
 * System clock settings
 */
static __inline__ void clock_setup(void)
{
	/* Apply factory settings for Crystal Oscillator stabilization
	 * These settings adjust the trimming value and the counter value
	 * for the Crystal Oscillator */
	QM_SCSS_CCU->osc0_cfg1 &= ~OSC0_CFG1_OSC0_FADJ_XTAL_MASK;
	QM_SCSS_CCU->osc0_cfg1 |=
	    (OSC0_CFG1_OSC0_FADJ_XTAL_DEFAULT << OSC0_CFG1_OSC0_FADJ_XTAL_OFFS);
	QM_SCSS_CCU->osc0_cfg0 &= ~OSC0_CFG0_OSC0_XTAL_COUNT_VALUE_MASK;
	QM_SCSS_CCU->osc0_cfg0 |= (OSC0_CFG0_OSC0_XTAL_COUNT_VALUE_DEFAULT
				   << OSC0_CFG0_OSC0_XTAL_COUNT_VALUE_OFFS);

	/*
	 * Switch to each silicon oscillator to set up trim data
	 * This sets up the trim codes for the first boot.
	 * This consists of computing the trim code if not available
	 * in non volatile memory and write this results in flash.
	 *
	 * This step is only performed if the shadow region
	 * is not populated.
	 * We rely on the 32MHz trim code to be shadowed to
	 * consider the region populated.
	 *
	 * This can be modified if this policy does not match your
	 * specific requirements.
	 */
	if ((QM_FLASH_DATA_TRIM_CODE->osc_trim_32mhz &
	     QM_FLASH_TRIM_PRESENT_MASK) != QM_FLASH_TRIM_PRESENT) {
		boot_clk_trim_code_setup();
	}

	/* Switch to 32MHz silicon oscillator */
	boot_clk_hyb_set_mode(CLK_SYS_HYB_OSC_32MHZ, CLK_SYS_DIV_1);
	clk_trim_apply(QM_FLASH_DATA_TRIM_CODE->osc_trim_32mhz);
}

/*
 * Initialize IRQ on AON peripheral so as to catch a spurious interrupt after a
 * warm reset.
 */
static __inline__ void aon_handle_spurious_irq(void)
{
	/* The PIC IRR register may be asserted by the application before a warm
	 * reset. IRR cannot be cleared by software, so let's just catch this
	 * single spurious interrupt. */
	qm_irq_request(QM_IRQ_AONPT_0, aonpt_spurious_isr);
}

/*
 * Ensure that early JTAG requests can be served.
 *
 * Quark Microcontroller D2000's JTAG pins can muxed with other functions, among
 * which UART1,
 * PWMs and gpios.  At POR JTAG pins are muxed out by default.  Once JTAG pins
 * are assigned to a different function, it is impossible to access the TAP
 * without a reset.  It is then essential that the JTAG TAP be accessed before
 * any pin muxing on its pins happens.  This is a race condition that can
 * effectively cause the board to get bricked.
 *
 * This function implements a boot ROM hook to guarantee enough time for a JTAG
 * access.
 * The Boot ROM will busy loop for as long as WAIT_FOR_JTAG_PIN is set to 0.
 */
static __inline__ void sense_jtag_probe(void)
{
	/* Hardware default (sticky):
	 * - clock gating:
	 *     - QM_CLK_PERIPH_REGISTER and QM_CLK_PERIPH_GPIO_REGISTER enabled
	 *     - QM_CLK_PERIPH_CLK disabled
	 * - pin muxing
	 *     - gpio's muxed out
	 *     - input pads enabled
	 *     - pad pullup disabled
	 * - gpio port configured as input
	 */

	qm_gpio_state_t state;

	qm_pmux_pullup_en(WAIT_FOR_JTAG_PIN, true);
	qm_pmux_select(WAIT_FOR_JTAG_PIN, QM_PMUX_FN_0);
	qm_pmux_input_en(WAIT_FOR_JTAG_PIN, true);
	clk_periph_enable(CLK_PERIPH_REGISTER | CLK_PERIPH_CLK |
			  CLK_PERIPH_GPIO_REGISTER);

	do {
		/* Busy loop to allow JTAG access */
		qm_gpio_read_pin(QM_GPIO_0, WAIT_FOR_JTAG_PIN, &state);
	} while (state == QM_GPIO_LOW);

	/* Restore hardware default settings */
	clk_periph_disable(CLK_PERIPH_CLK);
	qm_pmux_pullup_en(WAIT_FOR_JTAG_PIN, false);
};

/*
 * System update pin MUX settings
 */
static __inline__ void sys_update_pin_mux_setup(void)
{
#if (SYSTEM_UPDATE_VIA_SPI)
	/* SPI Slave*/
	qm_pmux_select(QM_PIN_ID_0, QM_PMUX_FN_2);
	qm_pmux_select(QM_PIN_ID_1, QM_PMUX_FN_2);
	qm_pmux_select(QM_PIN_ID_2, QM_PMUX_FN_2);
	qm_pmux_select(QM_PIN_ID_3, QM_PMUX_FN_2);

#elif(SYSTEM_UPDATE_VIA_UART0)
	/* UART 0*/
	qm_pmux_select(QM_PIN_ID_18, QM_PMUX_FN_0);
	qm_pmux_select(QM_PIN_ID_19, QM_PMUX_FN_0);

#elif(SYSTEM_UPDATE_VIA_UART1)
	/* UART 1*/
	qm_pmux_select(QM_PIN_ID_16, QM_PMUX_FN_2);
	qm_pmux_select(QM_PIN_ID_17, QM_PMUX_FN_2);
#endif
}

/*
 * SCSS interrupt routing initalization.
 */
static __inline__ void irq_setup(void)
{
	uint8_t i;

	/* Apply POR settings to SCSS int routing as SCSS regs are sticky */
	for (i = 0; i < QM_SCSS_INT_MASK_NUMREG; i++) {
		*((volatile uint32_t *)QM_SCSS_INT_BASE + i) =
		    QM_SCSS_INT_MASK_DEFAULT;
	}
}

static __inline__ void boot_services_setup()
{
}

/*
 * C runtime initialization.
 * This will be called from rom_startup.s
 */
void rom_startup(void)
{
	void (*app_entry)(void) = (void *)LMT_APP_ADDR;

	extern uint32_t __bss_start[];
	extern uint32_t __data_vma[];
	extern uint32_t __data_lma[];
	extern uint32_t __data_size[];
	extern uint32_t __bss_end[];

	/* Zero out bss */
	memset(__bss_start, 0x00, (uint32_t)__bss_end - (uint32_t)__bss_start);

	/* Copy initialised variables */
	memcpy(__data_vma, __data_lma, (size_t)__data_size);

	power_setup();
	clock_setup();

	sense_jtag_probe();

	/* Interrupt initialisation */
	irq_setup();
	idt_init();
	aon_handle_spurious_irq();
#if (DEBUG)
	qm_int_vector_request(QM_INT_VECTOR_DOUBLE_FAULT, double_fault_isr);
#endif
	mvic_init();
	__asm__ __volatile__("sti");
#if (SYSTEM_UPDATE_ENABLE)
	/* Check if the system update mode sticky bit is set */
	if (DM_STICKY_BIT_CHK()) {
		DM_STICKY_BIT_CLR();
		/* run the device management code; dm_main() never returns */
		dm_main();
	}
	/* Set up ISR to enter DM mode */
	dm_hook_setup();
#endif

	boot_services_setup();

	/* Execute application, provided that it has been programmed. */
	if (0xffffffff != *(uint32_t *)LMT_APP_ADDR) {
		app_entry();
	}
}

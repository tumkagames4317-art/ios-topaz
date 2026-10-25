#pragma once
#include <stdint.h>

/*
 * PMIC keys and reset paths, used by the panic park loop so a hung shim can be
 * driven back to fastboot or recovery without pulling the battery. See pmic.c.
 */
#define PMIC_KEY_NONE		0
#define PMIC_KEY_POWER		1	/* reboot (normal path) */
#define PMIC_KEY_VOL_DOWN	2	/* reboot and ask ABL for fastboot */

int pmic_init(void);
uint64_t pmic_wait_key(void);
void reboot_to_fastboot(void);
/* Reset after telling ABL where to go. reason 0x77665501 = fastboot. */
void reboot_now(uint64_t imem_reason);

/*
 * Arm the APCS watchdog so the board resets on its own if xnu hangs. Without it
 * a wedged xnu sits on the panel forever, the phone never returns to fastboot,
 * and every boot costs a manual key sequence -- which makes the flash/boot/
 * observe loop impossible to run unattended. Timeouts are in ticks of the
 * watchdog clock (19.2 MHz on SM6125): the defaults give roughly 30 s.
 */
void watchdog_arm(uint32_t bark_ticks, uint32_t bite_ticks);
/* Stop it again, e.g. if a later stage sets up its own. */
void watchdog_disarm(void);

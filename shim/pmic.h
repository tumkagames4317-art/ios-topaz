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

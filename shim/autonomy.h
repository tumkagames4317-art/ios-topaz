#pragma once
#include <stdint.h>

/*
 * Autonomous recovery paths for SM6125: make a wedged boot return to fastboot
 * without anyone pressing a key.
 *
 * See autonomy.c for why the fastboot request is TWO writes (PMIC
 * PON_SOFT_RB_SPARE and IMEM restart_reason) rather than the single IMEM store
 * the project used before, and why 0x77665500 is the fastboot code while
 * 0x77665501 is "unknown reason".
 */

/* enum pon_restart_reason */
#define PON_RESTART_REASON_RECOVERY	0x01U
#define PON_RESTART_REASON_BOOTLOADER	0x02U

/* IMEM restart_reason values (msm-poweroff.c) */
#define IMEM_RESTART_BOOTLOADER	0x77665500U
#define IMEM_RESTART_RECOVERY	0x77665502U
#define IMEM_RESTART_NORMAL	0x00000000U

/* Arm the APCS watchdog: bark then bite. Kept for completeness; the block is
 * gated on SM6125 and is not a usable reset path here. */
void watchdog_arm(uint32_t bark_ticks, uint32_t bite_ticks);
void watchdog_disarm(void);

/* Read the watchdog registers back and report whether our writes latch.
 * 1 = armed, 0 = gated, -1 = block does not decode. */
int wdt_selftest(void);

/* IMEM restart reason: set/read. Survives a warm reset. */
void imem_set_reason(uint32_t reason);
uint32_t imem_get_reason(void);

/*
 * Store the PON restart reason in the PMIC (bits [7:2] of PON_SOFT_RB_SPARE)
 * exactly as qpnp_pon_set_restart_reason() does. 1 = read back as written.
 */
int pon_set_restart_reason(uint32_t reason);

/*
 * Next boot goes to fastboot: PMIC reason + IMEM reason (both verified), then
 * PSCI SYSTEM_RESET with PS_HOLD as the fallback. Does not return on success.
 */
void autonomy_to_fastboot(void);

/*
 * Reboot the phone every `seconds` seconds, regardless of what the rest of the
 * shim is doing. Returns only if the elapsed time could not be measured.
 *
 * The APCS watchdog at 0x0f017000 does not produce a reset on SM6125 (measured:
 * armed, waited 105 s, board kept running), so this is a polled deadline on
 * CNTPCT_EL0 instead of a hardware watchdog. It is called from the one place in
 * the boot that cannot itself wedge, and it never gives up: when the deadline
 * passes it rewrites the IMEM reason and drops both PSCI and PS_HOLD.
 *
 * This is deliberately a *forced* reboot, not an "if stuck" reboot: the point
 * is that a wedge anywhere later in the boot still ends with the board cycling
 * instead of sitting there silently.
 */
void autonomy_reboot_loop(unsigned seconds);

/*
 * Start a core watchdog on a DIFFERENT CPU, then return.
 *
 * The boot CPU is about to be handed to xnu, which on ginkgo wedges and never
 * comes back (measured: 150 s with no panel change and no reset). Every
 * timeout that lives on the boot CPU dies with it: shim code is not running
 * any more, so autonomy_reboot_loop() there cannot help. The APCS watchdog
 * does not reset this SoC either. What does keep running is a core we start
 * ourselves, because xnu is never told about it.
 *
 * After `seconds` the secondary writes the fastboot reason and resets the
 * board, so a wedged xnu ends in fastboot like everything else.
 *
 * Returns 1 if a core accepted the handoff, 0 if none did (in which case
 * nothing guards the handoff and the caller should know).
 */
int watchdog_start_secondary(unsigned seconds);

/*
 * Print the SPMI write path's live state: arbiter version, the EE ownership
 * register, the APID resolved for the PMIC PON peripheral, and the channel's
 * STATUS/CMD words. This exists because every earlier attempt reported only
 * the RESULT of a write ("did not land") without showing whether the arbiter
 * had even accepted the command.
 */
void spmi_probe_report(void);

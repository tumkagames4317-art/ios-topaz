/*
 * Autonomous recovery: make every boot end in fastboot without a human.
 *
 * The requirement: press Power on a phone that has our image installed and it
 * comes back to fastboot on its own, every time. That has to hold for a boot
 * that wedges inside xnu, where nothing we control ever runs again.
 *
 * ---- Why the obvious recipe does not work ------------------------------
 *
 * The project has been writing IMEM restart_reason = 0x77665501 and resetting
 * via PSCI, expecting ABL to stop in fastboot. It does not: the phone reboots
 * and boots normally again, cycling every 30-60 s. Two separate defects:
 *
 * 1. WRONG CODE. From Qualcomm's msm-poweroff.c, the string ABL and the kernel
 *    agree on is "bootloader" -> 0x77665500. 0x77665501 is what the driver
 *    writes for any *unrecognised* reboot command. So we have never asked for
 *    fastboot at all; we have been asking for "restart, no particular reason".
 *
 * 2. WRONG (AND MISSING) REGISTER. msm-poweroff.c does not rely on IMEM alone.
 *    Alongside the IMEM store it calls qpnp_pon_set_restart_reason(), which
 *    writes PON_SOFT_RB_SPARE in the PMIC:
 *
 *        QPNP_PON_SOFT_RB_SPARE = pon_base + 0x8F
 *        qpnp_pon_masked_write(pon, base + 0x8F, GENMASK(7, 2), reason << 2)
 *
 *    PM6125 is a gen2 PON, so the reason sits in bits [7:2] of that byte. ABL
 *    reads the PMIC, not just IMEM; with the PMIC byte untouched the reason is
 *    UNKNOWN and ABL falls through to a normal boot. That is exactly the
 *    "IMEM reason is seen but ABL still boots Android" note in NOTES.md.
 *
 * So a correct "go to fastboot" is three writes, in this order:
 *
 *    PON_SOFT_RB_SPARE  <- PON_RESTART_REASON_BOOTLOADER (0x02) << 2
 *    IMEM restart_reason<- 0x77665500
 *    reset
 *
 * ---- Where the reset comes from ---------------------------------------
 *
 * PSCI SYSTEM_RESET (0x84000009 via `smc #0`) is the documented arm64 route
 * and needs no clock block. The APCS watchdog at 0x0f017000 is NOT usable: it
 * was tried before this session ("arming still ineffective") and measured
 * again here -- 105 s with no reset. The block is gated on SM6125.
 *
 * PSHOLD (qcom,pshold) is the last-resort path the mainline driver uses for
 * power-off: writing 0 there drops PS_HOLD and the PMIC power-cycles the
 * board. It is used only if PSCI returns, because it is a power cycle rather
 * than a warm reset.
 *
 * ---- Autonomy strategy ------------------------------------------------
 *
 * Never hand off into a state that can wedge without a way back. The loader
 * writes the reason pair (PMIC + IMEM) and resets, so the phone lands in
 * fastboot. When xnu is fixed enough to be worth watching, the same pair is
 * written before the jump and the watchdog/PSCI reset after it; until then the
 * fastboot landing is the whole point.
 */
#include "autonomy.h"
#include "console.h"
#include "ufslog.h"

/* ---- addresses ---------------------------------------------------------- */

/*
 * SPMI arbiter v5 observer window. The project already reads PON status here
 * (pmic.c: PON_RT_STS at 0x1c40000 + 0x800 + 0x10), so the same window is the
 * proven way to reach PM6125 registers with the MMU off.
 */
#define SPMI_ARB_BASE		0x01c40000UL
#define SPMI_OBS_SID0		0x800U

/* PM6125 PON peripheral base, as seen through the observer window. */
#define PON_BASE		(SPMI_ARB_BASE + SPMI_OBS_SID0)
#define QPNP_PON_SOFT_RB_SPARE	(PON_BASE + 0x8F)

#define IMEM_RESTART_REASON	0x0c12565cUL

/* PSHOLD: the always-on block whose reset line reaches the PMIC. */
#define PSHOLD_BASE		0x0440b000UL

#define PSCI_SYSTEM_RESET	0x84000009u
#define PSCI_SYSTEM_RESET2	0xc4000012u

/* enum pon_restart_reason from include/linux/input/qpnp-power-on.h */
#define PON_RESTART_REASON_RECOVERY	0x01
#define PON_RESTART_REASON_BOOTLOADER	0x02

/* IMEM values from drivers/power/reset/msm-poweroff.c */
#define IMEM_REASON_BOOTLOADER	0x77665500U
#define IMEM_REASON_RECOVERY	0x77665502U

#define REG32(a)	(*(volatile uint32_t *)(a))
#define REG8(a)		(*(volatile uint8_t *)(a))

/* ---- primitives --------------------------------------------------------- */

void imem_set_reason(uint32_t reason)
{
	REG32(IMEM_RESTART_REASON) = reason;
	__asm__ volatile("dsb sy" ::: "memory");
}

uint32_t imem_get_reason(void)
{
	return REG32(IMEM_RESTART_REASON);
}

/*
 * One PSCI call over `smc #0`.
 *
 * The constraint list is copied from shim.c, which is the version proven on
 * this board: the SMCCC lets firmware clobber x1-x17, so those registers must
 * be declared, and the arguments must actually be passed (a SYSTEM_RESET with
 * garbage in x1-x3 is not a well-formed call). Declaring only "memory" around
 * a bare `smc` let the compiler keep live values in x1-x17 across the call and
 * is very likely why the earlier resets did nothing.
 *
 * Returns x0, i.e. the PSCI status: 0 means the call was accepted.
 */
static uint64_t psci_reset(uint64_t fn)
{
	register uint64_t x0 __asm__("x0") = fn;
	register uint64_t x1 __asm__("x1") = 0;
	register uint64_t x2 __asm__("x2") = 0;
	register uint64_t x3 __asm__("x3") = 0;
	__asm__ volatile("smc #0"
			 : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3)
			 :
			 : "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11",
			   "x12", "x13", "x14", "x15", "x16", "x17", "memory");
	return x0;
}

/*
 * Write the PON restart reason the way qpnp_pon_set_restart_reason() does for
 * a gen2 PON: read the byte, clear bits [7:2], OR in reason << 2, write back.
 *
 * BEST EFFORT. The SPMI arbiter only answers once the RPM is up -- the project
 * proved keys readable inside xnu and not from the shim (pmic.c, and
 * NOTES-repo patch 0060) -- so with the MMU off this read-back most often
 * fails. It is still attempted because it is free and correct when it works,
 * but a failure here must never block the reset: IMEM is the channel that does
 * work at this stage, so the decision to reset is taken on IMEM alone.
 *
 * Returns 1 if the value read back as written, 0 otherwise.
 */
int pon_set_restart_reason(uint32_t reason)
{
	uint8_t before = REG8(QPNP_PON_SOFT_RB_SPARE);
	uint8_t val = (uint8_t)((before & 0x03u) | ((reason << 2) & 0xFCu));

	REG8(QPNP_PON_SOFT_RB_SPARE) = val;
	__asm__ volatile("dsb sy" ::: "memory");

	uint8_t after = REG8(QPNP_PON_SOFT_RB_SPARE);
	con_hex2("PONRB", "was=", before, "now=", after);
	return after == val;
}

/*
 * Next boot goes to fastboot. Does not return unless every write is refused.
 *
 * The order is deliberate: the PMIC byte first (ABL consults it), then IMEM
 * (the channel that does work at this stage), then reset.
 *
 * Only IMEM decides whether to reset. The PMIC write is best effort: the SPMI
 * arbiter needs the RPM, which is not up before xnu runs, so requiring it
 * would park every boot on a path that cannot succeed yet. IMEM is reachable
 * with the MMU off and its read-back is a real check, so a failed IMEM store
 * means the reset would be pointless and is reported instead.
 */
void autonomy_to_fastboot(void)
{
	int pon_ok = pon_set_restart_reason(PON_RESTART_REASON_BOOTLOADER);
	imem_set_reason(IMEM_REASON_BOOTLOADER);
	uint32_t imem = imem_get_reason();

	con_hex2("AUTO", "pon=", pon_ok, "imem=", imem);
	ufslog_hex2("AUTO", "pon=", pon_ok, "imem=", imem);

	if (imem != IMEM_REASON_BOOTLOADER) {
		/*
		 * IMEM did not take the value. Resetting anyway would produce a
		 * normal boot -- another trip round the loop that hides the
		 * failure. Park and leave the panel explaining why.
		 */
		con_str("AUTO", "IMEM refused: parked");
		ufslog_str("AUTO", "IMEM refused");
		ufslog_flush();
		for (;;)
			__asm__ volatile("wfi");
	}

	con_str("AUTO", "PSCI reset -> fastboot");
	ufslog_str("AUTO", "PSHOLD/PSCI reset to fastboot");
	ufslog_flush();

	psci_reset(PSCI_SYSTEM_RESET);

	/*
	 * PSCI refused. Try SYSTEM_RESET2 as well before falling back to PS_HOLD:
	 * some firmware implements only the newer call.
	 */
	con_str("AUTO", "SYSTEM_RESET refused; trying SYSTEM_RESET2");
	psci_reset(PSCI_SYSTEM_RESET2);

	/* PSCI refused both: drop PS_HOLD, which power-cycles the board. */
	con_str("AUTO", "PSCI refused; dropping PS_HOLD");
	REG32(PSHOLD_BASE) = 0;
	__asm__ volatile("dsb sy" ::: "memory");

	con_str("AUTO", "PSHOLD did not reset: parked");
	ufslog_str("AUTO", "no reset path available");
	ufslog_flush();
	for (;;)
		__asm__ volatile("wfi");
}

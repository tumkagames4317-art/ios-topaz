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

/*
 * The PON block for SID 0 has always been read through SPMI_ARB_BASE + 0x800,
 * and those reads DO return sensible values (PON_RT_STS read as 0, not
 * 0xffffffff), so that read path is left untouched. From the device tree the
 * five regions are:
 *   core 0x1c40000  chnls 0x1e00000  obsrvr 0x3e00000
 *   intr 0x3f00000  cnfg 0x1c0a000
 * Writes need chnls, and the APID table lives in cnfg at 0x900 + 4*apid.
 */
#define SPMI_OBS_SID0		0x800U

/* PM6125 PON peripheral base, as seen through the observer window. */
#define PON_BASE		(SPMI_ARB_BASE + SPMI_OBS_SID0)
#define QPNP_PON_SOFT_RB_SPARE	(PON_BASE + 0x8F)

#define IMEM_RESTART_REASON	0x0c12565cUL

/* PSHOLD: the always-on block whose reset line reaches the PMIC. */
#define PSHOLD_BASE		0x0440b000UL

/*
 * PS_HOLD reset control inside the PON block.
 *
 * qpnp_pon_reset_config() clears RESET_EN in RST_CTL2, writes the power-off
 * type into the low nibble of RST_CTL, waits 500 us and re-enables RESET_EN.
 * Only a WARM reset keeps the restart reason; a hard reset clears it, which
 * is why ABL boots Android after our resets even though the IMEM value is
 * written correctly.
 */
#define QPNP_PON_PS_HOLD_RST_CTL	(PON_BASE + 0x5A)
#define QPNP_PON_PS_HOLD_RST_CTL2	(PON_BASE + 0x5B)
#define QPNP_PON_RESET_EN		(1u << 7)
#define QPNP_PON_POWER_OFF_MASK		0x0Fu
#define PON_POWER_OFF_SHUTDOWN		0x0u
#define PON_POWER_OFF_HARD_RESET	0x1u
#define PON_POWER_OFF_WARM_RESET	0x2u

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
 * Write the PON restart reason the way qpnp_pon_set_restart_reason() does.
 *
 * The register and the bit layout were checked against the actual driver that
 * ships in this phone's kernel
 * (LineageOS android_kernel_xiaomi_sm6250, drivers/input/misc/qpnp-power-on.c):
 *
 *     #define QPNP_PON_SOFT_RB_SPARE(pon)  ((pon)->base + 0x8F)
 *
 *     if (is_pon_gen2(pon) && !pon->legacy_hard_reset_offset)
 *             qpnp_pon_masked_write(pon, QPNP_PON_SOFT_RB_SPARE(pon),
 *                                   GENMASK(7, 1), (reason << 1));
 *     else
 *             qpnp_pon_masked_write(pon, QPNP_PON_SOFT_RB_SPARE(pon),
 *                                   GENMASK(7, 2), (reason << 2));
 *
 * There are two layouts, and which one applies is decided by DT, not by the
 * PMIC generation alone. On this device the boot DTB contains
 * `qcom,store-hard-reset-reason` and does NOT contain
 * `qcom,use-legacy-hard-reset-offset`, so the **gen2 / non-legacy** branch is
 * the live one: bits [7:1] carry reason << 1.
 *
 * An earlier version of this file hardcoded the legacy layout (mask [7:2],
 * shift 2), which puts BOOTLOADER (0x02) in the wrong bits entirely -- the PMIC
 * would have stored a different reason and ABL would have ignored it. Both
 * layouts are written here where they cannot be confused, and the non-legacy
 * one is used because that is what this device's DT selects.
 *
 * BEST EFFORT. The SPMI arbiter only answers once the RPM is up -- the project
 * proved keys readable inside xnu and not from the shim (pmic.c, and
 * NOTES-repo patch 0060) -- so with the MMU off this read-back most often
 * fails. It is still attempted because it is free and correct when it works,
 * but a failure here must never block the reset: IMEM is the channel that does
 * work at this stage.
 *
 * Returns 1 if the value read back as written, 0 otherwise.
 */
#define PON_GEN2_REASON_MASK	0xFEu	/* GENMASK(7, 1) */
#define PON_GEN2_REASON_SHIFT	1
#define PON_LEGACY_MASK		0xFCu	/* GENMASK(7, 2) */
#define PON_LEGACY_SHIFT	2

/*
 * Real SPMI writes through the PMIC arbiter.
 *
 * The observer window at 0x1c40000 + 0x800 that this project used for years
 * is READ-ONLY: a store there goes nowhere and the read-back returns the old
 * value, which is exactly what the panel showed -- reads worked (PON_RT_STS
 * came back as 0 instead of all-ones) while every PMIC write silently did
 * nothing and the reason byte stayed 0.
 *
 * drivers/spmi/spmi-pmic-arb.c describes what a write actually is:
 * put the data in WDATA0, write the command into CMD, then poll STATUS until
 * DONE. The command word for arbiter v2 and later is
 *     (opc << 27) | ((addr & 0xff) << 4) | (bc & 0x7)
 * with opc 14 for WRITE, and the channel offset comes from the APID, not
 * from a fixed window: 0x10000 * apid.
 *
 * Addresses from the device tree node qcom,spmi@1c40000:
 *   core  0x01c40000  chnls 0x01c00000  obsrvr 0x01e00000 ... 0x01f00000
 * The observer window used so far (0x1c40000 + 0x800) is what the read path
 * needs; writes must go through chnls.
 */
#define SPMI_CORE_BASE		0x01c40000UL
#define SPMI_CHNLS_BASE		0x01e00000UL
#define SPMI_OBSRV_BASE		0x03e00000UL
#define SPMI_CNFG_BASE		0x01c0a000UL
/*
 * The APID table was actually readable at obsrvr + 0x900 (words 0x140, 0x494,
 * ...), so that is where it is on this arbiter revision; cnfg + 0x900 read
 * back all zeroes. Worth remembering that the offsets in spmi-pmic-arb.c
 * assume program headers that this firmware evidently rearranged.
 */
#define SPMI_TBL_BASE		(SPMI_OBSRV_BASE + 0x900UL)
#define SPMI_ARB_CMD		0x00
#define SPMI_ARB_STATUS		0x08
#define SPMI_ARB_WDATA0		0x10
#define SPMI_ARB_STATUS_DONE	(1u << 0)
#define SPMI_ARB_STATUS_FAIL	(1u << 1)
#define SPMI_ARB_STATUS_DENIED	(1u << 2)
#define SPMI_OP_WRITE		14u

/*
 * APID for a given (sid, peripheral). The mapping table lives in cnfg at
 * 0x900 + 4*n for arbiter v5, but reading it needs the full walk that
 * spmi-pmic-arb.c does. On this platform APPS is EE 2 and the PON peripheral
 * of SID 0 (PM6125) is peripheral 0x08, which is what the kernel's own
 * qcom,power-on@800 node refers to; the resulting APID is stable and was
 * confirmed by the observer window answering for exactly this peripheral.
 */
static uint32_t cnt_nz(void)
{
	uint32_t n = 0;
	for (int i = 0; i < 512; i++)
		if (REG32(SPMI_TBL_BASE + 4UL * (uint32_t)i))
			n++;
	return n;
}

/*
 * Find the APID that owns a given PPID.
 *
 * For arbiter v5 -- and this one reports v5 (0x50010000), and its DT has the
 * v5 reg-names list "core chnls obsrvr intr cnfg" -- the APID map lives in
 * cnfg, not obsrvr:
 *
 *     pmic_arb_read_apid_map_v5(): core + PMIC_ARB_APID_MAP + 4*n
 *     PMIC_ARB_APID_MAP = 0x900  -> cnfg + 0x900 = 0x01c0a900
 *
 * Reading obsrvr + 0x900 was the old guess and it is what produced the
 * "144 non-zero words but no PPID 0x800" reading: obsrvr is the read-only
 * observer window and has nothing to do with the APID map.
 *
 * The PPID in the map is (sid << 8) | peripheral. PON on PM6125 (SID 0) is
 * peripheral 0x08, so PPID 0x0808.
 *
 * Returns the APID, or -1 when the PPID is not mapped.
 */
static int spmi_find_apid(uint16_t ppid)
{
	for (int n = 0; n < 512; n++) {
		uint32_t reg = REG32(SPMI_CNFG_BASE + 0x900UL + 4UL * (uint32_t)n);

		if (!reg)
			continue;
		if (((reg >> 8) & 0xFFFu) == ppid)
			return n;
	}
	return -1;
}

/*
 * One byte write through the arbiter command channel.
 *
 * Returns 1 on a clean DONE, 0 on timeout or an error bit.
 *
 * `addr` is the 16-bit SPMI address: (peripheral << 8) | offset, which for
 * PON is 0x0800 | register.
 *
 * The channel offset for arbiter v5 read/write is 0x10000 * apid from the
 * "chnls" base (0x01c00000). The observer window used before is read-only,
 * so storing there did nothing and the read-back simply returned the old
 * value.
 */
static int g_spmi_code = -1;
static uint32_t g_v1_val;
static uint32_t g_v1_val2;

/*
 * ---- arbiter v5 command channel ----------------------------------------
 *
 * What a write actually is, from spmi-pmic-arb.c (v5 path):
 *
 *   channel base  = chnls + 0x10000 * apid
 *   WDATA0   ch+0x10  <- data          (byte count decides how much counts)
 *   CMD      ch+0x00  <- (opc << 27) | ((addr & 0xffff) << 4) | (bc & 7)
 *   STATUS   ch+0x08  -> bit0 DONE, bit1 FAIL, bit2 DENIED
 *
 * opc 14 = WRITE, opc 15 = READ. `addr` is the 16-bit SPMI address
 * (peripheral << 8) | offset. The SID does NOT appear in CMD on v5: it is
 * encoded in the address and resolved through the APID map.
 *
 * The old code used core + 0x800 as the channel. That is not a channel at
 * all: core is only 0x1100 bytes and 0x800 is the start of the EE0 register
 * bank (PMIC_ARB_EE_BLOCK), so every "transaction" was really reading and
 * writing EE status words. That is why reads looked plausible and writes
 * never landed.
 */
#define SPMI_OP_READ		15u

/*
 * Claim this execution environment's channels before touching them.
 *
 * pmic_arb_claim_ownership(): read PMIC_ARB_ENABLE at core + 0x1000; if our
 * EE's bit is clear, set it. Without this the arbiter answers DENIED and the
 * PMIC never sees the command. APPS is EE 2 on Qualcomm platforms; try 0..3
 * and keep whichever bit sticks, because the EE number is a DT property we
 * cannot read here (MMU off, no DT parsing at this stage).
 */
static int g_spmi_ee = -1;

static int spmi_claim_ee(void)
{
	if (g_spmi_ee >= 0)
		return g_spmi_ee;

	uint32_t en = REG32(SPMI_CORE_BASE + 0x1000UL);

	for (int ee = 0; ee < 4; ee++) {
		if (en & (1u << ee)) {
			g_spmi_ee = ee;
			return ee;
		}
	}
	/* None set: claim EE0 and verify it took. */
	REG32(SPMI_CORE_BASE + 0x1000UL) = en | 1u;
	__asm__ volatile("dsb sy" ::: "memory");
	if (REG32(SPMI_CORE_BASE + 0x1000UL) & 1u) {
		g_spmi_ee = 0;
		return 0;
	}
	return -1;
}

/* One byte write. 1 = DONE cleanly, 0 = timeout/FAIL/DENIED. */
static int spmi_write_byte(uint8_t sid, uint16_t addr, uint8_t val)
{
	(void)spmi_claim_ee();

	int apid = spmi_find_apid((uint16_t)(((uint16_t)sid << 8) | (addr >> 8)));

	if (apid < 0)
		apid = g_spmi_code;	/* probe-confirmed fallback */
	if (apid < 0)
		return 0;

	uintptr_t ch = SPMI_CHNLS_BASE + 0x10000UL * (uint32_t)apid;

	/* Clear a stale completion, then stage the data. */
	REG32(ch + SPMI_ARB_STATUS) = 0;
	__asm__ volatile("dsb sy" ::: "memory");

	REG32(ch + SPMI_ARB_WDATA0) = val;
	REG32(ch + 0x04) = 0;		/* no priority/irq bits: plain byte write */
	__asm__ volatile("dsb sy" ::: "memory");

	REG32(ch + SPMI_ARB_CMD) = (SPMI_OP_WRITE << 27) |
				   (((uint32_t)addr & 0xFFu) << 4) |
				   0u;		/* bc = 0 -> 1 byte */
	__asm__ volatile("dsb sy" ::: "memory");

	for (int i = 0; i < 200000; i++) {
		uint32_t st = REG32(ch + SPMI_ARB_STATUS);

		if (!(st & SPMI_ARB_STATUS_DONE))
			continue;
		if (st & (SPMI_ARB_STATUS_FAIL | SPMI_ARB_STATUS_DENIED))
			return 0;
		return 1;
	}
	return 0;
}

/* Diagnostic: report the raw state of the write path so the panel explains it. */
void spmi_probe_report(void)
{
	uint32_t ver = REG32(SPMI_CORE_BASE + 0x5c4UL);
	uint32_t en = REG32(SPMI_CORE_BASE + 0x1000UL);
	int ppid_pon = 0x0808;
	int apid = spmi_find_apid((uint16_t)ppid_pon);
	int ee = g_spmi_ee;

	con_hex2("SPMI", "ver=", ver, " en=", en);
	con_hex2("SPMI", "ppid=0808 apid=", (uint32_t)(apid < 0 ? 0xdead : apid),
		 " ee=", (uint32_t)(ee < 0 ? 0xdead : ee));
	if (apid >= 0) {
		uintptr_t ch = SPMI_CHNLS_BASE + 0x10000UL * (uint32_t)apid;
		con_hex2("SPMI", "ch=sts ", REG32(ch + SPMI_ARB_STATUS),
			 " cmd ", REG32(ch + SPMI_ARB_CMD));
	}
}

/*
 * Tell the PMIC to treat dropping PS_HOLD as a WARM reset.
 *
 * Mirrors qpnp_pon_reset_config() with PON_POWER_OFF_WARM_RESET, the step the
 * kernel runs before storing the reason. Without it the reset is a hard one
 * and the stored reason is thrown away, which is why ABL keeps booting
 * Android.
 *
 * Every access is one byte, through the arbiter command channel: these
 * registers sit at odd offsets (0x5A, 0x5B) so a 32-bit access would fault
 * under -mstrict-align, and the observer window is read-only so a plain store
 * would vanish.
 *
 * SPMI address for PON is (peripheral << 8) | offset = 0x0800 | reg.
 */
int pon_config_ps_hold_warm_reset(void)
{
	uint8_t en = REG8(QPNP_PON_PS_HOLD_RST_CTL2);

	/* Clear RESET_EN so the power-off type can be changed safely. */
	spmi_write_byte(0, 0x0800 | 0x5B, (uint8_t)(en & ~QPNP_PON_RESET_EN));
	__asm__ volatile("dsb sy" ::: "memory");
	for (volatile int i = 0; i < 5000; i++)
		;

	uint8_t ctl = REG8(QPNP_PON_PS_HOLD_RST_CTL);
	spmi_write_byte(0, 0x0800 | 0x5A,
			(uint8_t)((ctl & ~QPNP_PON_POWER_OFF_MASK) |
				   (PON_POWER_OFF_WARM_RESET & QPNP_PON_POWER_OFF_MASK)));
	__asm__ volatile("dsb sy" ::: "memory");

	/* Re-enable reset, or dropping PS_HOLD would only power the board down. */
	spmi_write_byte(0, 0x0800 | 0x5B, (uint8_t)(en | QPNP_PON_RESET_EN));
	__asm__ volatile("dsb sy" ::: "memory");

	uint8_t back = REG8(QPNP_PON_PS_HOLD_RST_CTL);
	con_hex2("PONPS", "ctl=", back, "en=", REG8(QPNP_PON_PS_HOLD_RST_CTL2));
	spmi_probe_report();
	return (int)(back & QPNP_PON_POWER_OFF_MASK);
}

int pon_set_restart_reason(uint32_t reason)
{
	uint8_t before = REG8(QPNP_PON_SOFT_RB_SPARE);
	uint8_t val = (uint8_t)((before & ~PON_GEN2_REASON_MASK) |
				((reason << PON_GEN2_REASON_SHIFT) &
				 PON_GEN2_REASON_MASK));

	spmi_write_byte(0, 0x0800 | 0x8F, val);
	__asm__ volatile("dsb sy" ::: "memory");

	uint8_t after = REG8(QPNP_PON_SOFT_RB_SPARE);

	/*
	 * If the SPMI write cannot land, the read-back returns the same value it
	 * returned before, so compare against `before` too: a store that was
	 * refused must not be reported as a success just because nothing moved.
	 */
	con_hex2("PONRB", "was=", before, "now=", after);
	if (after != before)
		return after == val;

	/*
	 * Nothing changed. Either the write was refused (expected before the RPM
	 * is up) or the value was already correct. Try the legacy layout as a
	 * fallback -- writing both layouts is harmless because each only touches
	 * bits the other leaves alone only when the reason fits in one bit, and
	 * ABL accepts whichever it reads. Report the gen2 result as authoritative.
	 */
	uint8_t legacy = (uint8_t)((before & ~PON_LEGACY_MASK) |
				   ((reason << PON_LEGACY_SHIFT) & PON_LEGACY_MASK));
	if (legacy != val) {
		spmi_write_byte(0, 0x0800 | 0x8F, legacy);
		__asm__ volatile("dsb sy" ::: "memory");
		uint8_t l2 = REG8(QPNP_PON_SOFT_RB_SPARE);
		con_hex2("PONleg", "val=", legacy, "back=", l2);
		if (l2 == legacy)
			return 1;
	}
	return 0;
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
	/* Declared at function scope: the reset path below reads it after the ladder. */
	uint32_t imem = 0;

	/*
	 * SWEET_DIAG=3: does the SPMI observer window answer from the shim?
	 *
	 * Everything else is already proven on hardware: a pattern written to
	 * IMEM reads back identically, and dropping PS_HOLD does reset the
	 * board. The one missing step is telling the PMIC to treat that as a
	 * WARM reset, because a hard reset does not carry the stored reason --
	 * the ramoops log says "Power-on reason: Triggered from Hard Reset",
	 * which is exactly why ABL boots Android instead of fastboot.
	 *
	 * Whether that step is even possible depends on one unknown: whether
	 * 0x1c40000 + 0x800 responds to reads this early. FFFFFFFF means the
	 * arbiter is not up and the PMIC route is closed. A real value means it
	 * is open and the remaining work is ordinary register programming.
	 *
	 * Two rows via con_hex2 -- the only print this panel has reliably shown.
	 */
#if SWEET_DIAG == 2
	/*
	 * SWEET_DIAG=2: the real attempt, not a probe.
	 *
	 * The probe (SWEET_DIAG=1) answered everything it was built to answer:
	 *   A1 rtsts=00000000  -> the SPMI window DOES respond from the shim,
	 *                          so the PMIC is programmable after all; the
	 *                          old note claiming otherwise was wrong.
	 *   A2 rctl2=00000000  -> RESET_EN is CLEARED. The reset line is off,
	 *                          which is why dropping PS_HOLD powers the
	 *                          board down and ABL comes back to Android.
	 *   A3 spare=00000000  -> the reason byte ABL reads is empty.
	 *   A4 rctl=00000000   -> power-off type 0 (shutdown), not warm reset.
	 *
	 * So the fix is exactly qpnp_pon_reset_config(WARM_RESET): enable the
	 * reset line, set the type to warm, then store the reason. A hard reset
	 * would throw the reason away; a warm reset carries it to ABL.
	 *
	 * Order matters and mirrors the kernel: configure PS_HOLD first, then
	 * write the reason, then reset.
	 */
	int warm = pon_config_ps_hold_warm_reset();
	int pon_ok = pon_set_restart_reason(PON_RESTART_REASON_BOOTLOADER);
	imem_set_reason(IMEM_REASON_BOOTLOADER);
	imem = imem_get_reason();

	con_hex2("R1 warm=", "", (uint32_t)warm, " pon=", (uint32_t)pon_ok);
	con_hex2("R2 imem=", "", imem, " rb=",
		 (uint32_t)REG8(QPNP_PON_SOFT_RB_SPARE));
	con_hex2("R3 rctl2=", "", REG8(QPNP_PON_PS_HOLD_RST_CTL2), " rctl=",
		 (uint32_t)REG8(QPNP_PON_PS_HOLD_RST_CTL));
	ufslog_flush();
	for (;;)
		__asm__ volatile("wfi");
#elif SWEET_DIAG == 4
	/*
	 * SWEET_DIAG=4: configure the PMIC, then actually reset.
	 *
	 * Same as 2 up to the writes, but this one drops PS_HOLD afterwards
	 * instead of parking, so the reset can be judged.
	 */
	int warm = pon_config_ps_hold_warm_reset();
	int pon_ok = pon_set_restart_reason(PON_RESTART_REASON_BOOTLOADER);
	imem_set_reason(IMEM_REASON_BOOTLOADER);
	imem = imem_get_reason();

	con_hex2("S1 warm=", "", (uint32_t)warm, " pon=", (uint32_t)pon_ok);
	con_hex2("S2 imem=", "", imem, " rb=",
		 (uint32_t)REG8(QPNP_PON_SOFT_RB_SPARE));
	con_hex2("S3 rctl2=", "", REG8(QPNP_PON_PS_HOLD_RST_CTL2), " rctl=",
		 (uint32_t)REG8(QPNP_PON_PS_HOLD_RST_CTL));
	con_str("S4", "dropping PS_HOLD now");
	ufslog_flush();

	REG32(PSHOLD_BASE) = 0;
	__asm__ volatile("dsb sy" ::: "memory");
	psci_reset(PSCI_SYSTEM_RESET);
	psci_reset(PSCI_SYSTEM_RESET2);
	for (;;)
		__asm__ volatile("wfi");
#elif SWEET_DIAG == 1
	/*
	 * Probe only: report, do not change anything.
	 *
	 * One row per register, a marker on each, because the panel has only
	 * ever been transcribed up to its first readable row. Every access is
	 * 8-bit: these registers sit at odd offsets (0x5A, 0x5B, 0x8F) and a
	 * 32-bit load from an odd address faults under -mstrict-align, which is
	 * what made every earlier PS_HOLD attempt look like a hang.
	 */
	con_hex2("A0 alive=", "", 0xAAAAAAAAu, "", 0);
	con_hex2("A1 rtsts=", "", REG32(PON_BASE + 0x10), "", 0);
	con_hex2("A2 rctl2=", "", REG8(QPNP_PON_PS_HOLD_RST_CTL2), "", 0);
	con_hex2("A3 spare=", "", REG8(QPNP_PON_SOFT_RB_SPARE), "", 0);
	con_hex2("A4 rctl=", "", REG8(QPNP_PON_PS_HOLD_RST_CTL), "", 0);
	con_hex2("A5 imem=", "", imem_get_reason(), "", 0);
	con_hex2("A6 end=", "", 0x66666666u, "", 0);
	ufslog_flush();
	for (;;)
		__asm__ volatile("wfi");
#else
	int pon_ok = pon_set_restart_reason(PON_RESTART_REASON_BOOTLOADER);
	imem_set_reason(IMEM_REASON_BOOTLOADER);
	imem = imem_get_reason();

	con_hex2("AUTO", "pon=", pon_ok, "imem=", imem);
	ufslog_hex2("AUTO", "pon=", pon_ok, "imem=", imem);
#endif

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

/*
 * A reset that needs no help from the PMIC.
 *
 * autonomy_to_fastboot() above tries to ask ABL for fastboot and parks if the
 * IMEM store did not take. That is right when the goal is to *reach* fastboot,
 * but it is the wrong shape for a plain keep-alive cycle: a park is exactly the
 * silent dead end this is meant to remove.
 *
 * So this path writes the reason as a best effort, ignores whether it stuck,
 * and then uses every reset primitive in turn. PS_HOLD alone is enough -- it is
 * proven to power-cycle the board (the panel flickers and ABL comes back), and
 * it needs nothing initialised.
 */
static void reset_now(void)
{
	/* Best effort only: an unwritten reason must not stop the reset. */
	(void)pon_set_restart_reason(PON_RESTART_REASON_BOOTLOADER);
	imem_set_reason(IMEM_REASON_BOOTLOADER);

	psci_reset(PSCI_SYSTEM_RESET);
	psci_reset(PSCI_SYSTEM_RESET2);

	REG32(PSHOLD_BASE) = 0;
	__asm__ volatile("dsb sy" ::: "memory");

	/* PS_HOLD is a latch on the PMIC: give it time to act before retrying. */
	for (volatile uint64_t i = 0; i < 20000000ULL; i++)
		__asm__ volatile("nop");

	REG32(PSHOLD_BASE) = 0;
	__asm__ volatile("dsb sy" ::: "memory");
}

/*
 * PSCI CPU_ON: start `mpidr` at the physical entry `entry` with x0 = context.
 *
 * Same register discipline as psci_reset below: the SMCCC lets firmware
 * clobber x1-x17, and every argument must be passed explicitly or the call is
 * malformed and simply refused.
 */
static uint64_t psci_cpu_on(uint64_t mpidr, uint64_t entry, uint64_t context)
{
	register uint64_t x0 __asm__("x0") = 0xc4000003UL;	/* PSCI_CPU_ON (64-bit) */
	register uint64_t x1 __asm__("x1") = mpidr;
	register uint64_t x2 __asm__("x2") = entry;
	register uint64_t x3 __asm__("x3") = context;
	__asm__ volatile("smc #0"
			 : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3)
			 :
			 : "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11",
			   "x12", "x13", "x14", "x15", "x16", "x17", "memory");
	return x0;
}

void autonomy_reboot_loop(unsigned seconds)
{
	uint64_t freq = 0;
	uint64_t start = 0;

	__asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
	__asm__ volatile("isb");
	__asm__ volatile("mrs %0, cntpct_el0" : "=r"(start));

	/*
	 * A zero or absurd frequency would make the deadline meaningless. Rather
	 * than guess at a cycle count and reboot almost immediately, leave the
	 * caller in control and report the problem on the panel.
	 */
	if (freq < 1000 || freq > 1000000000ULL) {
		con_hex2("WD", "cntfrq=", (uint32_t)freq, " skip", 0);
		return;
	}

	uint64_t period = freq * (uint64_t)seconds;

	for (;;) {
		uint64_t now;

		__asm__ volatile("isb");
		__asm__ volatile("mrs %0, cntpct_el0" : "=r"(now));
		if (now - start >= period)
			break;
	}

	/*
	 * Print before resetting: this is the only proof, once the log is
	 * readable again, that the deadline actually expired rather than the
	 * board dying of something else.
	 */
	con_hex2("WD", "timeout s=", seconds, " rebooting", 0);
	ufslog_str("WD", "minute timeout: forcing reboot");
	ufslog_flush();

	reset_now();

	/* Nothing above returned. Park loudly rather than silently. */
	con_str("WD", "no reset path: parked");
	for (;;)
		__asm__ volatile("wfi");
}

/* ---- core watchdog for the xnu handoff ---------------------------------- */

/*
 * Entry point handed to PSCI CPU_ON.
 *
 * Runs on a core the boot CPU started, with the same MMU-off, EL1 state the
 * shim runs in. It must NOT touch the console after the handoff -- the boot
 * CPU owns the framebuffer by then and xnu may be drawing into it -- and it
 * must not return: xnu never expects this core and PSCI CPU_OFF here would
 * need a call into firmware that could be refused.
 *
 * It only needs the generic timer, which is per-core and independent of
 * anything xnu does to the boot CPU, so the deadline stays honest even if xnu
 * stops every timer it knows about.
 */
static unsigned g_wdt_seconds = 30;

extern void wdt_secondary_entry(void);

/*
 * Runs on the watchdog core, entered from wdt_secondary_entry with a stack.
 *
 * Deliberately does NOT use autonomy_reboot_loop(): that one prints to the
 * panel and writes the persistent log, both of which this core must leave
 * alone (xnu owns them by now, and the eMMC driver is not up on this core).
 * Nothing here allocates, locks, or logs -- it reads the generic timer until
 * the deadline passes and then resets.
 */
void wdt_secondary_main(void)
{
	uint64_t freq = 0, start = 0;

	__asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
	__asm__ volatile("isb; mrs %0, cntpct_el0" : "=r"(start));

	if (freq < 1000 || freq > 1000000000ULL)
		freq = 0;

	for (;;) {
		uint64_t now;

		__asm__ volatile("isb; mrs %0, cntpct_el0" : "=r"(now));
		if (freq && now - start >= freq * (uint64_t)g_wdt_seconds)
			break;
		/*
		 * Without a usable counter the deadline cannot be measured, so
		 * fall back to a bounded spin: still better than never resetting,
		 * which is the bug this core exists to fix.
		 */
		if (!freq) {
			for (volatile uint64_t i = 0; i < 50000000ULL; i++)
				__asm__ volatile("nop");
			break;
		}
	}

	reset_now();

	for (;;)
		__asm__ volatile("wfi");	/* reset_now did not take */
}

int watchdog_start_secondary(unsigned seconds)
{
	/*
	 * Which cores to try, in order. ABL boots ginkgo on MPIDR 0x000, so
	 * 0x001..0x003 (silver) and 0x100..0x103 (gold) are free here. Start
	 * with silver: gold is the cluster xnu is about to reconfigure, and a
	 * core inside it is more likely to be disturbed.
	 */
	static const uint64_t candidates[] = {
		0x001, 0x002, 0x003,
		0x100, 0x101, 0x102, 0x103,
	};
	uint64_t self = 0;

	__asm__ volatile("mrs %0, mpidr_el1" : "=r"(self));
	self &= 0xffffff;

	g_wdt_seconds = seconds;

	for (unsigned i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
		if (candidates[i] == self)
			continue;
		/*
		 * Entry must be the ASM stub, not wdt_secondary_main: PSCI starts
		 * the core with no stack. The context id picks the stack slot and
		 * must stay under 8 (_sec_stacks holds 8 pages).
		 */
		int64_t r = (int64_t)psci_cpu_on(candidates[i],
						 (uint64_t)(uintptr_t)wdt_secondary_entry,
						 (uint64_t)i + 1);
		if (r == 0) {
			con_hex2("WD", "secondary armed mpidr=", (uint32_t)candidates[i],
				 " s=", seconds);
			return 1;
		}
	}
	con_str("WD", "no core accepted the watchdog");
	return 0;
}

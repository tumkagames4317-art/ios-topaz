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
 * spmi-pmic-arb.c: pmic_arb_read_apid_map_v5() takes one entry per APID from
 *     core + 0x900 + 4*n
 * and reads the PPID from bits [19:8]. The PPID is (sid << 8) | peripheral,
 * so PON on SID 0 is 0x0800.
 *
 * A fixed guess is not good enough: a channel belonging to another peripheral
 * is DENIED by the arbiter, or worse, the write lands somewhere harmless and
 * looks like a silent no-op -- which is exactly what every attempt so far
 * looked like on the panel.
 *
 * Returns the APID, or -1 when the PPID is not mapped.
 */
static int spmi_find_apid(uint16_t ppid)
{
	for (int n = 0; n < 512; n++) {
		uint32_t reg = REG32(SPMI_TBL_BASE + 4UL * (uint32_t)n);

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
 * Command-channel access in the v1-compatible layout.
 *
 * The arbiter reports version 0x50010000 but answers on the v1 channel at
 * core + 0x800 (its STATUS and WDATA0 read back sensibly), while the v5
 * wr_base at 0x1e00000 stalls the bus outright. So the v1 protocol is what
 * this firmware actually exposes, and it is the only way in.
 */
#define SPMI_V1_CH	(SPMI_CORE_BASE + 0x800UL)

#define SPMI_OP_READ	15u

/*
 * One transaction. `write` selects the opcode; the value to store comes from
 * *val, and a read result is returned through it.
 *
 * The command word is (opc << 27) | ((sid & 0xf) << 20) | (addr << 4) | (bc & 7)
 * where addr is the 16-bit (peripheral << 8) | offset.
 */
static int spmi_v1_xfer(int write, uint8_t sid, uint16_t addr, uint32_t *val)
{
	uintptr_t ch = SPMI_V1_CH;

	/* Clear any stale completion before starting. */
	REG32(ch + 0x08) = 0;
	__asm__ volatile("dsb sy" ::: "memory");

	if (write)
		REG32(ch + 0x10) = *val;

	REG32(ch + 0x00) = ((write ? 14u : SPMI_OP_READ) << 27) |
			   (((uint32_t)sid & 0xFu) << 20) |
			   (((uint32_t)addr & 0xFFFFu) << 4) |
			   0u;
	__asm__ volatile("dsb sy" ::: "memory");

	for (int i = 0; i < 100000; i++) {
		uint32_t st = REG32(ch + 0x08);

		if (!(st & 1u))
			continue;
		if (st & 0x6u)
			return 0;
		if (!write)
			*val = REG32(ch + 0x18) & 0xFFu;
		return 1;
	}
	return 0;
}

static int spmi_v1_read(uint8_t sid, uint16_t addr, uint32_t *out)
{
	*out = 0;
	return spmi_v1_xfer(0, sid, addr, out);
}

static int spmi_write_byte(uint8_t sid, uint16_t addr, uint8_t val)
{
	int apid = spmi_find_apid((uint16_t)(((uint16_t)sid << 8) | (addr >> 8)));
	int code = g_spmi_code;

	/*
	 * Prefer the channel that brute force confirmed during the probe. The
	 * table lookup alone never finds the PON peripheral: the word order in
	 * this firmware does not follow the positions spmi-pmic-arb.c assumes.
	 */
	if (code < 0) {
		if (apid < 0)
			return 0;
		code = apid;
	}

	uintptr_t base = (code & 0x10000) ? SPMI_CHNLS_BASE : SPMI_CORE_BASE;
	uintptr_t ch = base + 0x10000UL * (uint32_t)(code & 0xFFFF);

	REG32(ch + SPMI_ARB_WDATA0) = val;
	REG32(ch + SPMI_ARB_CMD) = (SPMI_OP_WRITE << 27) |
				   (((uint32_t)addr & 0xFFu) << 4);
	__asm__ volatile("dsb sy" ::: "memory");

	for (int i = 0; i < 100000; i++) {
		uint32_t st = REG32(ch + SPMI_ARB_STATUS);

		if (!(st & SPMI_ARB_STATUS_DONE))
			continue;
		if (st & (SPMI_ARB_STATUS_FAIL | SPMI_ARB_STATUS_DENIED))
			return 0;
		return 1;
	}
	return 0;
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
	int ap = spmi_find_apid(0x0800);
	con_hex2("PONAP", "apon=", (uint32_t)ap, "nz="
		 , cnt_nz());
	/*
	 * The arbiter answers in its v1-compatible layout, not the v5 one.
	 *
	 * Reading core+0x08 gives 0x00000000 (STATUS, idle) and core+0x10 gives
	 * 0x00001043 -- those are the per-EE channel's STATUS and WDATA0, the
	 * same registers spmi-pmic-arb.c uses when it treats the arbiter as v1
	 * (wr_base == rd_base == core). chnls (0x1e00000) stalls the bus, so the
	 * v5 wr_base is unusable here.
	 *
	 * That also explains a long-standing illusion: reading core+0x800+0x10
	 * and calling it PON_RT_STS was reading WDATA0 of the channel, which is
	 * zero whenever no transaction is in flight. It looked like a live PMIC
	 * read because zero is a plausible status value.
	 *
	 * v1 channel for EE 0 lives at core + 0x800, with
	 *   CMD +0x00  CONFIG +0x04  STATUS +0x08  WDATA0 +0x10  RDATA0 +0x18
	 * and the command word is
	 *   (opc << 27) | ((sid & 0xf) << 20) | (addr << 4) | (bc & 0x7)
	 */
	con_hex2("Q1 st=", "", REG32(SPMI_V1_CH + 0x08), " cfg=",
		 REG32(SPMI_V1_CH + 0x04));
	con_hex2("Q2 cmd=", "", REG32(SPMI_V1_CH + 0x00), " rd=",
		 REG32(SPMI_V1_CH + 0x18));

	/* Read PON_RT_STS (sid 0, addr 0x0810) properly through the channel. */
	int rok = spmi_v1_read(0, 0x0810, &g_v1_val);
	con_hex2("Q3 rok=", "", (uint32_t)rok, " val=", g_v1_val);

	/* Read the spare byte ABL uses for the restart reason. */
	int rok2 = spmi_v1_read(0, 0x088F, &g_v1_val2);
	con_hex2("Q4 rok2=", "", (uint32_t)rok2, " spare=", g_v1_val2);
	con_hex2("Q5 done", "", 5u, "", 0);
	ufslog_flush();
	for (;;)
		__asm__ volatile("wfi");
	int wr = spmi_write_byte(0, 0x0800 | 0x8F, 0x12);
	con_hex2("PONWR", "ok=", (uint32_t)wr, "rb="
		 , REG8(QPNP_PON_SOFT_RB_SPARE));
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
